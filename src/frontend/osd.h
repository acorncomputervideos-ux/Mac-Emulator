// The on-screen menu (F12, Pause, Scroll Lock or Menu): disks, machine
// settings, display and sound, network.  It talks to the platform through
// OsdHost, so the same menu runs on the Pi and in the hosted build.

#ifndef MACEMU_OSD_H
#define MACEMU_OSD_H

#include "config.h"
#include "video.h"

namespace macemu {

class OsdHost {
public:
    enum { kNameLen = 64 };
    enum FileKind { Floppies, HardDisks, Roms, WifiNetworks, CdImages };   // Wi-Fi: a scan, strongest first
    virtual ~OsdHost() {}
    // Files of one kind on the card; returns how many were stored.
    virtual int listFiles(FileKind kind, char (*names)[kNameLen], int max) = 0;
    virtual bool insertDisk(int drive, const char *name) = 0;
    virtual void ejectDisk(int drive) = 0;
    virtual const char *diskName(int drive) = 0;        // "" when empty
    virtual bool insertCd(const char *name) = 0;        // null or "": eject
    virtual const char *cdName() = 0;                   // "" when empty
    virtual void resetMachine() = 0;                    // also attaches the chosen hard disks
    virtual void interruptButton() = 0;
    virtual void settingsChanged() = 0;                 // apply Config to video/sound/input
    virtual bool saveSettings() = 0;
    virtual void saveAndReboot() = 0;                   // for model, ROM and memory changes
    virtual void powerOff() = 0;                        // save everything and turn the Pi off
    virtual bool hibernate() = 0;                       // save the running Mac and turn off (false: cannot)
    virtual bool screenshot() = 0;
    // A new initialized floppy (800K, or 1.44 MB) in the disk folder, put in a drive; its name.
    virtual bool newFloppy(bool highDensity, char *name, unsigned max) = 0;
    virtual const char *infoLine(int n) = 0;            // n = 0..1 status lines, 2: version (title bar)
    virtual const char *networkLine(int n) = 0;         // n = 0..2, network status
};

class Osd {
public:
    enum Nav { NavUp, NavDown, NavLeft, NavRight, NavSelect, NavBack, NavPageUp, NavPageDown };

    Osd(Config &config, OsdHost &host);

    bool active() const { return isOpen; }
    void open();
    void close() { isOpen = false; }
    void key(Nav nav);
    void draw(Surface &s, const Font &f);
    bool needsRedraw() { bool d = dirty; dirty = false; return d; }
    // Status that changes by itself (network) is on screen: redraw it.
    void refresh() { if (isOpen && (page == PageNetwork || page == PageMain)) dirty = true; }

    // Text entry (Wi-Fi name and password): while editing() the frontend
    // passes typed characters here ('\b' deletes); Enter keeps, Esc cancels.
    bool editing() const { return isOpen && editItem >= 0; }
    void typeChar(char c);

    // Settings that only take effect after a reboot have been changed.
    bool rebootPending() const { return pendingReboot; }

private:
    enum { kMaxFiles = 128, kVisible = 12, kMaxItems = 24 };
    enum Page { PageMain, PageDisks, PageMachine, PagePrefs, PageNetwork, PagePicker };
    enum Item {
        // main
        ItFloppy1, ItFloppy2, ItDisksPage, ItNewFloppy, ItMachinePage, ItPrefsPage, ItNetworkPage,
        ItReset, ItInterrupt, ItScreenshot, ItSave, ItHibernate, ItPowerOff, ItResume,
        // hard disks
        ItHd0, ItHd6 = ItHd0 + 6, ItCdrom, ItDisksBack,
        // machine
        ItModel, ItRom, ItRam, ItVideo, ItMonitor, ItScaling, ItApply, ItMachineBack,
        // display and sound
        ItSpeed, ItJit, ItPalette, ItSound, ItVolume, ItSoundOut, ItMouse, ItAlt, ItPrefsBack,
        // network
        ItNetMode, ItWifiScan, ItWifiName, ItWifiPass, ItWifiCountry, ItKeyboard,
        ItMacInternet, ItHttps, ItShared, ItNetApply, ItNetworkBack,
        // section headings: shown, never selected
        ItHeadDisks, ItHeadSettings, ItHeadMac, ItHeadScsi, ItHeadCd, ItHeadEmulation, ItHeadDisplay,
        ItHeadInput, ItHeadConnection, ItHeadWifi, ItHeadSharing, ItHeadLast = ItHeadSharing,
    };
    static bool heading(int item) { return item >= ItHeadDisks && item <= ItHeadLast; }
    enum PickTarget { PickFloppy1, PickFloppy2, PickHardDisk, PickRom, PickWifi, PickCd };

    Config &cfg;
    OsdHost &host;
    bool isOpen, dirty, pendingReboot, disksChanged;
    Page page;
    int sel[6];
    PickTarget pickTarget;
    int pickId;
    int pickCount, pickSel, pickTop;
    char names[kMaxFiles][OsdHost::kNameLen];
    char message[64];
    int editItem;                       // -1 when not editing
    int confirmItem;                    // asked "Enter again?" for this item, or -1
    bool newHighDensity;
    char editBuf[64];

    int items(Page p, int *out) const;
    void move(int dir);                 // the next selectable item that way (wraps)
    void selectItem(Page p, int item);
    void label(int item, char *out, u32 max);
    void adjust(int item, int dir);
    void activate(int item);
    void openPicker(PickTarget target, int id);
    void pickerKey(Nav nav);
    void say(const char *text);
};

} // namespace macemu

#endif
