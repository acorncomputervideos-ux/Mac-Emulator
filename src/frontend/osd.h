// The on-screen menu (F12): disk swapping, speed, display and sound
// settings.  It talks to the platform through OsdHost, so the same menu
// runs on the Pi and in the hosted test build.

#ifndef MACEMU_OSD_H
#define MACEMU_OSD_H

#include "config.h"
#include "video.h"

namespace macemu {

class OsdHost {
public:
    enum { kNameLen = 64 };
    virtual ~OsdHost() {}
    // Disk images in the disk folder; returns how many were stored.
    virtual int listDisks(char (*names)[kNameLen], int max) = 0;
    virtual bool insertDisk(int drive, const char *name) = 0;
    virtual void ejectDisk(int drive) = 0;
    virtual const char *diskName(int drive) = 0;        // "" when empty
    virtual void resetMachine() = 0;
    virtual void interruptButton() = 0;
    virtual void settingsChanged() = 0;                 // apply Config to video/sound
    virtual bool saveSettings() = 0;
    virtual bool screenshot() = 0;
    virtual const char *infoLine(int n) = 0;            // n = 0..1, status text
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

private:
    enum { kMaxDisks = 128, kVisible = 12 };
    enum Item {
        ItFloppy1, ItFloppy2, ItSpeed, ItScale, ItPalette, ItSound, ItVolume, ItMouse, ItAlt,
        ItReset, ItInterrupt, ItScreenshot, ItSave, ItResume, kItems
    };

    Config &cfg;
    OsdHost &host;
    bool isOpen, dirty;
    int sel;
    bool picking;
    int pickDrive, pickCount, pickSel, pickTop;
    char names[kMaxDisks][OsdHost::kNameLen];
    char message[64];

    void adjust(int item, int dir);
    void activate(int item);
    void label(int item, char *out, u32 max);
    void openPicker(int drive);
    void pickerKey(Nav nav);
};

} // namespace macemu

#endif
