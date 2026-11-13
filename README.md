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
  VIA quirks), which boots Mac OS 7.1–8.1. Checked here with System 7.1 and
  System 7.5.3 (whose SCSI Manager 4.3 needs the SCSI interrupt latched in
  VIA2 the moment the chip raises it).

**Which ROMs work**
- Quadra 610/650/800 (F1ACAD13) and Centris 610/650 (F1A6F343): the
  Quadra model. Both boot System 7.1.
- Mac II: the Mac II model. Mac Plus, 512K and 128K: the Plus model.
- Mac Classic (A49F9914) and SE: the Plus board with an ADB keyboard and
  mouse, the full SCC and, on the Classic, a SuperDrive. The Classic starts
  System 7.1 to the Finder, and holding Command-Option-X-O at power-on
  starts its built-in System 6 ROM disk.
- Mac Plus: starts from SCSI hard disks (its ROM checks for the SCSI chip
  by how the ROM is mirrored, and waits for the 5380's DRQ after a blind
  write; both now as on the real board).
- ROM dumps with extra bytes after the ROM are accepted (the checksum
  shows where the ROM ends).
- Recognised but not emulated yet, with a message saying so: IIx/IIcx/SE/30,
  IIci, IIsi, IIfx, LC, LC II, LC III, Color Classic, IIvx, LC 475/Quadra 605,
  LC 580/Quadra 630, Quadra 700/900/950.

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
- Hard disk and CD images are also kept in the Pi's memory, as far as it
  allows (a 500 MB disk fits with 1 GB or more): the Pi copies them in while
  it is otherwise idle, in the first minute or so, and from then on the Mac
  reads them without waiting for the card, so the cursor no longer stops
  while it loads things. Writes go to the card straight away.
  `macemu.log` says how much is in memory.

**Display, sound and input**
- 512×342 screen, scaled by the largest whole number that fits, or a chosen
  zoom. Only changed rows are redrawn.
- Six colour schemes: black & white, paper, green phosphor, amber, LCD and
  inverted.
- Mac sound hardware (the 22 kHz sound buffer, volume levels and the
  timer-driven square wave) played over HDMI, the 3.5 mm jack (PWM) or, on
  a Pi 4, 400 or 5, a USB audio device (USB speakers, a headset or a USB
  sound stick: the Pi 400 has no jack, and many monitors no HDMI sound).
  *Sound output* on the menu (`sound_device=` in `macemu.cfg`): Automatic
  uses a USB audio device whenever one is plugged in, otherwise HDMI;
  plugging one in or out switches at once. Or HDMI, Headphone jack, or USB
  only. The *This Mac* panel on the web page shows which is in use.
- USB keyboard with the full Mac Plus layout, including keypad and arrows.
  Caps Lock latches like the real key.
- USB mouse with adjustable speed.
- Hot-plugging of keyboard and mouse.

