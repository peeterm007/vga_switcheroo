# vga_switcheroo

Port of vga_switcheroo to DragonFly

### Installing

Copy `vga_switcheroo.h` to

`/usr/src/sys/dev/drm/include/linux/`

and then `make`. Load the module with

`# kldload ./vga_switcheroo.ko`
