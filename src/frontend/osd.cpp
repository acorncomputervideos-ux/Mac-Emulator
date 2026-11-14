#include "osd.h"
#include "util.h"

namespace macemu {

static const u32 kRamSizes[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072};
static const int kVideoModes[][2] = {{640, 480}, {832, 624}, {1024, 768}};
static const char *kNetModes[] = {"auto", "ethernet", "wifi", "off"};
static const char *kCountries[] = {"GB", "US", "IE", "DE", "FR", "ES", "IT", "NL", "BE", "CH", "AT", "SE",
                                   "NO", "DK", "FI", "PL", "PT", "AU", "NZ", "CA", "JP"};
static const int kCountryCount = (int)(sizeof kCountries / sizeof kCountries[0]);

Osd::Osd(Config &config, OsdHost &h) : cfg(config), host(h)
{
    isOpen = dirty = pendingReboot = disksChanged = false;
    page = PageMain;
    for (int i = 0; i < 7; i++) sel[i] = 0;
    pickTarget = PickFloppy1;
    pickId = 0;
    pickCount = pickSel = pickTop = 0;
    message[0] = 0;
    editItem = -1;
    editBuf[0] = 0;
    confirmItem = -1;
    newHighDensity = false;
}

void Osd::open()
{
    isOpen = true;
    dirty = true;
    page = PageMain;
    message[0] = 0;
    editItem = -1;
    confirmItem = -1;
}

void Osd::say(const char *text)
{
    strCopy(message, text, sizeof message);
}

int Osd::items(Page p, int *out) const
{
    static const int main[] = {ItHeadDisks, ItFloppy1, ItFloppy2, ItDisksPage, ItNewFloppy,
                               ItHeadSettings, ItMachinePage, ItPrefsPage, ItNetworkPage,
                               ItHeadMac, ItReset, ItInterrupt, ItScreenshot, ItSave, ItSavedPage, ItHibernate, ItPowerOff, ItResume};
    static const int disks[] = {ItHeadScsi, ItHd0, ItHd0 + 1, ItHd0 + 2, ItHd0 + 3, ItHd0 + 4, ItHd0 + 5, ItHd6,
                                ItHeadCd, ItCdrom, ItDisksBack};
    static const int machine[] = {ItModel, ItRom, ItRam, ItVideo, ItMonitor, ItScaling, ItApply, ItMachineBack};
    static const int prefs[] = {ItHeadEmulation, ItSpeed, ItJit, ItHeadDisplay, ItPalette, ItSound, ItVolume, ItSoundOut,
                                ItHeadInput, ItMouse, ItMouseMode, ItAlt, ItPrefsBack};
    static const int net[] = {ItHeadConnection, ItNetMode, ItHeadWifi, ItWifiScan, ItWifiName, ItWifiPass, ItWifiCountry,
                              ItKeyboard, ItHeadSharing, ItMacInternet, ItHttps, ItShared, ItNetApply, ItNetworkBack};
    static const int saved[] = {ItHeadRestore, ItLoad1, ItLoad1 + 1, ItLoad1 + 2, ItLoad4,
                                ItHeadSaveTo, ItSave1, ItSave1 + 1, ItSave1 + 2, ItSave4, ItSavedBack};
    const int *list = p == PageMain ? main : p == PageDisks ? disks : p == PageMachine ? machine : p == PagePrefs ? prefs
                    : p == PageSaved ? saved : net;
    int n = p == PageMain ? (int)(sizeof main / sizeof main[0]) : p == PageDisks ? (int)(sizeof disks / sizeof disks[0])
          : p == PageMachine ? (int)(sizeof machine / sizeof machine[0]) : p == PagePrefs ? (int)(sizeof prefs / sizeof prefs[0])
          : p == PageSaved ? (int)(sizeof saved / sizeof saved[0]) : (int)(sizeof net / sizeof net[0]);
    for (int i = 0; i < n; i++) out[i] = list[i];
    return n;
}

void Osd::move(int dir)
{
    int list[kMaxItems];
    int n = items(page, list);
    int &s = sel[page];
    if (s < 0 || s >= n) s = 0;
    for (int k = 0; k < n; k++) {
        s += dir;
        if (s >= n) s = 0;
        if (s < 0) s = n - 1;
        if (!heading(list[s])) return;
    }
}

void Osd::selectItem(Page p, int item)
{
    int list[kMaxItems];
    int n = items(p, list);
    for (int i = 0; i < n; i++) if (list[i] == item) sel[p] = i;
}

static int cycle(int v, int dir, int lo, int hi)
{
    v += dir;
    if (v > hi) v = lo;
    if (v < lo) v = hi;
    return v;
}

static const char *baseName(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

void Osd::adjust(int item, int dir)
{
    switch (item) {
    case ItSpeed: {
        static const int steps[] = {1, 2, 4, 8, 0};
        int i = 0;
        while (i < 4 && steps[i] != cfg.speed) i++;
        cfg.speed = steps[cycle(i, dir, 0, 4)];
        break;
    }
    case ItJit: cfg.jit = !cfg.jit; break;
    case ItSoundOut: {
        static const char *outs[] = {"auto", "usb", "hdmi", "pwm"};
        int i = 0;
        while (i < 3 && !strEqNoCase(outs[i], cfg.soundDevice)) i++;
        strCopy(cfg.soundDevice, outs[cycle(i, dir, 0, 3)], sizeof cfg.soundDevice);
        break;
    }
    case ItNewFloppy: newHighDensity = !newHighDensity; return;
    case ItPalette: cfg.palette = cycle(cfg.palette, dir, 0, kPaletteCount - 1); break;
    case ItSound: cfg.sound = !cfg.sound; break;
    case ItVolume:
        cfg.volume += dir;
        if (cfg.volume < 0) cfg.volume = 0;
        if (cfg.volume > 10) cfg.volume = 10;
        break;
    case ItMouse:
        cfg.mouseSpeed += dir;
        if (cfg.mouseSpeed < 1) cfg.mouseSpeed = 1;
        if (cfg.mouseSpeed > 8) cfg.mouseSpeed = 8;
        break;
    case ItAlt: cfg.altIsOption = !cfg.altIsOption; break;
    case ItMouseMode: cfg.directMouse = !cfg.directMouse; break;
    case ItModel: {
        static const char *models[] = {"auto", "plus", "ii", "quadra"};
        int i = 0;
        while (i < 3 && !strEqNoCase(models[i], cfg.machine)) i++;
        strCopy(cfg.machine, models[cycle(i, dir, 0, 3)], sizeof cfg.machine);
        pendingReboot = true;
        return;
    }
    case ItRam: {
        int i = 0, n = (int)(sizeof kRamSizes / sizeof kRamSizes[0]);
        while (i < n - 1 && kRamSizes[i] < cfg.ramKB) i++;
        cfg.ramKB = kRamSizes[cycle(i, dir, 0, n - 1)];
        pendingReboot = true;
        return;
    }
    case ItNetMode: {
        int i = 0;
        while (i < 3 && !strEqNoCase(kNetModes[i], cfg.network)) i++;
        strCopy(cfg.network, kNetModes[cycle(i, dir, 0, 3)], sizeof cfg.network);
        pendingReboot = true;
        return;
    }
    case ItWifiCountry: {
        int i = 0;
        while (i < kCountryCount - 1 && !strEqNoCase(kCountries[i], cfg.wifiCountry)) i++;
        strCopy(cfg.wifiCountry, kCountries[cycle(i, dir, 0, kCountryCount - 1)], sizeof cfg.wifiCountry);
        pendingReboot = true;
        return;
    }
    case ItScaling:
        cfg.sharp = cycle(cfg.sharp, dir, 0, 2);
        pendingReboot = true;
        return;
    case ItMonitor:
        strCopy(cfg.monitor, strEq(cfg.monitor, "21") ? "vga" : "21", sizeof cfg.monitor);
        pendingReboot = true;
        return;
    case ItMacInternet:
        cfg.macInternet = !cfg.macInternet;
        pendingReboot = true;
        return;
    case ItHttps:
        cfg.httpsHelper = !cfg.httpsHelper;
        pendingReboot = true;
        return;
    case ItShared:
        strCopy(cfg.sharedFolder, cfg.sharedFolder[0] ? "" : "shared", sizeof cfg.sharedFolder);
        pendingReboot = true;
        return;
    case ItKeyboard:
        strCopy(cfg.keyboard, strEqNoCase(cfg.keyboard, "uk") ? "us" : "uk", sizeof cfg.keyboard);
        return;
    case ItVideo: {
        int i = 0;
        while (i < 2 && kVideoModes[i][0] != cfg.videoWidth) i++;
        i = cycle(i, dir, 0, 2);
        cfg.videoWidth = kVideoModes[i][0];
        cfg.videoHeight = kVideoModes[i][1];
        pendingReboot = true;
        return;
    }
    default:
        return;
    }
    host.settingsChanged();
}

void Osd::openPicker(PickTarget target, int id)
{
    pickTarget = target;
    pickId = id;
    OsdHost::FileKind kind = target == PickHardDisk ? OsdHost::HardDisks : target == PickRom ? OsdHost::Roms
                           : target == PickWifi ? OsdHost::WifiNetworks : target == PickCd ? OsdHost::CdImages
                           : OsdHost::Floppies;
    pickCount = host.listFiles(kind, names, kMaxFiles);
    for (int i = 1; i < pickCount && target != PickWifi; i++) {   // alphabetical
        char tmp[OsdHost::kNameLen];
        strCopy(tmp, names[i], sizeof tmp);
        int j = i - 1;
        while (j >= 0) {
            const char *a = names[j], *b = tmp;
            while (*a && lower(*a) == lower(*b)) { a++; b++; }
            if (lower(*a) <= lower(*b)) break;
            strCopy(names[j + 1], names[j], sizeof names[0]);
            j--;
        }
        strCopy(names[j + 1], tmp, sizeof names[0]);
    }
    pickSel = pickTop = 0;
    page = PagePicker;
}

void Osd::activate(int item)
{
    // What cannot be undone asks first: Enter once more to do it.
    bool isLoad = item >= ItLoad1 && item <= ItLoad4, isSave = item >= ItSave1 && item <= ItSave4;
    if (isLoad && !host.slotInfo(item - ItLoad1)[0]) { say("Nothing saved there yet"); return; }
    if (item == ItReset || item == ItHibernate || item == ItPowerOff || isLoad || isSave) {
        if (confirmItem != item) {
            confirmItem = item;
            say(item == ItReset ? "Press Enter again to restart the Mac"
                : item == ItHibernate ? "Press Enter again to save the Mac and turn off"
                : isLoad ? "Enter again: the Mac and its disks go back"
                : isSave ? (host.slotInfo(item - ItSave1)[0] ? "Enter again to save here (replaces it)" : "Press Enter again to save the Mac here")
                : "Press Enter again to turn off the Pi");
            return;
        }
        confirmItem = -1;
        message[0] = 0;
    }
    if (isSave) {
        say(host.saveSlot(item - ItSave1) ? "Saved" : "Could not save it (Quadra only; is the card full?)");
        return;
    }
    if (isLoad) {
        char why[64];
        if (host.loadSlot(item - ItLoad1, why, sizeof why)) { isOpen = false; page = PageMain; }
        else say(why);
        return;
    }
    switch (item) {
    case ItNewFloppy: {
        char name[OsdHost::kNameLen];
        if (host.newFloppy(newHighDensity, name, sizeof name)) {
            char m[64] = "Made ";
            strAppend(m, name, sizeof m);
            strAppend(m, ": it is in a floppy drive", sizeof m);
            say(m);
        } else {
            say(newHighDensity ? "Could not make it (1.44 MB needs a Quadra)" : "Could not make it (is the card full?)");
        }
        break;
    }
    case ItFloppy1: openPicker(PickFloppy1, 0); break;
    case ItFloppy2: openPicker(PickFloppy2, 1); break;
    case ItDisksPage: page = PageDisks; break;
    case ItMachinePage: page = PageMachine; break;
    case ItPrefsPage: page = PagePrefs; break;
    case ItNetworkPage: page = PageNetwork; break;
    case ItSavedPage: page = PageSaved; break;
    case ItReset:
        host.resetMachine();
        disksChanged = false;
        isOpen = false;
        break;
    case ItInterrupt: host.interruptButton(); isOpen = false; break;
    case ItScreenshot: say(host.screenshot() ? "Screenshot saved" : "Could not save the screenshot"); break;
    case ItSave: say(host.saveSettings() ? "Settings saved to macemu.cfg" : "Could not write macemu.cfg"); break;
    case ItResume: isOpen = false; break;
    case ItHibernate: if (!host.hibernate()) say("Could not save the Mac (Quadra only; is the card full?)"); break;
    case ItPowerOff: host.powerOff(); break;
    case ItRom: openPicker(PickRom, 0); break;
    case ItWifiScan: openPicker(PickWifi, 0); break;
    case ItCdrom: openPicker(PickCd, 0); break;
    case ItApply: case ItNetApply: host.saveAndReboot(); break;
    case ItWifiName: case ItWifiPass:
        editItem = item;
        strCopy(editBuf, item == ItWifiName ? cfg.wifiSsid : cfg.wifiPassword, sizeof editBuf);
        break;
    case ItDisksBack: case ItMachineBack: case ItPrefsBack: case ItNetworkBack: case ItSavedBack:
        page = PageMain;
        break;
    default:
        if (item >= ItHd0 && item <= ItHd6) openPicker(PickHardDisk, item - ItHd0);
        else adjust(item, 1);
        break;
    }
}

// Entry 0 of the picker is "none" (eject / no disk); files follow.
void Osd::pickerKey(Nav nav)
{
    int total = pickCount + 1;
    Page back = (pickTarget == PickHardDisk || pickTarget == PickCd) ? PageDisks : pickTarget == PickRom ? PageMachine
              : pickTarget == PickWifi ? PageNetwork : PageMain;
    switch (nav) {
    case NavUp: pickSel = cycle(pickSel, -1, 0, total - 1); break;
    case NavDown: pickSel = cycle(pickSel, 1, 0, total - 1); break;
    case NavPageUp: pickSel = pickSel > kVisible ? pickSel - kVisible : 0; break;
    case NavPageDown: pickSel = pickSel + kVisible < total ? pickSel + kVisible : total - 1; break;
    case NavBack: case NavLeft: page = back; return;
    case NavSelect: case NavRight: {
        const char *name = pickSel ? names[pickSel - 1] : "";
        message[0] = 0;
        switch (pickTarget) {
        case PickFloppy1: case PickFloppy2:
            host.ejectDisk(pickId);
            if (pickSel && !host.insertDisk(pickId, name)) say("Not a usable floppy (400K, 800K; 1.44 MB on a Quadra)");
            break;
        case PickHardDisk:
            strCopy(cfg.hd[pickId], pickSel ? name : "none", Config::kPath);
            disksChanged = true;
            say("Restart the Mac to use the new disks");
            break;
        case PickCd:
            if (pickSel && !host.insertCd(name)) say("Cannot open that CD image");
            if (!pickSel) host.insertCd(0);
            break;
        case PickWifi:
            if (!pickSel) break;
            strCopy(cfg.wifiSsid, name, sizeof cfg.wifiSsid);
            if (strEqNoCase(cfg.network, "off") || strEqNoCase(cfg.network, "ethernet"))
                strCopy(cfg.network, "auto", sizeof cfg.network);
            pendingReboot = true;
            page = back;
            selectItem(PageNetwork, ItWifiPass);                      // straight on to the password
            editItem = ItWifiPass;
            editBuf[0] = 0;
            return;
        case PickRom:
            if (!pickSel) break;
            strCopy(cfg.rom, name, Config::kPath);
            pendingReboot = true;
            break;
        }
        page = back;
        return;
    }
    }
    if (pickSel < pickTop) pickTop = pickSel;
    if (pickSel >= pickTop + kVisible) pickTop = pickSel - kVisible + 1;
}

void Osd::typeChar(char c)
{
    if (!editing()) return;
    dirty = true;
    u32 n = strLen(editBuf);
    if (c == '\b') {
        if (n) editBuf[n - 1] = 0;
    } else if (c >= 32 && c < 127 && n + 1 < sizeof editBuf) {
        editBuf[n] = c;
        editBuf[n + 1] = 0;
    }
}

void Osd::key(Nav nav)
{
    if (!isOpen) return;
    dirty = true;
    if (editItem >= 0) {
        if (nav == NavSelect) {
            if (editItem == ItWifiName) strCopy(cfg.wifiSsid, editBuf, sizeof cfg.wifiSsid);
            else strCopy(cfg.wifiPassword, editBuf, sizeof cfg.wifiPassword);
            if (editItem == ItWifiName && editBuf[0] && strEqNoCase(cfg.network, "off"))
                strCopy(cfg.network, "auto", sizeof cfg.network);
            pendingReboot = true;
            editItem = -1;
        } else if (nav == NavBack) {
            editItem = -1;
        } else if (nav == NavLeft) {
            typeChar('\b');
        }
        return;
    }
    if (page == PagePicker) { pickerKey(nav); return; }
    if (nav != NavSelect) confirmItem = -1;
    int list[kMaxItems];
    int n = items(page, list);
    int &s = sel[page];
    if (s < 0 || s >= n || heading(list[s])) { s = n - 1; move(1); }
    switch (nav) {
    case NavUp: move(-1); message[0] = 0; break;
    case NavDown: move(1); message[0] = 0; break;
    case NavPageUp: s = n - 1; move(1); break;
    case NavPageDown: s = 0; move(-1); break;
    case NavLeft: adjust(list[s], -1); break;
    case NavRight: adjust(list[s], 1); break;
    case NavSelect: activate(list[s]); break;
    case NavBack:
        if (page == PageMain) isOpen = false;
        else page = PageMain;
        break;
    }
}

void Osd::label(int item, char *out, u32 max)
{
    out[0] = 0;
    auto field = [&](const char *name, const char *value) {
        strAppend(out, name, max);
        for (u32 n = strLen(name); n < 18; n++) strAppend(out, " ", max);
        strAppend(out, value, max);
    };
    char v[64];
    v[0] = 0;
    switch (item) {
    case ItFloppy1: case ItFloppy2: {
        const char *n = host.diskName(item - ItFloppy1);
        field(item == ItFloppy1 ? "Floppy drive 1" : "Floppy drive 2", n[0] ? baseName(n) : "(empty)");
        break;
    }
    case ItDisksPage: strAppend(out, "Hard disks and CD-ROM  >", max); break;
    case ItNewFloppy: field("New blank floppy", newHighDensity ? "1.44 MB  (Enter: make)" : "800K  (Enter: make)"); break;
    case ItMachinePage: strAppend(out, "Machine, ROM and memory  >", max); break;
    case ItPrefsPage: strAppend(out, "Speed, display, sound, input  >", max); break;
    case ItNetworkPage: strAppend(out, "Network and sharing  >", max); break;
    case ItMacInternet: field("Mac's internet", cfg.macInternet ? "On (through the Pi)" : "Off"); break;
    case ItHttps:
        field("HTTPS helper", !cfg.macInternet ? "(needs internet)" : cfg.httpsHelper ? "On (old browsers)" : "Off");
        break;
    case ItShared:
        if (cfg.sharedFolder[0]) { strCopy(v, "On (folder ", sizeof v); strAppend(v, cfg.sharedFolder, sizeof v); strAppend(v, ")", sizeof v); }
        else strCopy(v, "Off", sizeof v);
        field("Shared disk", v);
        break;
    case ItHeadDisks: strAppend(out, "DISKS", max); break;
    case ItSavedPage: strAppend(out, "Saved Macs (snapshots)  >", max); break;
    case ItHeadRestore: strAppend(out, "RESTORE (the Mac now running is lost)", max); break;
    case ItHeadSaveTo: strAppend(out, "SAVE THE MAC AS IT IS NOW", max); break;
    case ItHeadSettings: strAppend(out, "SETTINGS", max); break;
    case ItHeadMac: strAppend(out, "THE MAC", max); break;
    case ItHeadScsi: strAppend(out, "SCSI HARD DISKS (restart the Mac to change)", max); break;
    case ItHeadCd: strAppend(out, "CD-ROM (changes at once)", max); break;
    case ItHeadEmulation: strAppend(out, "EMULATION", max); break;
    case ItHeadDisplay: strAppend(out, "DISPLAY AND SOUND", max); break;
    case ItHeadInput: strAppend(out, "MOUSE AND KEYBOARD", max); break;
    case ItHeadConnection: strAppend(out, "THE PI'S CONNECTION", max); break;
    case ItHeadWifi: strAppend(out, "WI-FI", max); break;
    case ItHeadSharing: strAppend(out, "FOR THE MAC (after a restart)", max); break;
    case ItNetMode: {
        const char *m = strEqNoCase(cfg.network, "ethernet") ? "Ethernet cable" : strEqNoCase(cfg.network, "wifi") ? "Wi-Fi"
                      : strEqNoCase(cfg.network, "off") ? "Off" : "Automatic";
        field("Connection", m);
        break;
    }
    case ItWifiName: case ItWifiPass: {
        const char *name = item == ItWifiName ? "Wi-Fi name" : "Wi-Fi password";
        if (editItem == item) {
            // Show the end of what is being typed, with a cursor.
            u32 room = max > 20 ? max - 20 : 1;
            const char *t = editBuf;
            while (strLen(t) + 1 > room) t++;
            strCopy(v, t, sizeof v);
            strAppend(v, "_", sizeof v);
        } else if (item == ItWifiName) {
            strCopy(v, cfg.wifiSsid[0] ? cfg.wifiSsid : "(not set)", sizeof v);
        } else {
            strCopy(v, cfg.wifiPassword[0] ? "********" : "(none: open network)", sizeof v);
        }
        field(name, v);
        break;
    }
    case ItWifiCountry: field("Wi-Fi country", cfg.wifiCountry); break;
    case ItCdrom: {
        char name[20] = "Drive (SCSI ID ";
        appendUint(name, (u32)(cfg.cdromId < 0 ? 3 : cfg.cdromId), sizeof name);
        strAppend(name, ")", sizeof name);
        const char *n = host.cdName();
        field(name, n[0] ? baseName(n) : "(empty)");
        break;
    }
    case ItWifiScan: strAppend(out, "Choose Wi-Fi network (search)  >", max); break;
    case ItKeyboard: field("Typing layout", strEqNoCase(cfg.keyboard, "us") ? "US" : "UK"); break;
    case ItNetApply: strAppend(out, pendingReboot ? "Save and restart now" : "Save and restart", max); break;
    case ItReset: strAppend(out, disksChanged ? "Restart the Mac (attach new disks)" : "Restart the Mac", max); break;
    case ItInterrupt: strAppend(out, "Press the interrupt switch", max); break;
    case ItScreenshot: strAppend(out, "Save a screenshot", max); break;
    case ItSave: strAppend(out, "Save settings", max); break;
    case ItResume: strAppend(out, "Resume", max); break;
    case ItHibernate: strAppend(out, "Hibernate: save the Mac, turn off", max); break;
    case ItPowerOff: strAppend(out, "Turn off the Pi", max); break;
    case ItModel: {
        const char *m = strEqNoCase(cfg.machine, "plus") ? "Macintosh Plus" : strEqNoCase(cfg.machine, "ii") ? "Macintosh II"
                      : strEqNoCase(cfg.machine, "quadra") ? "Quadra 800" : "From the ROM";
        field("Model", m);
        break;
    }
    case ItRom: field("ROM file", cfg.rom[0] ? baseName(cfg.rom) : "(mac.rom or similar)"); break;
    case ItRam:
        if (cfg.ramKB >= 1024) { appendUint(v, cfg.ramKB / 1024, sizeof v); strAppend(v, " MB", sizeof v); }
        else { appendUint(v, cfg.ramKB, sizeof v); strAppend(v, " KB", sizeof v); }
        field("Memory", v);
        break;
    case ItVideo:
        appendUint(v, (u32)cfg.videoWidth, sizeof v);
        strAppend(v, " x ", sizeof v);
        appendUint(v, (u32)cfg.videoHeight, sizeof v);
        field("Mac II screen", v);
        break;
    case ItScaling:
        field("Screen scaling", cfg.sharp == 2 ? "Scanlines (like a CRT)" : cfg.sharp ? "Sharp (whole pixels)" : "Smooth (fills screen)");
        break;
    case ItMonitor: field("Quadra monitor", strEq(cfg.monitor, "21") ? "21-inch, 1152 x 870" : "VGA, 640 x 480 / 800 x 600"); break;
    case ItApply: strAppend(out, pendingReboot ? "Apply: save and restart now" : "Save and restart", max); break;
    case ItSpeed:
        if (cfg.speed) { appendUint(v, (u32)cfg.speed, sizeof v); strAppend(v, "x", sizeof v); }
        else strAppend(v, "Unlimited", sizeof v);
        field("Speed", v);
        break;
    case ItJit: field("JIT (Quadra)", cfg.jit ? "On" : "Off"); break;
    case ItPalette: field("B&W colours", paletteName(cfg.palette)); break;
    case ItSound: field("Sound", cfg.sound ? "On" : "Off"); break;
    case ItVolume: appendUint(v, (u32)cfg.volume, sizeof v); field("Volume", v); break;
    case ItSoundOut: {
        const char *o = strEqNoCase(cfg.soundDevice, "usb") ? "USB only" : strEqNoCase(cfg.soundDevice, "hdmi") ? "HDMI"
                      : strEqNoCase(cfg.soundDevice, "pwm") ? "Headphone jack" : "Automatic (USB, HDMI)";
        field("Sound output", o);
        break;
    }
    case ItMouse: appendUint(v, (u32)cfg.mouseSpeed, sizeof v); field("Mouse speed", v); break;
    case ItMouseMode: field("Mouse", cfg.directMouse ? "Direct (smooth)" : "Like a real Mac"); break;
    case ItAlt: field("Alt key", cfg.altIsOption ? "Option (Win = Cmd)" : "Command (Win = Opt)"); break;
    case ItDisksBack: case ItMachineBack: case ItPrefsBack: case ItNetworkBack: case ItSavedBack:
        strAppend(out, "<  Back", max);
        break;
    default:
        if ((item >= ItLoad1 && item <= ItLoad4) || (item >= ItSave1 && item <= ItSave4)) {
            int slot = item >= ItSave1 ? item - ItSave1 : item - ItLoad1;
            char name[12] = "Slot ";
            appendUint(name, (u32)slot + 1, sizeof name);
            const char *info = host.slotInfo(slot);
            for (u32 n = strLen(name); n < 8; n++) strAppend(name, " ", sizeof name);
            strAppend(out, name, max);
            strAppend(out, info[0] ? info : "(empty)", max);
        }
        if (item >= ItHd0 && item <= ItHd6) {
            char name[16] = "SCSI ID ";
            appendUint(name, (u32)(item - ItHd0), sizeof name);
            const char *d = cfg.hd[item - ItHd0];
            char def[24] = "(hd";
            appendUint(def, (u32)(item - ItHd0), sizeof def);
            strAppend(def, ".img if present)", sizeof def);
            field(name, strEqNoCase(d, "none") ? "(none)" : d[0] ? baseName(d) : def);
        }
        break;
    }
}

void Osd::draw(Surface &s, const Font &f)
{
    if (!isOpen) return;
    int fw = f.width(), fh = f.height();
    int cols = s.width / fw - 4;
    if (cols > 52) cols = 52;
    if (cols < 20) cols = 20;
    int list[kMaxItems];
    int n = page == PagePicker ? 0 : items(page, list);
    if (page != PagePicker) {
        int &cur = sel[page];
        if (cur < 0 || cur >= n || heading(list[cur])) { cur = n - 1; move(1); }
    }
    int top = page == PageMain ? 2 : page == PageNetwork ? 3 : 0;     // status lines above the items
    int rows = page == PagePicker ? kVisible + 2 : n + top + (top ? 1 : 0) + 2;   // + message, footer
    int w = (cols + 2) * fw;
    int h = (rows + 2) * fh;
    if (h > s.height) h = s.height;
    int x = (s.width - w) / 2, y = (s.height - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    fillRect(s, x - 2, y - 2, w + 4, h + 4, ColFrame);
    fillRect(s, x, y, w, h, ColMenuBg);
    fillRect(s, x, y, w, fh + fh / 2, ColFrame);
    int tx = x + fw, ty = y + fh / 4;
    char line[96];

    static const char *titles[] = {"Mac Emulator", "Hard disks and CD-ROM", "Machine, ROM and memory",
                                   "Speed, display, sound and input", "Network and sharing", "Saved Macs", ""};
    const char *title = titles[page];
    if (page == PagePicker)
        title = pickTarget == PickHardDisk ? "Choose a hard disk image" : pickTarget == PickRom ? "Choose a ROM"
              : pickTarget == PickCd ? "Choose a CD-ROM image (.iso .toast .cdr)"
              : pickTarget == PickWifi ? "Choose your Wi-Fi network"
              : "Choose a floppy disk";
    drawText(s, f, tx, ty, title, ColSelFg);
    if (page == PageMain) {                                         // the version, at the right of the title
        const char *v = host.infoLine(2);
        int len = (int)strLen(v), room = cols - (int)strLen(title) - 2;
        if (len > 0 && len <= room) drawText(s, f, x + w - fw - len * fw, ty, v, ColSelFg);
    }
    ty = y + 2 * fh;
    const char *footer;

    if (page == PagePicker) {
        int total = pickCount + 1;
        for (int i = 0; i < kVisible && pickTop + i < total; i++) {
            int idx = pickTop + i;
            bool hot = idx == pickSel;
            if (hot) fillRect(s, x + fw / 2, ty, w - fw, fh, ColSelBg);
            strCopy(line, idx == 0 ? (pickTarget == PickRom ? "(keep the current ROM)" : pickTarget == PickWifi ? "(cancel)" : "(none / eject)")
                                   : names[idx - 1], (u32)cols + 1);
            drawText(s, f, tx, ty, line, hot ? ColSelFg : idx == 0 ? ColDim : ColMenuFg);
            ty += fh;
        }
        if (pickCount == 0)
            drawText(s, f, tx, ty + fh, pickTarget == PickWifi ? "No networks found (or no Wi-Fi on this Pi)" : "No matching files on the SD card", ColDim);
        line[0] = 0;
        appendUint(line, (u32)pickCount, sizeof line);
        strAppend(line, pickTarget == PickWifi ? " networks   Enter: choose   Esc: back" : " files   Enter: choose   Esc: back", sizeof line);
        drawText(s, f, tx, y + h - fh - fh / 4, line, ColDim);
        return;
    }

    // Status above the items: the Mac and sharing (main page), the network.
    for (int i = 0; i < top; i++) {
        strCopy(line, page == PageMain ? host.infoLine(i) : host.networkLine(i), (u32)cols + 1);
        drawText(s, f, tx, ty, line, page == PageNetwork && i == 0 ? ColMenuFg : ColDim);
        ty += fh;
    }
    if (top) ty += fh / 2;

    int cur = sel[page];
    for (int i = 0; i < n; i++) {
        label(list[i], line, (u32)(heading(list[i]) ? cols + 1 : cols - 1));
        if (heading(list[i])) {
            drawText(s, f, tx, ty, line, ColDim);
        } else {
            bool hot = i == cur;
            if (hot) fillRect(s, x + fw / 2, ty, w - fw, fh, ColSelBg);
            char indented[100] = "  ";
            strAppend(indented, line, (u32)cols + 1);
            drawText(s, f, tx, ty, indented, hot ? ColSelFg : ColMenuFg);
        }
        ty += fh;
    }
    ty += fh / 4;
    const char *msg = message[0] ? message
                    : editItem >= 0 ? "Type, then Enter to keep or Esc to cancel"
                    : ((page == PageMachine || page == PageNetwork) && pendingReboot) ? "Changes apply after Save and restart" : 0;
    if (msg) drawText(s, f, tx, ty, msg, ColSelBg);

    footer = editItem >= 0 ? "Enter: keep   Esc: cancel   Left: delete"
           : page == PageMain ? "Arrows: move, change   Enter: open   Esc: close"
           : "Arrows: move, change   Enter: open   Esc: back";
    drawText(s, f, tx, y + h - fh - fh / 4, footer, ColDim);
}

} // namespace macemu
