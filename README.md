# Mac Emulator for Raspberry Pi

A classic Macintosh emulator that runs **bare metal** on the Raspberry Pi
using the [Circle](https://github.com/rsta2/circle) C++ environment. There is
no Linux underneath: the Pi boots straight into the Mac.

Three models are emulated:

- **Macintosh Plus** (and 512Ke, 512K, 128K): black and white, 68000.
- **Macintosh II**: colour, 68020 + 68881 FPU, ADB, Apple Sound Chip and a
  NuBus colour video card. Not yet tested with a real Mac II ROM.
- **Quadra 800** (one ROM covers the Quadra 610/650/800 and Centris
  610/650): 68040 with MMU and FPU, built-in DAFB colour video, 53C96 SCSI,
  Enhanced ASC. With a real Quadra 650 ROM it **boots System 7.1 to the
  Finder** from a SCSI disk image, in colour, with working mouse and menus.

## Features

**Processors**
- Motorola 68000, checked against the 1,000,000-test
  [SingleStepTests](https://github.com/SingleStepTests/680x0) suite.
  Every vector that does not involve an address error passes.
- 68020, 68030 and 68040 instruction sets: 32-bit addressing, memory-indirect
  modes, bit fields, CAS/CAS2, 64-bit multiply and divide, MOVEC/VBR/MSP,
  stack frame formats, bus errors and MOVE16.
- 68881/68882 and 68040 floating point: every instruction, including the
  transcendental functions, in every data format (also packed decimal). It
  computes in `long double`, which is IEEE quad precision on the 64-bit Pi
  kernels, a superset of the 68881's 80-bit extended format.

**Quadra 800 / 650 / 610 (colour, 68040)**
- 68040 with its MMU and FPU at 33 MHz.
- DAFB built-in video: a VGA monitor (640×480, 800×600) or an Apple 21-inch
  display (1152×870), at 1/2/4/8 bits and millions of colours on VGA (the
  ROM's own driver picks the mode).
- NCR 53C96 SCSI with pseudo-DMA, up to seven disks.
- ADB keyboard and mouse, Enhanced ASC sound, clock and PRAM, soft power off.
- Follows QEMU's Quadra 800 model (GLUE interrupt routing, djMEMC, IOSB,
  VIA quirks), which boots Mac OS 7.1–8.1.

**Macintosh II (colour)**
- 24-bit and 32-bit addressing (the Mac II HMMU), ROM overlay, NuBus slots
  with bus errors for empty slots.
- A NuBus colour video card designed for the emulator. It offers 1, 2, 4 and
  8 bits per pixel at 640×480, 832×624 or 1024×768 (`video=`), with a
  256-entry colour table, gray mode and VBL interrupts. Its declaration ROM
  and display driver (`src/core/videodrv.s`) are original, so no Apple video
  card ROM is needed.
- Apple Desktop Bus keyboard (extended layout: function keys, Escape,
  Control, Home/End/Page keys, forward delete) and mouse, with autopolling.
- Apple Sound Chip: FIFO and wavetable sound.
- Shut Down really turns the Mac off. The Pi then waits for a key to start it
  again.

**Machine**
- Accurate bus timing (7.8336 MHz, 370 lines × 352 cycles per frame, 60.15 Hz).
- Mac Plus (128K ROM) with 1, 2 or 4 MB of RAM. Mac 128K/512K with 64K ROMs.
- 6522 VIA with both timers and the shift register.
- Real-time clock and 256-byte parameter RAM, saved to the SD card, so
  Control Panel settings survive a power cycle.
- Interrupt switch (F9), for MacsBug and other debuggers.

**Disks**
- Two 400K/800K floppy drives behind an emulated IWM. Disks are presented as
  real GCR-encoded tracks, so the unmodified ROM `.Sony` driver reads, writes
  and **formats** them. No ROM patches.
- Raw `.dsk` / `.img` and DiskCopy 4.2 images.
- Floppies can be swapped at any time from the F12 menu. Disks ejected from the
  Finder are released, and changes are written back to the SD card.
- Up to seven SCSI hard disks (NCR 5380, IDs 0–6) for System 6 and System 7 on
  a Mac Plus. Supports pseudo-DMA and programmed I/O, plus INQUIRY, READ/WRITE
  (6/10), READ CAPACITY, MODE SENSE/SELECT and REQUEST SENSE.

**Display, sound and input**
- 512×342 screen, scaled by the largest whole number that fits, or a chosen
  zoom. Only changed rows are redrawn.
- Six colour schemes: black & white, paper, green phosphor, amber, LCD and
  inverted.
- Mac sound hardware (the 22 kHz sound buffer, volume levels and the
  timer-driven square wave) played over HDMI or the 3.5 mm jack (PWM).
- USB keyboard with the full Mac Plus layout, including keypad and arrows.
  Caps Lock latches like the real key.
- USB mouse with adjustable speed.
- Hot-plugging of keyboard and mouse.

**Emulator controls**
- **F12** (or Pause, Scroll Lock, the Menu key) opens the on-screen menu.
  The Mac pauses while it is open.
  - *Floppy 1 / Floppy 2*: insert or eject a floppy from the `disks` folder.
  - *Hard disks*: choose the image for each SCSI ID 0–6 (from the top of the
    card or the `disks` folder), then *Restart the Mac* to use them.
  - *Machine, ROM and memory*: model (auto, Plus, II, Quadra), ROM file,
    memory size, the Mac II screen size and the Quadra's monitor (VGA, or a
    21-inch display at 1152×870). *Apply* saves the settings and
    restarts the Pi with them.
  - *Display, sound and input*: speed (1×–8×, unlimited), colour scheme,
    sound, volume, mouse speed and key layout.
  - *Network*: connection type, Wi-Fi name, password and country, keyboard
    layout for typing them, and the web file manager's address.
  - Restart the Mac, interrupt switch, screenshot, save settings.
- **F11** toggles unlimited speed (turbo).
- **F10** saves a screenshot to `screenshots/macNNN.bmp` on the SD card.
- **F9** presses the interrupt switch.
- The ACT LED shows disk activity.
- Settings live in a plain-text `macemu.cfg`.

**Network: Wi-Fi or Ethernet**
- Wi-Fi (Pi 3, 3+, 4, 5, Zero W and Zero 2 W): open the F12 menu, choose
  *Network*, type the Wi-Fi name and password, pick your country and choose
  *Save and restart*. The Wi-Fi chip's firmware is in `firmware/` on the
  card. A `wpa_supplicant.conf` of your own on the card works too.
- Ethernet: plug in a cable (Pi 3/3+ via USB, Pi 4 and Pi 5 built in).
- *Connection* chooses Automatic (Wi-Fi when a Wi-Fi network is set up,
  otherwise the cable), Wi-Fi, Ethernet or Off. One of them is used at a time.
- Nothing waits for the network: it comes up while the Mac starts.

**Web file manager and software update**
- Open `http://macemu.local/` in a browser (or the address shown on the
  menu's *Network* page). Upload (drag and drop, up to 4 GB), download and
  delete disk images and ROMs, and restart the Mac.
- *Update the emulator*: drop a new `MacEmulator-SD.zip` on the page. The
  browser unpacks it and sends the new kernels, firmware and start-up files;
  then the Pi restarts. Your settings, ROMs, PRAM and disks are kept.
- Disks the Mac is using cannot be replaced or deleted, the Pi's start-up
  files can only be replaced by an update, and `macemu.cfg` and the Wi-Fi
  files (which hold the password) are never shown or sent. Uploads go to a
  temporary file first, so a broken transfer never leaves half a file.
- Transfers are fastest while the F12 menu is open (the Mac is paused).

**Fast start-up**
- The Pi's rainbow splash, the boot delay and the firmware's warning
  overlays are switched off in `config.txt`, and the ARM runs at full speed
  from the first instruction (`initial_turbo`).
- The emulator does not wait for USB or the network: keyboard, mouse and
  Ethernet are found while the Mac is already starting, and the screen stays
  dark until the Mac draws its first picture.

## What you need

- A Raspberry Pi. Pi 3, Pi 3+, Zero 2 W, Pi 4 and Pi 5 run in 64-bit mode;
  Pi 1, Zero and Pi 2 have 32-bit builds. For the Mac II, use a Pi 3 or
  later.
- A FAT32 SD card, a USB keyboard and mouse, and an HDMI display.
- **A Macintosh ROM image you are entitled to use.** None is included. Name
  it `mac.rom`. The ROM's size picks the model, or set `machine=plus|ii`:
  - Mac Plus: 128 KB, checksum `4D1F8172` or `4D1EEEE1`.
  - Mac II: 256 KB, checksum `97851DB6` or `9779D2C4`.
  - Quadra 610/650/800, Centris 610/650: 1 MB, checksum `F1ACAD13`.
    Give it 8 MB of RAM or more (`ram=32768` is a good start).
  - The 256 KB "universal" ROM (IIx, IIcx, SE/30) and other 512 KB/1 MB ROMs
    (IIci, IIfx, Quadra 700/900/950) need machines that are not emulated yet.
- System software disk images. Apple distributed System 6 and earlier for free.

## Setting up the SD card

1. Build the kernels (or take `sdcard/*.img` from this repository or from a CI
   run) and download the Pi firmware:
   ```sh
   ./build.sh all        # firmware + Pi 3, Pi 4 and Pi 5 kernels into ./sdcard
   ./build.sh pi2        # optional: 32-bit kernels
   ./build.sh pi1
   ```
2. Copy **everything in `sdcard/`** to the root of a FAT32 SD card.
3. Add your ROM as `mac.rom`.
4. Put floppy images in `disks/`. To boot straight from one, set `floppy1=` in
   `macemu.cfg`.
5. Optional: add a hard disk image as `hd0.img`. Hard disk images are raw
   dumps of a SCSI disk with an Apple partition map and driver, for example
   images made for other Mac Plus emulators or copied from real hardware.
   Apple HD SC Setup only formats Apple-branded drives, so for a new disk start
   from a ready-made partitioned image, or use a third-party formatter.

Then insert the card and power on. With no bootable disk you get the familiar
flashing question-mark disk. Press F12 to choose one.

## Keyboard

| PC key                        | Mac key                                      |
|-------------------------------|----------------------------------------------|
| Left Ctrl, Alt                | Command (⌘)                                  |
| Windows key, Right Alt        | Option                                       |
| Right Ctrl                    | Control (Mac II)                             |
| Esc, F1–F8, Home, End, Page Up/Down, Insert (Help), Delete | the same keys on the Mac II's extended keyboard |
| Caps Lock                     | Caps Lock (latching)                         |
| Backspace / Delete            | Delete                                       |
| Arrows, keypad                | Mac Plus arrows and keypad                   |
| Num Lock                      | Keypad Clear                                 |
| F9 / F10 / F11 / F12          | Interrupt / screenshot / turbo / menu        |

`alt_is_option=1` swaps Alt and the Windows key.

In the menu, use the arrow keys, Enter, Esc, and Page Up/Down.

## Configuration

Every option is described in [`sdcard/macemu.cfg`](sdcard/macemu.cfg). The F12
menu's *Save settings* rewrites that file with the current choices, including
the floppies that are inserted.

The Pi has no battery-backed clock. The Mac's clock starts at `date=` (format
`YYYY-MM-DD HH:MM`), or at the build date if that is empty.

## Building

### Raspberry Pi kernel

```sh
./build.sh pi3     # or pi4, pi5, pizero2, pi3-32, pi2, pi1, firmware, all
```

On first use the script clones Circle (Step 51.1.1) into `./circle`, then
configures and builds it with the SD card and FatFs add-ons. It writes the
kernel to `sdcard/`. It uses `aarch64-none-elf-` (Arm GNU Toolchain) or
`aarch64-linux-gnu-` for 64-bit models and `arm-none-eabi-` for 32-bit ones.
Set `PREFIX=` to override. On Debian or Ubuntu:

```sh
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu gcc-arm-none-eabi libnewlib-arm-none-eabi
```

### Tests and the host runner

```sh
make test                                          # hardware + frontend unit tests (ASan/UBSan)
git clone --depth 1 https://github.com/SingleStepTests/680x0 sst
make cputest                                       # 68000 instruction tests
make build/macemu-host                             # headless runner for a PC
build/macemu-host mac.rom --floppy1 System.dsk --frames 1200 --out boot
```

`macemu-host` runs the same core and menu code on a PC. It writes
screenshots (`.ppm`/`.bmp`) and, with `--wav`, the sound output. It can also
type text (`--type`) and click (`--click X,Y`). Use it to check a ROM and disk
combination before writing an SD card. `tools/mktestrom.py` builds a tiny
self-test ROM (no Apple code) that paints a test pattern. It is handy for
checking a fresh build on real hardware.

## Layout

```
src/core/      machine emulation: 68000, VIA, IWM + GCR floppy, NCR 5380 SCSI,
               RTC/PRAM, keyboard, mouse, sound, memory map (freestanding C++)
src/frontend/  platform-independent frontend: config file, USB→Mac key map,
               display scaling/palettes/BMP, F12 menu
src/circle/    the Raspberry Pi kernel: framebuffer, USB HID, HDMI/PWM sound,
               SD card (FatFs), pacing, hot keys, web file manager
src/host/      headless PC runner
tests/         unit tests and the SingleStepTests driver; webtest runs the
               web file manager against real sockets and files
tools/         test-vector converter, self-test ROM generator
sdcard/        files that go on the SD card
```

## Status and limitations

What has been verified for the Quadra:
- With the Quadra 610/650/800 ROM (`F1ACAD13`) on the host build and in the
  Pi 3 kernel under QEMU, the ROM plays the startup chime, sizes RAM, scans
  NuBus, turns on the 68040 MMU and shows the desktop with the disk prompt.
  With a System 7.1 hard disk image (with PowerTalk and AppleTalk) it boots
  to the Finder in about 40 emulated seconds; the Apple menu opens with
  the mouse. Getting there needed the 53C96's exact pseudo-DMA behaviour
  and a real Z8530 SCC (the AppleTalk driver waits for its interrupts).
- `mmutest` covers the 68040 MMU's table walks, faults, transparent
  translation and the translation cache.

What has been verified for the Mac II and the 68020+/FPU:
- 216 hand-written 68020/68030/68040 and FPU cases (`tests/cpu020_cases.h`).
  Their expected values come from the Motorola manuals and are generated with
  GNU as. The maths library is within a few ulp of an 80-bit `libm`.
- The video card's declaration ROM is parsed and checksummed the way the Slot
  Manager does it. The 68k display driver runs on the emulated 68020 against a
  minimal fake Mac OS (Open, SetMode, SetEntries, GetEntries, GetGamma, VBL
  interrupt, Close).
- Mac II hardware tests cover the memory map, 24/32-bit switching, slot bus
  errors, ADB transactions and autopoll, the ASC and the interrupt wiring.
- `tools/mktestrom.py --macii` builds a colour self-test ROM. It renders
  identically in the host build and in the Pi 3 kernel under QEMU.

The ADB and ASC behaviour follows QEMU's Quadra 800 model, which is known to
boot Mac OS. The rest of the Mac II wiring is from Apple's hardware
documentation. **Booting a real Mac II ROM has not been tried yet**, so expect
fixes there.

What has been verified for the Mac Plus:
- The 68000 core against the SingleStepTests vectors.
- Every peripheral against unit tests that drive it the way the Mac ROM does:
  floppy GCR read/write, the SCSI command/data/status cycle, RTC and PRAM,
  the keyboard protocol, mouse, sound, VBL and timing.
- The kernel boots under QEMU's `raspi3b` machine: it mounts the SD card,
  reads `macemu.cfg`, loads the ROM and runs the emulated Mac with the
  configured colours.

What has **not** been verified:
- Booting a real Apple ROM. No Apple ROM was available while this was
  written, so expect some fixes once real System software runs on it.
- Real Pi hardware and the 32-bit Pi 1/Zero/2 kernels. The 32-bit kernels
  build, but did not produce output under QEMU's `raspi2b` machine (Circle
  normally needs its `--qemu` option for that).

