/*
 * vga_switcheroo.c - Support for laptop with dual GPU using one set of outputs
 *
 * Copyright (c) 2010 Red Hat Inc.
 * Author : Dave Airlie <airlied@redhat.com>
 *
 * Copyright (c) 2015 Lukas Wunner <lukas@wunner.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <sys/param.h>
#include <sys/kernel.h>
#include <sys/bio.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/conf.h>
#include <sys/rman.h>

#define pr_fmt(fmt) "vga_switcheroo: " fmt

//#include <linux/console.h>
//#include <linux/debugfs.h>
#include <linux/fb.h>					/* exists: struct fb_info */
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/pci.h>				/* exists: struct pci_dev */
//#include <linux/pm_domain.h>
//#include <linux/pm_runtime.h>
//#include <linux/seq_file.h>		/* exists: struct seq_file */
//#include <linux/uaccess.h>
//#include <linux/vgaarb.h>
#include <linux/vga_switcheroo.h>

/* DragonFly */
#include <linux/pci_ids.h>		/* PCI_VENDOR_ID_INTEL, PCI_VENDOR_ID_NVIDIA */
#include <linux/mutex.h>			/* DEFINE_MUTEX */
#include <linux/list.h>				/* LIST_HEAD_INIT -> INIT_LIST_HEAD */
#include <linux/export.h>			/* EXPORT_SYMBOL */
//#include <linux/slab.h>				/* kzalloc(), kfree() */

/**
 * DOC: Overview
 *
 * vga_switcheroo is the Linux subsystem for laptop hybrid graphics.
 * These come in two flavors:
 *
 * * muxed: Dual GPUs with a multiplexer chip to switch outputs between GPUs.
 * * muxless: Dual GPUs but only one of them is connected to outputs.
 * 	The other one is merely used to offload rendering, its results
 * 	are copied over PCIe into the framebuffer. On Linux this is
 * 	supported with DRI PRIME.
 *
 * Hybrid graphics started to appear in the late Naughties and were initially
 * all muxed. Newer laptops moved to a muxless architecture for cost reasons.
 * A notable exception is the MacBook Pro which continues to use a mux.
 * Muxes come with varying capabilities: Some switch only the panel, others
 * can also switch external displays. Some switch all display pins at once
 * while others can switch just the DDC lines. (To allow EDID probing
 * for the inactive GPU.) Also, muxes are often used to cut power to the
 * discrete GPU while it is not used.
 *
 * DRM drivers register GPUs with vga_switcheroo, these are henceforth called
 * clients. The mux is called the handler. Muxless machines also register a
 * handler to control the power state of the discrete GPU, its ->switchto
 * callback is a no-op for obvious reasons. The discrete GPU is often equipped
 * with an HDA controller for the HDMI/DP audio signal, this will also
 * register as a client so that vga_switcheroo can take care of the correct
 * suspend/resume order when changing the discrete GPU's power state. In total
 * there can thus be up to three clients: Two vga clients (GPUs) and one audio
 * client (on the discrete GPU). The code is mostly prepared to support
 * machines with more than two GPUs should they become available.
 *
 * The GPU to which the outputs are currently switched is called the
 * active client in vga_switcheroo parlance. The GPU not in use is the
 * inactive client. When the inactive client's DRM driver is loaded,
 * it will be unable to probe the panel's EDID and hence depends on
 * VBIOS to provide its display modes. If the VBIOS modes are bogus or
 * if there is no VBIOS at all (which is common on the MacBook Pro),
 * a client may alternatively request that the DDC lines are temporarily
 * switched to it, provided that the handler supports this. Switching
 * only the DDC lines and not the entire output avoids unnecessary
 * flickering.
 */

/**
 * struct vga_switcheroo_client - registered client
 * @pdev: client pci device
 * @fb_info: framebuffer to which console is remapped on switching
 * @pwr_state: current power state
 * @ops: client callbacks
 * @id: client identifier. Determining the id requires the handler,
 * 	so gpus are initially assigned VGA_SWITCHEROO_UNKNOWN_ID
 * 	and later given their true id in vga_switcheroo_enable()
 * @active: whether the outputs are currently switched to this client
 * @driver_power_control: whether power state is controlled by the driver's
 * 	runtime pm. If true, writing ON and OFF to the vga_switcheroo debugfs
 * 	interface is a no-op so as not to interfere with runtime pm
 * @list: client list
 *
 * Registered client. A client can be either a GPU or an audio device on a GPU.
 * For audio clients, the @fb_info, @active and @driver_power_control members
 * are bogus.
 */
struct vga_switcheroo_client {
	struct pci_dev *pdev;
	struct fb_info *fb_info;
	enum vga_switcheroo_state pwr_state;
	const struct vga_switcheroo_client_ops *ops;
	enum vga_switcheroo_client_id id;
	bool active;
	bool driver_power_control;
	struct list_head list;
};

/*
 * protects access to struct vgasr_priv
 */
static DEFINE_MUTEX(vgasr_mutex);

/**
 * struct vgasr_priv - vga_switcheroo private data
 * @active: whether vga_switcheroo is enabled.
 * 	Prerequisite is the registration of two GPUs and a handler
 * @delayed_switch_active: whether a delayed switch is pending
 * @delayed_client_id: client to which a delayed switch is pending
 * @debugfs_root: directory for vga_switcheroo debugfs interface
 * @switch_file: file for vga_switcheroo debugfs interface
 * @registered_clients: number of registered GPUs
 * 	(counting only vga clients, not audio clients)
 * @clients: list of registered clients
 * @handler: registered handler
 * @handler_flags: flags of registered handler
 * @mux_hw_lock: protects mux state
 *	(in particular while DDC lines are temporarily switched)
 * @old_ddc_owner: client to which DDC lines will be switched back on unlock
 *
 * vga_switcheroo private data. Currently only one vga_switcheroo instance
 * per system is supported.
 */