**Emulator controls**
- **F12** (or Pause, Scroll Lock, the Menu key) opens the on-screen menu.
  The Mac pauses while it is open. The top shows the Mac (model, memory,
  speed, JIT), the shared disk and the web page's address; the items are in
  sections, and the bottom line lists the keys (arrows move and change,
  Enter opens, Esc closes or goes back).
  - *Disks*: Floppy 1 / Floppy 2 insert or eject a floppy from the `disks`
    folder; the floppies and the CD in the drives are there again at the
    next start-up. *New blank floppy* makes an empty, ready-to-use disk
    (800K, or 1.44 MB on a Quadra) as `disks/Blank N.dsk` and puts it in a
    drive. *Hard disks and CD-ROM* chooses the image for each SCSI ID 0–6
    (from the top of the card or the `disks` folder; *Restart the Mac* to
    use them) and the CD in the drive (at once).
  - *Machine, ROM and memory*: model (auto, Plus, II, Quadra), ROM file,
    memory size, the Mac II screen size, the Quadra's monitor (VGA, or a
    21-inch display at 1152×870) and screen scaling (Smooth fills the
    screen; Sharp keeps square pixels at a whole-number zoom; Scanlines
    draws at the display's own resolution with a CRT's dark line under
    every row of Mac pixels, on a display at least twice the Mac's size).
    *Save and restart* restarts the Pi with them.
  - *Speed, display, sound, input*: speed (1×–8×, unlimited), the JIT,
    colour scheme, sound, volume, mouse speed and the Alt key.
  - *Network and sharing*: connection type, Wi-Fi name, password and
    country, the layout for typing them, the web page's address; and for
    the Mac: its internet, the HTTPS helper and the shared disk, on or off.
  - *The Mac*: restart, interrupt switch, screenshot, save settings,
    *Saved Macs*, hibernate, turn off the Pi. Restart, hibernate, turn off
    and the Saved Macs ask for Enter a second time.
- **Power key** (Pi 400: Fn+F10; Pi 5: the power button; keyboards with a
  power key): the Mac II and Quadra ask "Shut Down?". Shutting the Mac down
  shows Mac OS's own "It is now safe to switch off your Macintosh." screen
  (everything is saved by then); on a Pi 400 or Pi 5 the Pi turns off a few
  seconds later and the power key starts it again, elsewhere its Restart
  button starts the Mac again.
  Press it twice, or in the menu, to turn the Pi off at once (after saving).
  The menu also has *Turn off the Pi*. On a Pi 400 the power is cut through
  the keyboard controller (the firmware's GLOBAL_SHUTDOWN line, as Linux
  does), not just halted. If Fn+F10 does nothing, `macemu.log` lists what
  the keyboard's extra USB interface describes and the first reports it
  sends: please send it in. (Not yet tried on a real Pi 400.)
- **F11** toggles unlimited speed (turbo).
- **F10** saves a screenshot to `screenshots/macNNN.bmp` on the SD card.
- **F9** presses the interrupt switch.
- The ACT LED shows disk activity.
- Settings live in a plain-text `macemu.cfg`. It, the PRAM and the Wi-Fi
  file are written to a new file that then replaces the old one, so
  pulling the power while saving never leaves a damaged file.

**Network: Wi-Fi or Ethernet**
- Wi-Fi (Pi 3, 3+, 4, 400, 5, Zero W and Zero 2 W): open the F12 menu,
  choose *Network and sharing*, then *Choose Wi-Fi network* (a scan, strongest first) or
  type the name, enter the password, pick your country and choose *Save and
  restart*. The Wi-Fi chip's firmware is in `firmware/` on the
  card. A `wpa_supplicant.conf` of your own on the card works too.
- Ethernet: plug in a cable (Pi 3/3+ via USB, Pi 4 and Pi 5 built in).
- *Connection* chooses Automatic (Wi-Fi when a Wi-Fi network is set up,
  otherwise the cable), Wi-Fi, Ethernet or Off. One of them is used at a time.
- Nothing waits for the network: it comes up while the Mac starts.
- No router? `ip_address=` (with `ip_netmask=`, `ip_gateway=`) in
  `macemu.cfg` gives the Pi a fixed address instead of asking DHCP: for two
  Pis joined by one cable, say.

**HTTPS helper: today's web for old browsers (Quadra)**
- Type addresses as `http://...` in the Mac's browser (Netscape, MacWeb,
  iCab, Internet Explorer). The helper, built into the emulator's router,
  fetches the page over HTTPS from the Pi and passes it to the Mac as
  plain HTTP:
  - `https://` links in pages become `http://`, so following them works;
  - text in UTF-8 is converted to Mac Roman (accents, curly quotes, dashes,
    the euro sign; characters Mac Roman lacks become `?`);
  - chunked replies are reassembled, and HTTPS-only headers (HSTS, Secure
    cookies, Content Security Policy) are removed;
  - sites with no HTTPS are fetched over plain HTTP.
- No set-up is needed for browsers that send the site's name (Netscape 2
  and later, Internet Explorer, iCab): the router hands the Mac's port 80
  connections to the helper. For older browsers set the HTTP proxy to
  `10.0.2.2` port `8080`. Opening `http://10.0.2.2:8080/` shows a help
  page.
- **Certificates are checked** against Mozilla's root certificates in
  `certs/cacert.pem` on the SD card (from https://curl.se/ca/cacert.pem;
  replace the file to update them). A site whose certificate cannot be
  verified gets an error page explaining why; the helper never quietly
  falls back to an unencrypted connection to that site.
- Checking a certificate needs today's date. The Pi has no clock of its own:
  it asks an internet time server (NTP, `pool.ntp.org`), and if that is
  blocked, takes the date from the site's own web server.
