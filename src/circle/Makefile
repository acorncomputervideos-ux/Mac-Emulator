#
# Mac Emulator - Circle kernel
#
# Build Circle and its fatfs/SDCard addons first (see README.md), then:
#   make CIRCLEHOME=/path/to/circle
#

CIRCLEHOME ?= ../../circle

OBJS	= main.o kernel.o storage.o \
	  ../core/m68k.o ../core/via.o ../core/iwm.o ../core/scsi.o ../core/mac.o \
	  ../frontend/config.o ../frontend/keymap.o ../frontend/video.o ../frontend/osd.o

LIBS	= $(CIRCLEHOME)/addon/fatfs/libfatfs.a \
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

-include $(DEPS)