struct vgasr_priv {
	bool active;
	bool delayed_switch_active;
	enum vga_switcheroo_client_id delayed_client_id;

	struct dentry *debugfs_root;
	struct dentry *switch_file;

	int registered_clients;
	struct list_head clients;

	const struct vga_switcheroo_handler *handler;
	enum vga_switcheroo_handler_flags_t handler_flags;

	//struct mutex mux_hw_lock;
	//DFly:
	struct lock mux_hw_lk;

	int old_ddc_owner;
};

#define ID_BIT_AUDIO		0x100
#define client_is_audio(c)	((c)->id & ID_BIT_AUDIO)
#define client_is_vga(c)	((c)->id == VGA_SWITCHEROO_UNKNOWN_ID || \
				 !client_is_audio(c))
#define client_id(c)		((c)->id & ~ID_BIT_AUDIO)

#if 0
static int vga_switcheroo_debugfs_init(struct vgasr_priv *priv);
static void vga_switcheroo_debugfs_fini(struct vgasr_priv *priv);
#endif

/* only one switcheroo per system */
static struct vgasr_priv *vgasr_priv;

static bool
vga_switcheroo_ready(void)
{
	/* we're ready if we get two clients + handler */
	return !vgasr_priv->active &&
	       vgasr_priv->registered_clients == 2 && vgasr_priv->handler;
}

static void
vga_switcheroo_enable(void)
{
	int ret;
	struct vga_switcheroo_client *client;

	/* call the handler to init */
	if (vgasr_priv->handler->init)
		vgasr_priv->handler->init();

	list_for_each_entry(client, &vgasr_priv->clients, list) {
		if (client->id != VGA_SWITCHEROO_UNKNOWN_ID)
			continue;
		ret = vgasr_priv->handler->get_client_id(client->pdev);
		if (ret < 0)
			return;

		client->id = ret;
	}
	//vga_switcheroo_debugfs_init(&vgasr_priv);
	vgasr_priv->active = true;
}

/**
 * vga_switcheroo_register_handler() - register handler
 * @handler: handler callbacks
 * @handler_flags: handler flags
 *
 * Register handler. Enable vga_switcheroo if two vga clients have already
 * registered.
 *
 * Return: 0 on success, -EINVAL if a handler was already registered.
 */
int
vga_switcheroo_register_handler(const struct vga_switcheroo_handler *handler,
				    enum vga_switcheroo_handler_flags_t handler_flags)
{
	mutex_lock(&vgasr_mutex);
	if (vgasr_priv->handler) {
		mutex_unlock(&vgasr_mutex);
		return -EINVAL;
	}

	vgasr_priv->handler = handler;
	vgasr_priv->handler_flags = handler_flags;
	if (vga_switcheroo_ready()) {
		pr_info("enabled\n");
		vga_switcheroo_enable();
	}
	mutex_unlock(&vgasr_mutex);
	//DEBUG
	pr_info("vga_switcheroo: registered handler\n");
	return 0;
}
EXPORT_SYMBOL(vga_switcheroo_register_handler);

/**
 * vga_switcheroo_unregister_handler() - unregister handler
 *
 * Unregister handler. Disable vga_switcheroo.
 */
void
vga_switcheroo_unregister_handler(void)
{
	mutex_lock(&vgasr_mutex);
	mutex_lock(&vgasr_priv->mux_hw_lk);
	vgasr_priv->handler_flags = 0;
	vgasr_priv->handler = NULL;
	if (vgasr_priv->active) {
		pr_info("disabled\n");
		//XXX TODO:
		//vga_switcheroo_debugfs_fini(&vgasr_priv);
		vgasr_priv->active = false;
	}
	mutex_unlock(&vgasr_priv->mux_hw_lk);
	mutex_unlock(&vgasr_mutex);
	//DEBUG
	pr_info("vga_switcheroo: unregistered handler\n");
}
EXPORT_SYMBOL(vga_switcheroo_unregister_handler);

/**
 * vga_switcheroo_handler_flags() - obtain handler flags
 *
 * Helper for clients to obtain the handler flags bitmask.
 *
 * Return: Handler flags. A value of 0 means that no handler is registered
 * or that the handler has no special capabilities.
 */
enum vga_switcheroo_handler_flags_t
vga_switcheroo_handler_flags(void)
{
	return vgasr_priv->handler_flags;
}
EXPORT_SYMBOL(vga_switcheroo_handler_flags);

MALLOC_DECLARE(M_VGA_SWITCHEROO_CLIENT);
MALLOC_DEFINE(M_VGA_SWITCHEROO_CLIENT, "vga_switcheroo_client", "vga_switcheroo client data");

static int
register_client(struct pci_dev *pdev,
			   const struct vga_switcheroo_client_ops *ops,
			   enum vga_switcheroo_client_id id, bool active,
			   bool driver_power_control)
{
	struct vga_switcheroo_client *client;

	//client = kzalloc(sizeof(*client), GFP_KERNEL);
	client = kmalloc(sizeof(*client), M_VGA_SWITCHEROO_CLIENT, M_WAITOK | M_ZERO);
	if (!client)
		return -ENOMEM;

	client->pwr_state = VGA_SWITCHEROO_ON;
	client->pdev = pdev;
	client->ops = ops;
	client->id = id;
	client->active = active;
	client->driver_power_control = driver_power_control;

	mutex_lock(&vgasr_mutex);
	list_add_tail(&client->list, &vgasr_priv->clients);
	if (client_is_vga(client))
		vgasr_priv->registered_clients++;

	if (vga_switcheroo_ready()) {
		pr_info("enabled\n");
		vga_switcheroo_enable();
	}
	mutex_unlock(&vgasr_mutex);
	return 0;
}