- The TLS (TLS 1.2) is [BearSSL](https://www.bearssl.org/) in
  `third_party/bearssl`. `https_helper=off` in `macemu.cfg` turns the
  helper off. The 10-second lines in `macemu.log` count pages fetched and
  certificates refused (`https=N/M`).
- Limits: pages arrive as the site sends them, so modern sites can still be
  too much for an old browser (heavy scripts and styles), and pictures in
  new formats (WebP, AVIF, SVG) do not show. Compressed replies are asked
  not to be sent. Secure (https) addresses typed into the browser cannot
  work: the browser's own old SSL is refused by today's sites; type
  `http://` instead.
- Checked by tests: a local server (chunked UTF-8 pages, links, cookies and
  headers rewritten, Mac Roman), and real sites over HTTPS with the
  certificate checked, with and without a clock, through the proxy port and
  through port 80, plus a refused untrusted certificate. The Pi kernel
  loads the root certificates and starts the helper (QEMU). Not yet tried
  from a real Mac browser or on a real Pi.

**Ethernet for the Mac (Quadra)**
- The Quadra's built-in SONIC Ethernet chip is emulated: descriptors and
  buffers in the Mac's memory, the address filter (CAM), interrupts through
  VIA2, and the address PROM. The Network control panel offers
  *EtherTalk Built In*.
- Its frames travel over the Pi's own network (cable or Wi-Fi) inside UDP
  datagrams on port 6066, the same as Basilisk II's and SheepShaver's "UDP
  tunnel". Macs on Pis on the same network, and Basilisk II or SheepShaver
  with `udptunnel true`, see each other over AppleTalk: file sharing,
  printers and network games through the Chooser.
- To use it, choose *EtherTalk Built In* in the Mac's Network control panel
  (System 7.1 includes EtherTalk). The Mac's Ethernet address is Apple's
  prefix `08:00:07` and three bytes of the Pi's serial number (Apple's
  Ethernet driver refuses Basilisk II's `B2` + IP addresses). Basilisk II
  and SheepShaver still talk to it: they broadcast to addresses not their
  own kind.
- `mac_ethernet=off` turns it off and `mac_ethernet=6067` picks another
  port. `mac_ethernet_peer=a.b.c.d` also sends broadcasts to one host
  that they don't reach (on another subnet, or across a VPN).
- Checked with two emulated Macs running System 7.1: they find each other
  with AARP, ZIP and NBP, and the second one defends its AppleTalk address
  against the first. And with the Pi 3 kernel under QEMU: the Pi's Mac
  takes the address `B2` + the Pi's IP, its AppleTalk frames reach a Mac on
  the host, and that Mac answers it directly. Not yet checked on real Pi
  hardware.
- Frames larger than 1472 bytes don't fit the tunnel (AppleTalk's are under 620).

**Internet for the Mac (Quadra)**
- A small router inside the emulator gives the Mac TCP/IP through the Pi's
  own connection (cable or Wi-Fi), the way QEMU's and VirtualBox's NAT
  networking do: web browsers, FTP (Fetch), email, Telnet, IRC, news.
- The Mac needs TCP/IP software: MacTCP (System 7.1, 7.5) or Open
  Transport (7.5.3 and later; Apple's *Network Software Selector* in Apple
  Extras switches between the two, and only the chosen one's control
  panels are listed). With MacTCP: click the **Ethernet** icon (not
  LocalTalk), *More…*: *Server* (or *Manually* with the values below),
  and under *Domain Name Server Information* the domain `.` with
  10.0.2.3, *Default* ticked; then **restart the Mac** (MacTCP only uses
  a new setting after a restart). With Open Transport: *Ethernet* and
  *Using DHCP Server*. Or set it by hand:

  | | |
  |---|---|
  | IP address | 10.0.2.15 |
  | Subnet mask | 255.255.255.0 |
  | Router (gateway) | 10.0.2.2 |
  | Name server (DNS) | 10.0.2.3 |

- The router answers DHCP and BootP, ARP and ping (10.0.2.2), passes DNS
  to the Pi's name server, and carries UDP and TCP through the Pi's
  sockets. AppleTalk still goes through the tunnel at the same time.
  Connections into the Mac from outside are not possible, and pings
  beyond 10.0.2.2 get no answer.
- Most web sites today need HTTPS with modern encryption, which browsers
  of the time cannot do: the **HTTPS helper** below bridges that. FTP
  downloads work directly.
- `mac_internet=off` in `macemu.cfg` turns the router off.
- Checked by a test that plays the Mac's part through the router to real
  servers: ARP, DHCP, ping, UDP, DNS and a 200 KB TCP transfer with
  acknowledgements and closing. And with Mac OS 7.5.3 itself
  (`ENET_TEST=1` in the host runner): the Slot Manager finds the built-in
  Ethernet, its `.ENET` driver opens and sends, and MacTCP 2.0.6 set to
  Ethernet and *Server* gets 10.0.2.15 from the router after a restart.
  Not yet a page loaded in a browser through MacTCP, nor on a real Pi.

