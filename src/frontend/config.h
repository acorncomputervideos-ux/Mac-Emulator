// Emulator settings, stored as "key=value" lines in macemu.cfg.

#ifndef MACEMU_CONFIG_H
#define MACEMU_CONFIG_H

#include "../core/types.h"

namespace macemu {

struct Config {
    enum { kPath = 96, kHardDisks = 7 };

    char rom[kPath];            // ROM file; empty = search the usual names
    char machine[8];            // "auto", "plus", "ii" or "quadra"
    u32 ramKB;                  // 128 KB .. 128 MB (Plus: at most 4096)
    int videoWidth, videoHeight;   // Mac II video card resolution
    char monitor[8];            // Quadra: "vga" (640x480, 800x600) or "21" (1152x870)
    char floppy[2][kPath];      // images inserted at power-on
    char hd[kHardDisks][kPath]; // SCSI IDs 0..6; empty = hdN.img if present
    char diskDir[kPath];        // folder the disk picker browses
    char cdrom[kPath];          // CD-ROM image in the drive, empty = none
    int cdromId;                // SCSI ID of the CD-ROM drive (0..6, -1 = no drive)
    int speed;                  // 1, 2, 4, 8 or 0 = unlimited
    int scale;                  // integer zoom, 0 = largest that fits
    int sharp;                  // 1: whole-number scaling with square pixels; 0: smooth, fills the height
    int width, height;          // framebuffer size, 0 = display's native mode
    int palette;
    int sound;                  // 0 = off
    int volume;                 // 0..10
    char soundDevice[8];        // "hdmi" or "pwm"
    int mouseSpeed;             // 1..8, 4 = 1:1
    int altIsOption;            // 0: Alt=Command, Win=Option; 1: swapped
    int floppyReadOnly;
    char network[12];           // "auto", "ethernet", "wifi" or "off"
    char wifiSsid[64];          // Wi-Fi network name; auto mode uses Wi-Fi when set
    char wifiPassword[64];      // empty = open network
    char wifiCountry[4];        // two-letter country code (radio rules)
    char ipAddress[16];         // fixed IPv4 address, empty = from the router (DHCP)
    char ipNetmask[16];         // with ipAddress; empty = 255.255.255.0
    char ipGateway[16];         // with ipAddress; empty = none
    int macEthernet;            // UDP port carrying the Mac's Ethernet (6066), 0 = off
    int macInternet;
    int httpsHelper;            // 1: the Mac's browsers reach HTTPS sites through the Pi            // 1: TCP/IP from the Mac goes out through the Pi (NAT)
    char macEthernetPeer[40];   // a host broadcasts don't reach (IPv4), or empty
    char keyboard[4];           // "us" or "uk": layout for typing in the menu
    char date[24];              // "YYYY-MM-DD HH:MM" start-up clock, empty = build date

    void setDefaults();
    void parse(const char *text, u32 len);
    u32 serialize(char *out, u32 max) const;   // returns length written
    void sanitize();
    u32 clockSeconds(const char *buildDate, const char *buildTime) const;
};

} // namespace macemu

#endif