/**
 * vga_switcheroo_register_client - register vga client
 * @pdev: client pci device
 * @ops: client callbacks
 * @driver_power_control: whether power state is controlled by the driver's
 * 	runtime pm
 *
 * Register vga client (GPU). Enable vga_switcheroo if another GPU and a
 * handler have already registered. The power state of the client is assumed
 * to be ON.
 *
 * Return: 0 on success, -ENOMEM on memory allocation error.
 */
int
vga_switcheroo_register_client(struct pci_dev *pdev,
				   const struct vga_switcheroo_client_ops *ops,
				   bool driver_power_control)
{
	int error = 0;
	int unit, default_vgapci_unit;
	size_t len;

	// get the device unit
	unit = device_get_unit(pdev->dev.bsddev);

	// get the default unit
	len = sizeof(default_vgapci_unit);
	error = kernel_sysctlbyname("hw.pci.default_vgapci_unit", &default_vgapci_unit, &len, NULL, 0, NULL);
	if (error) {
		pr_err("kernel_sysctlbyname: error %d\n", error);
		return (error);
	}

	return register_client(pdev, ops, VGA_SWITCHEROO_UNKNOWN_ID,
			       unit == default_vgapci_unit,
			       driver_power_control);
}
EXPORT_SYMBOL(vga_switcheroo_register_client);

/**
 * vga_switcheroo_register_audio_client - register audio client
 * @pdev: client pci device
 * @ops: client callbacks
 * @id: client identifier
 *
 * Register audio client (audio device on a GPU). The power state of the
 * client is assumed to be ON.
 *
 * Return: 0 on success, -ENOMEM on memory allocation error.
 */
int
vga_switcheroo_register_audio_client(struct pci_dev *pdev,
					 const struct vga_switcheroo_client_ops *ops,
					 enum vga_switcheroo_client_id id)
{
	return register_client(pdev, ops, id | ID_BIT_AUDIO, false, false);
}
EXPORT_SYMBOL(vga_switcheroo_register_audio_client);

static struct vga_switcheroo_client *
find_client_from_pci(struct list_head *head, struct pci_dev *pdev)
{
	struct vga_switcheroo_client *client;

	list_for_each_entry(client, head, list)
		if (client->pdev == pdev)
			return client;
	return NULL;
}

static struct vga_switcheroo_client *
find_client_from_id(struct list_head *head,
		    enum vga_switcheroo_client_id client_id)
{
	struct vga_switcheroo_client *client;

	list_for_each_entry(client, head, list)
		if (client->id == client_id)
			return client;
	return NULL;
}

static struct vga_switcheroo_client *
find_active_client(struct list_head *head)
{
	struct vga_switcheroo_client *client;

	list_for_each_entry(client, head, list)
		if (client->active)
			return client;
	return NULL;
}

/**
 * vga_switcheroo_get_client_state() - obtain power state of a given client
 * @pdev: client pci device
 *
 * Obtain power state of a given client as seen from vga_switcheroo.
 * The function is only called from hda_intel.c.
 *
 * Return: Power state.
 */
enum vga_switcheroo_state
vga_switcheroo_get_client_state(struct pci_dev *pdev)
{
	struct vga_switcheroo_client *client;
	enum vga_switcheroo_state ret;

	mutex_lock(&vgasr_mutex);
	client = find_client_from_pci(&vgasr_priv->clients, pdev);
	if (!client)
		ret = VGA_SWITCHEROO_NOT_FOUND;
	else
		ret = client->pwr_state;
	mutex_unlock(&vgasr_mutex);
	return ret;
}
EXPORT_SYMBOL(vga_switcheroo_get_client_state);

/**
 * vga_switcheroo_unregister_client() - unregister client
 * @pdev: client pci device
 *
 * Unregister client. Disable vga_switcheroo if this is a vga client (GPU).
 */
void
vga_switcheroo_unregister_client(struct pci_dev *pdev)
{
	struct vga_switcheroo_client *client;

	mutex_lock(&vgasr_mutex);
	client = find_client_from_pci(&vgasr_priv->clients, pdev);
	if (client) {
		if (client_is_vga(client))
			vgasr_priv->registered_clients--;
		list_del(&client->list);
		//kfree(client);
		kfree(client, M_VGA_SWITCHEROO_CLIENT);
	}
	if (vgasr_priv->active && vgasr_priv->registered_clients < 2) {
		pr_info("disabled\n");
		//vga_switcheroo_debugfs_fini(&vgasr_priv);
		vgasr_priv->active = false;
	}
	mutex_unlock(&vgasr_mutex);
}
EXPORT_SYMBOL(vga_switcheroo_unregister_client);

/**
 * vga_switcheroo_client_fb_set() - set framebuffer of a given client
 * @pdev: client pci device
 * @info: framebuffer
 *
 * Set framebuffer of a given client. The console will be remapped to this
 * on switching.
 */
void
vga_switcheroo_client_fb_set(struct pci_dev *pdev,
				 struct fb_info *info)
{
	struct vga_switcheroo_client *client;

	mutex_lock(&vgasr_mutex);
	client = find_client_from_pci(&vgasr_priv->clients, pdev);
	if (client)
		client->fb_info = info;
	mutex_unlock(&vgasr_mutex);
}
EXPORT_SYMBOL(vga_switcheroo_client_fb_set);