Not emulated yet:
- The 68030 MMU, and with it the IIx/IIcx/SE/30/IIci.
- Floppies on the Quadra beyond the SWIM's IWM mode (no 1.44 MB MFM disks).
- Ethernet for the Mac itself (the Pi's Ethernet is used for the web file
  manager). The serial ports' SCC works, but nothing is connected to it:
  AppleTalk finds no network.
- Restartable access faults: a faulting instruction completes before the
  exception, so virtual memory should stay off.
- Video modes with thousands or millions of colours.
- 68000 address errors.
- The serial ports: no modem, printer or LocalTalk. The SCC is modelled only
  far enough for the ROM.
- Mac SE.

The mouse is fed to the OS through its own low-memory mouse variables, the
same way the ROM's mouse interrupt handler does, rather than as SCC
quadrature interrupts. This is smooth, and it starts once the system enables
mouse interrupts.

Floppy images are kept in memory, at most 800 KB each. Changes are written
back to the SD card about two seconds after the disk goes idle, and on eject or
restart. Hard disk images are read and written in place.

## Licence

The emulator sources in this repository are original work. Circle (GPLv3) is
downloaded at build time and linked into the kernel, so a distributed kernel
image falls under the GPLv3.

The Wi-Fi chip firmware in `sdcard/firmware/` comes from Raspberry Pi's
[firmware-nonfree](https://github.com/RPi-Distro/firmware-nonfree) and is
redistributed under its own licence (`firmware/LICENCE.firmware-nonfree`).
The WPA supplicant (Circle's port of hostap) is BSD licensed.
