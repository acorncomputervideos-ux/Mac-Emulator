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
    enum FileKind { Floppies, HardDisks, Roms };
    virtual ~OsdHost() {}
    // Files of one kind on the card; returns how many were stored.
    virtual int listFiles(FileKind kind, char (*names)[kNameLen], int max) = 0;
    virtual bool insertDisk(int drive, const char *name) = 0;
    virtual void ejectDisk(int drive) = 0;
    virtual const char *diskName(int drive) = 0;        // "" when empty
    virtual void resetMachine() = 0;                    // also attaches the chosen hard disks
    virtual void interruptButton() = 0;
    virtual void settingsChanged() = 0;                 // apply Config to video/sound/input
    virtual bool saveSettings() = 0;
    virtual void saveAndReboot() = 0;                   // for model, ROM and memory changes
    virtual bool screenshot() = 0;
    virtual const char *infoLine(int n) = 0;            // n = 0..1, status text
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
    void refresh() { if (isOpen && page == PageNetwork) dirty = true; }

    // Settings that only take effect after a reboot have been changed.
    bool rebootPending() const { return pendingReboot; }

private:
    enum { kMaxFiles = 128, kVisible = 12, kMaxItems = 16 };
    enum Page { PageMain, PageDisks, PageMachine, PagePrefs, PageNetwork, PagePicker };
    enum Item {
        // main
        ItFloppy1, ItFloppy2, ItDisksPage, ItMachinePage, ItPrefsPage, ItNetworkPage,
        ItReset, ItInterrupt, ItScreenshot, ItSave, ItResume,
        // hard disks
        ItHd0, ItHd6 = ItHd0 + 6, ItDisksBack,
        // machine
        ItModel, ItRom, ItRam, ItVideo, ItApply, ItMachineBack,
        // display and sound
        ItSpeed, ItPalette, ItSound, ItVolume, ItMouse, ItAlt, ItPrefsBack,
        // network
        ItNetworkBack,
    };
    enum PickTarget { PickFloppy1, PickFloppy2, PickHardDisk, PickRom };

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

    int items(Page p, int *out) const;
    void label(int item, char *out, u32 max);
    void adjust(int item, int dir);
    void activate(int item);
    void openPicker(PickTarget target, int id);
    void pickerKey(Nav nav);
    void say(const char *text);
};

} // namespace macemu

#endif