/**
 * vga_switcheroo_lock_ddc() - temporarily switch DDC lines to a given client
 * @pdev: client pci device
 *
 * Temporarily switch DDC lines to the client identified by @pdev
 * (but leave the outputs otherwise switched to where they are).
 * This allows the inactive client to probe EDID. The DDC lines must
 * afterwards be switched back by calling vga_switcheroo_unlock_ddc(),
 * even if this function returns an error.
 *
 * Return: Previous DDC owner on success or a negative int on error.
 * Specifically, %-ENODEV if no handler has registered or if the handler
 * does not support switching the DDC lines. Also, a negative value
 * returned by the handler is propagated back to the caller.
 * The return value has merely an informational purpose for any caller
 * which might be interested in it. It is acceptable to ignore the return
 * value and simply rely on the result of the subsequent EDID probe,
 * which will be %NULL if DDC switching failed.
 */
int
vga_switcheroo_lock_ddc(struct pci_dev *pdev)
{
	enum vga_switcheroo_client_id id;

	mutex_lock(&vgasr_priv->mux_hw_lk);
	if (!vgasr_priv->handler || !vgasr_priv->handler->switch_ddc) {
		vgasr_priv->old_ddc_owner = -ENODEV;
		return -ENODEV;
	}

	id = vgasr_priv->handler->get_client_id(pdev);
	vgasr_priv->old_ddc_owner = vgasr_priv->handler->switch_ddc(id);
	return vgasr_priv->old_ddc_owner;
}
EXPORT_SYMBOL(vga_switcheroo_lock_ddc);

/**
 * vga_switcheroo_unlock_ddc() - switch DDC lines back to previous owner
 * @pdev: client pci device
 *
 * Switch DDC lines back to the previous owner after calling
 * vga_switcheroo_lock_ddc(). This must be called even if
 * vga_switcheroo_lock_ddc() returned an error.
 *
 * Return: Previous DDC owner on success (i.e. the client identifier of @pdev)
 * or a negative int on error.
 * Specifically, %-ENODEV if no handler has registered or if the handler
 * does not support switching the DDC lines. Also, a negative value
 * returned by the handler is propagated back to the caller.
 * Finally, invoking this function without calling vga_switcheroo_lock_ddc()
 * first is not allowed and will result in %-EINVAL.
 */
int
vga_switcheroo_unlock_ddc(struct pci_dev *pdev)
{
	enum vga_switcheroo_client_id id;
	int ret = vgasr_priv->old_ddc_owner;

	if (WARN_ON_ONCE(!mutex_is_locked(&vgasr_priv->mux_hw_lk)))
		return -EINVAL;

	if (vgasr_priv->old_ddc_owner >= 0) {
		id = vgasr_priv->handler->get_client_id(pdev);
		if (vgasr_priv->old_ddc_owner != id)
			ret = vgasr_priv->handler->switch_ddc(
						     vgasr_priv->old_ddc_owner);
	}
	mutex_unlock(&vgasr_priv->mux_hw_lk);
	return ret;
}
EXPORT_SYMBOL(vga_switcheroo_unlock_ddc);

/**
 * DOC: Manual switching and manual power control
 *
 * In this mode of use, the file /sys/kernel/debug/vgaswitcheroo/switch
 * can be read to retrieve the current vga_switcheroo state and commands
 * can be written to it to change the state. The file appears as soon as
 * two GPU drivers and one handler have registered with vga_switcheroo.
 * The following commands are understood:
 *
 * * OFF: Power off the device not in use.
 * * ON: Power on the device not in use.
 * * IGD: Switch to the integrated graphics device.
 * 	Power on the integrated GPU if necessary, power off the discrete GPU.
 * 	Prerequisite is that no user space processes (e.g. Xorg, alsactl)
 * 	have opened device files of the GPUs or the audio client. If the
 * 	switch fails, the user may invoke lsof(8) or fuser(1) on /dev/dri/
 * 	and /dev/snd/controlC1 to identify processes blocking the switch.
 * * DIS: Switch to the discrete graphics device.
 * * DIGD: Delayed switch to the integrated graphics device.
 * 	This will perform the switch once the last user space process has
 * 	closed the device files of the GPUs and the audio client.
 * * DDIS: Delayed switch to the discrete graphics device.
 * * MIGD: Mux-only switch to the integrated graphics device.
 * 	Does not remap console or change the power state of either gpu.
 * 	If the integrated GPU is currently off, the screen will turn black.
 * 	If it is on, the screen will show whatever happens to be in VRAM.
 * 	Either way, the user has to blindly enter the command to switch back.
 * * MDIS: Mux-only switch to the discrete graphics device.
 *
 * For GPUs whose power state is controlled by the driver's runtime pm,
 * the ON and OFF commands are a no-op (see next section).
 *
 * For muxless machines, the IGD/DIS, DIGD/DDIS and MIGD/MDIS commands
 * should not be used.
 */

#if 0  /* debugfs 1 */
static int
vga_switcheroo_show(struct seq_file *m, void *v)
{
	struct vga_switcheroo_client *client;
	int i = 0;

	mutex_lock(&vgasr_mutex);
	list_for_each_entry(client, &vgasr_priv.clients, list) {
		seq_printf(m, "%d:%s%s:%c:%s%s:%s\n", i,
			   client_id(client) == VGA_SWITCHEROO_DIS ? "DIS" :
								     "IGD",
			   client_is_vga(client) ? "" : "-Audio",
			   client->active ? '+' : ' ',
			   client->driver_power_control ? "Dyn" : "",
			   client->pwr_state ? "Pwr" : "Off",
			   pci_name(client->pdev));
		i++;
	}
	mutex_unlock(&vgasr_mutex);
	return 0;
}

