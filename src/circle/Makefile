#
# Mac Emulator - Circle kernel
#
# Build Circle and its fatfs/SDCard addons first (see README.md), then:
#   make CIRCLEHOME=/path/to/circle
#

CIRCLEHOME ?= ../../circle

OBJS	= main.o kernel.o storage.o webserver.o \
	  ../core/m68k.o ../core/fpu.o ../core/fmath.o ../core/via.o ../core/iwm.o ../core/scsi.o ../core/rtc.o \
	  ../core/adb.o ../core/asc.o ../core/nubusvideo.o ../core/mmu040.o ../core/dafb.o ../core/esp.o ../core/quadra.o ../core/scc.o \
	  ../core/mac.o ../core/macii.o \
	  ../frontend/config.o ../frontend/keymap.o ../frontend/video.o ../frontend/osd.o

LIBS	= $(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
	  $(CIRCLEHOME)/addon/wlan/libwlan.a \
	  $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	  $(CIRCLEHOME)/lib/net/libnet.a \
	  $(CIRCLEHOME)/lib/sched/libsched.a \
	  $(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	  $(CIRCLEHOME)/lib/sound/libsound.a \
	  $(CIRCLEHOME)/lib/usb/libusb.a \
	  $(CIRCLEHOME)/lib/input/libinput.a \
	  $(CIRCLEHOME)/lib/fs/libfs.a \
	  $(CIRCLEHOME)/lib/libcircle.a

INCLUDE	+= -I $(CIRCLEHOME)/addon

include $(CIRCLEHOME)/Rules.mk

# The emulator core is hot code: optimise it for speed.
OPTIMIZE = -O3

# sqrt() must become the hardware instruction: there is no libm to fall back on.
CFLAGS += -fno-math-errno

-include $(DEPS)
