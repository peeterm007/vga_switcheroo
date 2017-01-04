KMOD=	vga_switcheroo
SRCS=	vga_switcheroo.c
SRCS+=	device_if.h bus_if.h pci_if.h
#SRCS+=	opt_ktr.h iicbus_if.h opt_drm.h

CFLAGS+= -I${SYSDIR}/dev/drm/include

.include <bsd.kmod.mk>