static int
vga_switcheroo_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, vga_switcheroo_show, NULL);
}

static int
vga_switchon(struct vga_switcheroo_client *client)
{
	if (client->driver_power_control)
		return 0;
	if (vgasr_priv.handler->power_state)
		vgasr_priv.handler->power_state(client->id, VGA_SWITCHEROO_ON);
	/* call the driver callback to turn on device */
	client->ops->set_gpu_state(client->pdev, VGA_SWITCHEROO_ON);
	client->pwr_state = VGA_SWITCHEROO_ON;
	return 0;
}
#endif /* debugfs 1 */

static int
vga_switchoff(struct vga_switcheroo_client *client)
{
	if (client->driver_power_control)
		return 0;
	/* call the driver callback to turn off device */
	client->ops->set_gpu_state(client->pdev, VGA_SWITCHEROO_OFF);
	if (vgasr_priv->handler->power_state)
		vgasr_priv->handler->power_state(client->id, VGA_SWITCHEROO_OFF);
	client->pwr_state = VGA_SWITCHEROO_OFF;
	return 0;
}

static void
set_audio_state(enum vga_switcheroo_client_id id,
			    enum vga_switcheroo_state state)
{
	struct vga_switcheroo_client *client;

	client = find_client_from_id(&vgasr_priv->clients, id | ID_BIT_AUDIO);
	if (client && client->pwr_state != state) {
		client->ops->set_gpu_state(client->pdev, state);
		client->pwr_state = state;
	}
}

#if 0  /* debugfs 1.5 */
/* stage one happens before delay */
static int
vga_switchto_stage1(struct vga_switcheroo_client *new_client)
{
	struct vga_switcheroo_client *active;

	active = find_active_client(&vgasr_priv.clients);
	if (!active)
		return 0;

	if (new_client->pwr_state == VGA_SWITCHEROO_OFF)
		vga_switchon(new_client);

	vga_set_default_device(new_client->pdev);
	return 0;
}
#endif  /* debugfs 1.5 */

/* post delay */
static int
vga_switchto_stage2(struct vga_switcheroo_client *new_client)
{
	int ret;
	struct vga_switcheroo_client *active;

	active = find_active_client(&vgasr_priv->clients);
	if (!active)
		return 0;

	active->active = false;

	set_audio_state(active->id, VGA_SWITCHEROO_OFF);

	/* XXX CRITICAL TODO
	if (new_client->fb_info) {
		struct fb_event event;

		console_lock();
		event.info = new_client->fb_info;
		fb_notifier_call_chain(FB_EVENT_REMAP_ALL_CONSOLE, &event);
		console_unlock();
	}
	*/

	mutex_lock(&vgasr_priv->mux_hw_lk);
	ret = vgasr_priv->handler->switchto(new_client->id);
	mutex_unlock(&vgasr_priv->mux_hw_lk);
	if (ret)
		return ret;

	if (new_client->ops->reprobe)
		new_client->ops->reprobe(new_client->pdev);

	if (active->pwr_state == VGA_SWITCHEROO_ON)
		vga_switchoff(active);

	set_audio_state(new_client->id, VGA_SWITCHEROO_ON);

	new_client->active = true;
	return 0;
}

static bool
check_can_switch(void)
{
	struct vga_switcheroo_client *client;

	list_for_each_entry(client, &vgasr_priv->clients, list) {
		if (!client->ops->can_switch(client->pdev)) {
			pr_err("client %x refused switch\n", client->id);
			return false;
		}
	}
	return true;
}

