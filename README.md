# Mac Emulator for Raspberry Pi

A classic Macintosh (Mac Plus, 512Ke, 512K, 128K) emulator that runs **bare
metal** on the Raspberry Pi using the [Circle](https://github.com/rsta2/circle)
C++ environment. There is no Linux underneath: the Pi boots straight into the
Mac, and gets to the Happy Mac in about a second.

## Features

**Machine**
- Motorola 68000 interpreter, checked against the 1,000,000-test
  [SingleStepTests](https://github.com/SingleStepTests/680x0) suite.
  Every vector that does not involve an address error passes.
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
- **F12** on-screen menu: swap floppies, set speed (1×, 2×, 4×, 8×,
  unlimited), zoom, colours, sound, volume, mouse speed and key layout.
  Restart the Mac, press the interrupt switch, take a screenshot, save
  settings.
- **F11** toggles unlimited speed (turbo).
- **F10** saves a screenshot to `screenshots/macNNN.bmp` on the SD card.
- **F9** presses the interrupt switch.
- The ACT LED shows disk activity.
- Settings live in a plain-text `macemu.cfg`.

## What you need

- A Raspberry Pi. Pi 3, Pi 3+, Zero 2 W, Pi 4 and Pi 5 run in 64-bit mode;
  Pi 1, Zero and Pi 2 have 32-bit builds.
- A FAT32 SD card, a USB keyboard and mouse, and an HDMI display.
- **A Macintosh ROM image you are entitled to use.** None is included. The Mac
  Plus ROM (128 KB, checksum `4D1F8172` or `4D1EEEE1`) is recommended. Name it
  `mac.rom`.
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
| Ctrl, Alt                     | Command (⌘)                                  |
| Windows key, Right Alt        | Option                                       |
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
               SD card (FatFs), pacing, hot keys
src/host/      headless PC runner
tests/         unit tests and the SingleStepTests driver
tools/         test-vector converter, self-test ROM generator
sdcard/        files that go on the SD card
```

## Status and limitations

What has been verified:
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

Not emulated:
- 68000 address errors.
- The serial ports: no modem, printer or LocalTalk. The SCC is modelled only
  far enough for the ROM.
- Mac SE, Mac II and colour.

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