**1.44 MB floppies (Quadra)**
- The Quadra's drives are SuperDrives (FDHD): 1.44 MB MFM disks as well as
  400K/800K GCR. The SWIM's ISM half is emulated (mode switch, registers,
  parameter RAM, FIFO, marks and CRCs) and MFM tracks are built from the
  image on the fly; writes, including formatting, go back into the image.
  Checked with System 7.1: a 1.44 MB disk mounts, files open, and edits
  saved to it are in the image after ejecting.

**CD-ROM**
- A CD-ROM drive at SCSI ID 3 that behaves like an AppleCD 300. Choose an
  `.iso`, `.toast` or `.cdr` image on the F12 menu's *Hard disks and CD-ROM* page; the
  Mac can eject it. Mac OS needs Apple's *Apple CD-ROM* extension, as a
  real Mac does (with *Foreign File Access* and *ISO 9660 File Access* for
  PC CDs).

**Files in and out of Mac disks**
- On the web page, *[open]* next to a disk image shows its folders and files;
  download any file as MacBinary (`.bin`: both forks, type and creator) or
  just its data fork.
- *Send files to the Mac*: drop files of any size on the page. With the
  shared folder on (Mac OS 7.5 and later) they go on the Mac's **Shared**
  disk, and MacBinary files are unpacked there into the Mac file they hold
  (icon, resource fork, type and creator kept). Otherwise, or with *Put
  them on a transfer floppy or CD instead* ticked: small ones
  arrive on a new floppy ("Transfer 1", ...) in floppy drive 2; bigger ones
  (up to 2 GB) on a new CD-ROM disk (`Transfer 1.toast`, ...) put straight
  in the CD-ROM drive (`cdrom_id`, SCSI ID 3 by default; the Mac needs the
  Apple CD-ROM extension, as 7.5 has). Drag the files to the hard disk.
  MacBinary files (`.bin`) keep their icons, resource forks, type, creator
  and Finder flags; `.txt` files get Mac line endings. Up to 64 files at a
  time. Eject a transfer CD (drag it to the Trash) before sending the next.
  The Finder may say it could not rebuild the CD's desktop file: click OK,
  the files are all there (it says the same of CDs made by other tools).

**Web file manager and software update**
- Open `http://macemu.local/` in a browser (or the address shown at the
  top of the F12 menu). Works on phones, and follows the system's dark mode.
- *This Mac* shows what the Mac is doing (model and speed, the disks in
  each drive, the shared disk, the network), refreshed every few seconds,
  with a button to restart it.
- *Clipboard* (Quadra, any System 7): text copied on the Mac shows on the
  page; text typed or pasted there goes onto the Mac's clipboard, for
  Command-V (accents, curly quotes and such converted; characters Mac Roman
  lacks become `?`; up to 16 KB). Some programs only see a new clipboard
  after you switch to another program and back, as on a real Mac.
- *Insert* next to a floppy or CD image in the disks folder puts it in a
  drive; *Eject* next to a drive in *This Mac* takes it out (a CD only once
  the Mac has let go of it: drag it to the Trash first).
- Upload (drag and drop, up to 4 GB, with a progress bar per file),
  download and delete disk images and ROMs, and see the card's free space.
- *Update the emulator*: drop a new `MacEmulator-SD.zip` on the page. The
  browser unpacks it and sends the new kernels, firmware and start-up files;
  then the Pi restarts. Your settings, ROMs, PRAM and disks are kept.
- Disks the Mac is using cannot be replaced or deleted, the Pi's start-up
  files can only be replaced by an update, and `macemu.cfg` and the Wi-Fi
  files (which hold the password) are never shown or sent. Uploads go to a
  temporary file first, so a broken transfer never leaves half a file.
- Transfers are fastest while the F12 menu is open (the Mac is paused).

**Shared folder (Quadra, Mac OS 7.5 and later)**
- The folder `shared` on the card is a disk named **Shared** on the Mac's
  desktop, read and written by both sides. Put files in it from the web page
  (*Send files to the Mac*) or a computer and they are on the
  Mac within seconds, an open window included; save or copy files to it on
  the Mac and they are on the card. Any size, folders too.
- Resource forks and Finder info (type, creator, icons, positions) go in
  AppleDouble `._name` files beside each file, as macOS keeps them on cards
  like this, so applications copied in from a modern Mac or out to one keep
  them. A file without one gets its type from its name (`.txt` is TEXT ...).
- Names: Mac characters as Unicode on the card; ones the card cannot hold
  (`/ ? * : " < > |`, a leading or trailing dot) as `%XX`. Names longer
  than the Mac's 31 characters are shortened with a `#` and a code.
- The Finder keeps its desktop file there (`Desktop`, `._Desktop`).
- `shared_folder=` in `macemu.cfg` (a folder name, or `none`). It needs the
  File System Manager of Mac OS 7.5; on older systems the log says so.