#if 0  /* debugfs 2 */
static ssize_t
vga_switcheroo_debugfs_write(struct file *filp, const char __user *ubuf,
			     size_t cnt, loff_t *ppos)
{
	char usercmd[64];
	int ret;
	bool delay = false, can_switch;
	bool just_mux = false;
	enum vga_switcheroo_client_id client_id = VGA_SWITCHEROO_UNKNOWN_ID;
	struct vga_switcheroo_client *client = NULL;

	if (cnt > 63)
		cnt = 63;

	if (copy_from_user(usercmd, ubuf, cnt))
		return -EFAULT;

	mutex_lock(&vgasr_mutex);

	if (!vgasr_priv.active) {
		cnt = -EINVAL;
		goto out;
	}

	/* pwr off the device not in use */
	if (strncmp(usercmd, "OFF", 3) == 0) {
		list_for_each_entry(client, &vgasr_priv.clients, list) {
			if (client->active || client_is_audio(client))
				continue;
			if (client->driver_power_control)
				continue;
			set_audio_state(client->id, VGA_SWITCHEROO_OFF);
			if (client->pwr_state == VGA_SWITCHEROO_ON)
				vga_switchoff(client);
		}
		goto out;
	}
	/* pwr on the device not in use */
	if (strncmp(usercmd, "ON", 2) == 0) {
		list_for_each_entry(client, &vgasr_priv.clients, list) {
			if (client->active || client_is_audio(client))
				continue;
			if (client->driver_power_control)
				continue;
			if (client->pwr_state == VGA_SWITCHEROO_OFF)
				vga_switchon(client);
			set_audio_state(client->id, VGA_SWITCHEROO_ON);
		}
		goto out;
	}

	/* request a delayed switch - test can we switch now */
	if (strncmp(usercmd, "DIGD", 4) == 0) {
		client_id = VGA_SWITCHEROO_IGD;
		delay = true;
	}

	if (strncmp(usercmd, "DDIS", 4) == 0) {
		client_id = VGA_SWITCHEROO_DIS;
		delay = true;
	}

	if (strncmp(usercmd, "IGD", 3) == 0)
		client_id = VGA_SWITCHEROO_IGD;

	if (strncmp(usercmd, "DIS", 3) == 0)
		client_id = VGA_SWITCHEROO_DIS;

	if (strncmp(usercmd, "MIGD", 4) == 0) {
		just_mux = true;
		client_id = VGA_SWITCHEROO_IGD;
	}
	if (strncmp(usercmd, "MDIS", 4) == 0) {
		just_mux = true;
		client_id = VGA_SWITCHEROO_DIS;
	}

	if (client_id == VGA_SWITCHEROO_UNKNOWN_ID)
		goto out;
	client = find_client_from_id(&vgasr_priv.clients, client_id);
	if (!client)
		goto out;

	vgasr_priv.delayed_switch_active = false;

	if (just_mux) {
		mutex_lock(&vgasr_priv.mux_hw_lock);
		ret = vgasr_priv.handler->switchto(client_id);
		mutex_unlock(&vgasr_priv.mux_hw_lock);
		goto out;
	}

	if (client->active)
		goto out;

	/* okay we want a switch - test if devices are willing to switch */
	can_switch = check_can_switch();

	if (can_switch == false && delay == false)
		goto out;

	if (can_switch) {
		ret = vga_switchto_stage1(client);
		if (ret)
			pr_err("switching failed stage 1 %d\n", ret);

		ret = vga_switchto_stage2(client);
		if (ret)
			pr_err("switching failed stage 2 %d\n", ret);

	} else {
		pr_info("setting delayed switch to client %d\n", client->id);
		vgasr_priv.delayed_switch_active = true;
		vgasr_priv.delayed_client_id = client_id;

		ret = vga_switchto_stage1(client);
		if (ret)
			pr_err("delayed switching stage 1 failed %d\n", ret);
	}

out:
	mutex_unlock(&vgasr_mutex);
	return cnt;
}

#if 0  /* linux specific */
static const struct file_operations vga_switcheroo_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = vga_switcheroo_debugfs_open,
	.write = vga_switcheroo_debugfs_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static void
vga_switcheroo_debugfs_fini(struct vgasr_priv *priv)
{
	debugfs_remove(priv->switch_file);
	priv->switch_file = NULL;

	debugfs_remove(priv->debugfs_root);
	priv->debugfs_root = NULL;
}

static int
vga_switcheroo_debugfs_init(struct vgasr_priv *priv)
{
	static const char mp[] = "/sys/kernel/debug";

	/* already initialised */
	if (priv->debugfs_root)
		return 0;
	priv->debugfs_root = debugfs_create_dir("vgaswitcheroo", NULL);

	if (!priv->debugfs_root) {
		pr_err("Cannot create %s/vgaswitcheroo\n", mp);
		goto fail;
	}

	priv->switch_file = debugfs_create_file("switch", 0644,
						priv->debugfs_root, NULL,
						&vga_switcheroo_debugfs_fops);
	if (!priv->switch_file) {
		pr_err("cannot create %s/vgaswitcheroo/switch\n", mp);
		goto fail;
	}
	return 0;
fail:
	vga_switcheroo_debugfs_fini(priv);
	return -1;
}
#endif  /* debugfs 2 */

/*
 * Device infrastructure
 */
#define BUFFERSIZE 255

static d_open_t      vga_switcheroo_open;
static d_close_t     vga_switcheroo_close;
static d_read_t      vga_switcheroo_read;
static d_write_t     vga_switcheroo_write;

static struct dev_ops vga_switcheroo_ops = {
	{ "vga_switcheroo", 0, 0 },
	.d_open =		vga_switcheroo_open,
	.d_close =	vga_switcheroo_close,
	.d_read =		vga_switcheroo_read,
	.d_write =	vga_switcheroo_write,
};

struct s_vga_switcheroo {
	char msg[BUFFERSIZE + 1];
	int len;
};

static cdev_t vga_switcheroo_dev;
static struct s_vga_switcheroo *vga_switcheroo_buf;

MALLOC_DECLARE(M_VGA_SWITCHEROO_BUF);
MALLOC_DEFINE(M_VGA_SWITCHEROO_BUF, "vga_switcheroo_buf", "buffer for vga_switcheroo");
MALLOC_DECLARE(M_VGA_SWITCHEROO_VGASR_PRIV);
MALLOC_DEFINE(M_VGA_SWITCHEROO_VGASR_PRIV, "vga_switcheroo_vgasr_priv", "private data of vga_switcheroo");

static int
vga_switcheroo_handler(struct module *m __unused, int what, void *arg __unused)
{
	int error = 0;

	switch (what) {
	case MOD_LOAD:                /* kldload */
		vga_switcheroo_dev = make_dev(
				&vga_switcheroo_ops,
		    0,
		    UID_ROOT,
		    GID_WHEEL,
		    0600,
		    "vga_switcheroo");
		reference_dev(vga_switcheroo_dev);
		if (error != 0)
			break;

		vga_switcheroo_buf = kmalloc(sizeof(*vga_switcheroo_buf), M_VGA_SWITCHEROO_BUF, M_WAITOK | M_ZERO);

		//init vgasr_priv
		vgasr_priv = kmalloc(sizeof(*vgasr_priv), M_VGA_SWITCHEROO_VGASR_PRIV, M_WAITOK | M_ZERO);
		vgasr_priv->clients = (struct list_head) { &(vgasr_priv->clients), &(vgasr_priv->clients) };
		//LK_CANRECURSE or 0?
		lockinit(&vgasr_priv->mux_hw_lk, "mux_hw_lk", 0, LK_CANRECURSE);

		//DEBUG
		kprintf("vga_switcheroo device loaded\n");
		break;

	case MOD_UNLOAD:
	case MOD_SHUTDOWN:
		destroy_dev(vga_switcheroo_dev);
		kfree(vga_switcheroo_buf, M_VGA_SWITCHEROO_BUF);
		kfree(vgasr_priv, M_VGA_SWITCHEROO_VGASR_PRIV);
		//DEBUG
		kprintf("vga_switcheroo_buf device unloaded\n");
		break;

	default:
		error = EOPNOTSUPP;
		break;
	}

	return (error);
}

