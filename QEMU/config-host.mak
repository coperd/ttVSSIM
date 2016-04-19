# Automatically generated by configure - do not modify
# Configured with: './configure'
prefix=/usr/local
bindir=${prefix}/bin
mandir=${prefix}/share/man
datadir=${prefix}/share/qemu
docdir=${prefix}/share/doc/qemu
MAKE=make
INSTALL=install
INSTALL_DIR=install -d -m0755 -p
INSTALL_DATA=install -m0644 -p
INSTALL_PROG=install -m0755 -p
CC=gcc
HOST_CC=gcc
AR=ar
OBJCOPY=objcopy
LD=ld
CFLAGS= -g -fno-strict-aliasing -O2 -Wall -Wundef -Wendif-labels -Wwrite-strings -Wmissing-prototypes -Wstrict-prototypes -Wredundant-decls  -m64 
LDFLAGS= -g -Wl,--warn-common  -m64 
EXESUF=
PTHREADLIBS=-lpthread
CLOCKLIBS=
ARCH=x86_64
STRIP_OPT=-s
CONFIG_SLIRP=y
CONFIG_AC97=y
CONFIG_ES1370=y
CONFIG_SB16=y
CONFIG_OSS=y
CONFIG_OSS_LIB=
CONFIG_VNC_TLS=y
CONFIG_VNC_TLS_CFLAGS=-I/usr/include/p11-kit-1  
CONFIG_VNC_TLS_LIBS=-lgnutls  
CONFIG_VNC_SASL=y
CONFIG_VNC_SASL_CFLAGS=
CONFIG_VNC_SASL_LIBS=-lsasl2
VERSION=0.11.0
SRC_PATH=/home/huaicheng/git/VSSIM/QEMU
TARGET_DIRS=x86_64-softmmu
CONFIG_SDL=y
SDL_LIBS=-L/usr/lib/x86_64-linux-gnu -lSDL -lX11
SDL_CFLAGS=-I/usr/include/SDL -D_GNU_SOURCE=1 -D_REENTRANT
CONFIG_CURSES=y
CURSES_LIBS=-lncurses
CONFIG_BRLAPI=y
BRLAPI_LIBS=-lbrlapi
CONFIG_BLUEZ=y
CONFIG_BLUEZ_CFLAGS= 
CONFIG_BLUEZ_LIBS=-lbluetooth  
CONFIG_AIO=y
INSTALL_BLOBS=yes
FDT_LIBS=-lfdt
HOST_USB=linux
ARLIBS_BEGIN=-Wl,--whole-archive
ARLIBS_END=-Wl,--no-whole-archive
KVM_CFLAGS=-I/home/huaicheng/git/VSSIM/QEMU/kvm/include -I/home/huaicheng/git/VSSIM/QEMU/kvm/include/x86
KVM_KMOD=no
TOOLS=qemu-nbd$(EXESUF) qemu-io$(EXESUF) qemu-img$(EXESUF) 
ROMS=optionrom
subdir-x86_64-softmmu: subdir-libhw64