- How: at the first WaitNextEvent the emulator registers a file system with
  Mac OS's File System Manager, installs a small driver for its drive and
  mounts the volume; the File Manager then hands every call for it to the
  emulator (src/core/sharedfs.cpp). The host runner's `--share DIR` with
  `SHARE_TEST=1` checks it through Mac OS's own File Manager (42 checks:
  both forks, Finder info, folders, moving, renaming, deleting, odd names,
  files made on the host).

**Hibernate (Quadra)**
- *Hibernate: save the Mac, turn off* on the menu's main page freezes the
  running Mac (memory, CPU and every chip) to `macemu.state` on the card and
  turns the Pi off. The next start carries on exactly where it was, with
  the same windows and programs open, in a few seconds. The state is used
  once (the disks change as soon as the Mac runs again) and only with the
  same emulator version, ROM, memory size and hard disks; otherwise the Mac
  starts normally and the log says why. It takes about as much room on the
  card as the Mac has memory.

**Saved Macs (Quadra)**
- Four slots on the menu's *Saved Macs* page, each a snapshot: the running
  Mac and copies of its hard disks and floppies, in `saved/slotN/` on the
  card. *Save the Mac as it is now* keeps it in a slot (the Mac carries on);
  *Restore* brings it back, disks and all, so the Mac may be used, other
  slots restored, and so on, in between. A 500 MB disk takes about as much
  room again per slot, and saving or restoring copies it (the screen shows
  how far it is). Disks are put back beside the ones there and only then
  in their place, so losing the power meanwhile harms nothing.
- Saved states (these and Hibernate) load in later versions of the
  emulator too, as long as the way they are written stays the same
  (`kStateFormat` in `src/core/state.h`; a test fails when it must change).

**JIT (being tested)**
- *JIT* on the menu's preferences page, or `jit=on` in `macemu.cfg` (64-bit
  kernels, Quadra; it switches when the menu closes): the 68040's code runs as
  blocks of ARM code. Every block does exactly what the interpreter would,
  cycle for cycle, so the Mac behaves identically (the 7.1 and 7.5.3 boots
  give the same disk and screen bit for bit); only the speed differs. The
  log's `jit=` shows the blocks made, the share of instructions run in
  blocks and the cache flushes. The commonest instructions (MOVE, MOVEQ,
  ADDQ/SUBQ, LEA, TST, CLR, CMP/ADD/SUB/AND/OR/EOR and their immediate and
  address forms) become ARM code of their own, with the 68040's memory
  access done inline through the CPU's TLB; the rest call the
  interpreter's handlers. `make jittest` checks every one of those opcodes
  against the interpreter from random states (on ARM, or through
  qemu-aarch64), down to each flag, cycle and byte of memory.
- On the Pi 3, 4 and 5 the translating is done on core 3, so the Mac's core
  never waits for it: it records a run of instructions and queues it; core
  3 makes the ARM code; the Mac's core puts it in place once its code is
  checked to be unchanged. The log's `core3=` shows blocks queued, dropped
  (queue full: recorded again later) and stale (the code changed meanwhile).
  `make jittest` runs the checks both ways (`JIT_BG=1`: a second thread).

**Multi-core**
- On every Pi with more than one core, the emulated Mac has core 1 to
  itself and runs without interruption. Core 0 does everything else: USB
  keyboard and mouse, Wi-Fi/Ethernet and the web page, the screen (a steady
  60 frames a second, whatever the Mac's speed), sound and the SD card.
  The Mac's disk accesses are handed to core 0, which serves them at once.
- The Pi 1 and Zero (one core) run everything on that core, as before.

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
               SD card (FatFs), the Mac's own core, pacing, hot keys,
               web file manager, Wi-Fi, power keys
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
- The serial ports' SCC works, but nothing is connected to it: LocalTalk
  finds no network (use EtherTalk on the Quadra).
- Virtual memory does not start yet: with it switched on in the Memory
  control panel, System 7.1 starts without it (and makes no swap file).
  The 68040's restartable access faults that it needs are in place: a
  faulting instruction leaves the registers as it found them, writes
  nothing, and runs again after the handler returns (checked by a test with
  real page tables). The registers are only saved before each instruction
  once a page fault has been seen, so this costs nothing otherwise.
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

BearSSL (`third_party/bearssl`, TLS for the HTTPS helper) is MIT licensed
(`third_party/bearssl/LICENSE.txt`); its version is in
`third_party/bearssl/VERSION`. The root certificates in
`sdcard/certs/cacert.pem` are Mozilla's, extracted by the curl project, under
the Mozilla Public License 2.0.