static int
vga_switcheroo_open(struct dev_open_args *ap)
{
	int error = 0;

	//uprintf("Opened device vga_switcheroo successfully.\n");
	return (error);
}

static int
vga_switcheroo_close(struct dev_close_args *ap)
{
	//uprintf("Closing device \"echo\".\n");
	return (0);
}

static int
vga_switcheroo_read(struct dev_read_args *ap)
{
	/* XXX: TODO
	struct uio *uio = ap->a_uio;
	size_t amt;
	*/

	int error = 0;

	/* XXX */

	//DEBUG
	//uprintf(":Read successful!:");

	return (error);
}

static int
vga_switcheroo_write(struct dev_write_args *ap)
{
	/* XXX: TODO
	struct uio *uio = ap->a_uio;
	size_t amt;
	*/

	int error = 0;

	/* XXX */

	return (error);
}

MODULE_VERSION(vga_switcheroo, 1);
// MODULE_DEPEND(vga_switcheroo, drm, 1, 1, 4);

DEV_MODULE(vga_switcheroo, vga_switcheroo_handler, NULL);


/**
 * vga_switcheroo_process_delayed_switch() - helper for delayed switching
 *
 * Process a delayed switch if one is pending. DRM drivers should call this
 * from their ->lastclose callback.
 *
 * Return: 0 on success. -EINVAL if no delayed switch is pending, if the client
 * has unregistered in the meantime or if there are other clients blocking the
 * switch. If the actual switch fails, an error is reported and 0 is returned.
 */
int
vga_switcheroo_process_delayed_switch(void)
{
	struct vga_switcheroo_client *client;
	int ret;
	int err = -EINVAL;

	mutex_lock(&vgasr_mutex);
	if (!vgasr_priv->delayed_switch_active)
		goto err;

	pr_info("processing delayed switch to %d\n",
		vgasr_priv->delayed_client_id);

	client = find_client_from_id(&vgasr_priv->clients,
				     vgasr_priv->delayed_client_id);
	if (!client || !check_can_switch())
		goto err;

	ret = vga_switchto_stage2(client);
	if (ret)
		pr_err("delayed switching failed stage 2 %d\n", ret);

	vgasr_priv->delayed_switch_active = false;
	err = 0;
err:
	mutex_unlock(&vgasr_mutex);
	return err;
}
EXPORT_SYMBOL(vga_switcheroo_process_delayed_switch);

/**
 * DOC: Driver power control
 *
 * In this mode of use, the discrete GPU automatically powers up and down at
 * the discretion of the driver's runtime pm. On muxed machines, the user may
 * still influence the muxer state by way of the debugfs interface, however
 * the ON and OFF commands become a no-op for the discrete GPU.
 *
 * This mode is the default on Nvidia HybridPower/Optimus and ATI PowerXpress.
 * Specifying nouveau.runpm=0, radeon.runpm=0 or amdgpu.runpm=0 on the kernel
 * command line disables it.
 *
 * When the driver decides to power up or down, it notifies vga_switcheroo
 * thereof so that it can (a) power the audio device on the GPU up or down,
 * and (b) update its internal power state representation for the device.
 * This is achieved by vga_switcheroo_set_dynamic_switch().
 *
 * After the GPU has been suspended, the handler needs to be called to cut
 * power to the GPU. Likewise it needs to reinstate power before the GPU
 * can resume. This is achieved by vga_switcheroo_init_domain_pm_ops(),
 * which augments the GPU's suspend/resume functions by the requisite
 * calls to the handler.
 *
 * When the audio device resumes, the GPU needs to be woken. This is achieved
 * by vga_switcheroo_init_domain_pm_optimus_hdmi_audio(), which augments the
 * audio device's resume function.
 *
 * On muxed machines, if the mux is initially switched to the discrete GPU,
 * the user ends up with a black screen when the GPU powers down after boot.
 * As a workaround, the mux is forced to the integrated GPU on runtime suspend,
 * cf. https://bugs.freedesktop.org/show_bug.cgi?id=75917
 */

#if 0  /* used in: runtime_suspend/resume() */
static void
vga_switcheroo_power_switch(struct pci_dev *pdev,
					enum vga_switcheroo_state state)
{
	struct vga_switcheroo_client *client;

	if (!vgasr_priv->handler->power_state)
		return;

	client = find_client_from_pci(&vgasr_priv->clients, pdev);
	if (!client)
		return;

	if (!client->driver_power_control)
		return;

	vgasr_priv->handler->power_state(client->id, state);
}
#endif  /* used in: runtime_suspend/resume() */

/**
 * vga_switcheroo_set_dynamic_switch() - helper for driver power control
 * @pdev: client pci device
 * @dynamic: new power state
 *
 * Helper for GPUs whose power state is controlled by the driver's runtime pm.
 * When the driver decides to power up or down, it notifies vga_switcheroo
 * thereof using this helper so that it can (a) power the audio device on
 * the GPU up or down, and (b) update its internal power state representation
 * for the device.
 */
#if 0  /* DFly: no dynamic_switch */
void
vga_switcheroo_set_dynamic_switch(struct pci_dev *pdev,
				       enum vga_switcheroo_state dynamic)
{
	struct vga_switcheroo_client *client;

	mutex_lock(&vgasr_mutex);
	client = find_client_from_pci(&vgasr_priv.clients, pdev);
	if (!client || !client->driver_power_control) {
		mutex_unlock(&vgasr_mutex);
		return;
	}

	client->pwr_state = dynamic;
	set_audio_state(client->id, dynamic);
	mutex_unlock(&vgasr_mutex);
}
EXPORT_SYMBOL(vga_switcheroo_set_dynamic_switch);

/* switcheroo power domain */
static int
vga_switcheroo_runtime_suspend(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	ret = dev->bus->pm->runtime_suspend(dev);
	if (ret)
		return ret;
	mutex_lock(&vgasr_mutex);
	if (vgasr_priv.handler->switchto) {
		mutex_lock(&vgasr_priv.mux_hw_lock);
		vgasr_priv.handler->switchto(VGA_SWITCHEROO_IGD);
		mutex_unlock(&vgasr_priv.mux_hw_lock);
	}
	vga_switcheroo_power_switch(pdev, VGA_SWITCHEROO_OFF);
	mutex_unlock(&vgasr_mutex);
	return 0;
}

static int
vga_switcheroo_runtime_resume(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	int ret;

	mutex_lock(&vgasr_mutex);
	vga_switcheroo_power_switch(pdev, VGA_SWITCHEROO_ON);
	mutex_unlock(&vgasr_mutex);
	ret = dev->bus->pm->runtime_resume(dev);
	if (ret)
		return ret;

	return 0;
}
#endif  /* DFly: no dynamic_switch */

/**
 * vga_switcheroo_init_domain_pm_ops() - helper for driver power control
 * @dev: vga client device
 * @domain: power domain
 *
 * Helper for GPUs whose power state is controlled by the driver's runtime pm.
 * After the GPU has been suspended, the handler needs to be called to cut
 * power to the GPU. Likewise it needs to reinstate power before the GPU
 * can resume. To this end, this helper augments the suspend/resume functions
 * by the requisite calls to the handler. It needs only be called on platforms
 * where the power switch is separate to the device being powered down.
 */
#if 0 /* DFly: no pm_ops */
int
vga_switcheroo_init_domain_pm_ops(struct device *dev,
				      struct dev_pm_domain *domain)
{
	/* copy over all the bus versions */
	if (dev->bus && dev->bus->pm) {
		domain->ops = *dev->bus->pm;
		domain->ops.runtime_suspend = vga_switcheroo_runtime_suspend;
		domain->ops.runtime_resume = vga_switcheroo_runtime_resume;

		dev_pm_domain_set(dev, domain);
		return 0;
	}
	dev_pm_domain_set(dev, NULL);
	return -EINVAL;
}
EXPORT_SYMBOL(vga_switcheroo_init_domain_pm_ops);

void
vga_switcheroo_fini_domain_pm_ops(struct device *dev)
{
	dev_pm_domain_set(dev, NULL);
}
EXPORT_SYMBOL(vga_switcheroo_fini_domain_pm_ops);

static int
vga_switcheroo_runtime_resume_hdmi_audio(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct vga_switcheroo_client *client;
	struct device *video_dev = NULL;
	int ret;

	/* we need to check if we have to switch back on the video
	   device so the audio device can come back */
	mutex_lock(&vgasr_mutex);
	list_for_each_entry(client, &vgasr_priv.clients, list) {
		if (PCI_SLOT(client->pdev->devfn) == PCI_SLOT(pdev->devfn) &&
		    client_is_vga(client)) {
			video_dev = &client->pdev->dev;
			break;
		}
	}
	mutex_unlock(&vgasr_mutex);

	if (video_dev) {
		ret = pm_runtime_get_sync(video_dev);
		if (ret && ret != 1)
			return ret;
	}
	ret = dev->bus->pm->runtime_resume(dev);

	/* put the reference for the gpu */
	if (video_dev) {
		pm_runtime_mark_last_busy(video_dev);
		pm_runtime_put_autosuspend(video_dev);
	}
	return ret;
}
#endif  /* DFly: no pm_ops */

/**
 * vga_switcheroo_init_domain_pm_optimus_hdmi_audio() - helper for driver
 * 	power control
 * @dev: audio client device
 * @domain: power domain
 *
 * Helper for GPUs whose power state is controlled by the driver's runtime pm.
 * When the audio device resumes, the GPU needs to be woken. This helper
 * augments the audio device's resume function to do that.
 *
 * Return: 0 on success, -EINVAL if no power management operations are
 * defined for this device.
 */
#if 0  /* DFly: no pm */
int
vga_switcheroo_init_domain_pm_optimus_hdmi_audio(struct device *dev,
						 struct dev_pm_domain *domain)
{
	/* copy over all the bus versions */
	if (dev->bus && dev->bus->pm) {
		domain->ops = *dev->bus->pm;
		domain->ops.runtime_resume =
			vga_switcheroo_runtime_resume_hdmi_audio;

		dev_pm_domain_set(dev, domain);
		return 0;
	}
	dev_pm_domain_set(dev, NULL);
	return -EINVAL;
}
EXPORT_SYMBOL(vga_switcheroo_init_domain_pm_optimus_hdmi_audio);
#endif /* DFly: no pm */
