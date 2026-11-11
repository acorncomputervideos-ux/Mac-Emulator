#include "osd.h"
#include "util.h"

namespace macemu {

static const u32 kRamSizes[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072};
static const int kVideoModes[][2] = {{640, 480}, {832, 624}, {1024, 768}};

Osd::Osd(Config &config, OsdHost &h) : cfg(config), host(h)
{
    isOpen = dirty = pendingReboot = disksChanged = false;
    page = PageMain;
    for (int i = 0; i < 6; i++) sel[i] = 0;
    pickTarget = PickFloppy1;
    pickId = 0;
    pickCount = pickSel = pickTop = 0;
    message[0] = 0;
}

void Osd::open()
{
    isOpen = true;
    dirty = true;
    page = PageMain;
    message[0] = 0;
}

void Osd::say(const char *text)
{
    strCopy(message, text, sizeof message);
}

int Osd::items(Page p, int *out) const
{
    int n = 0;
    switch (p) {
    case PageMain:
        for (int i = ItFloppy1; i <= ItResume; i++) out[n++] = i;
        break;
    case PageDisks:
        for (int i = ItHd0; i <= ItDisksBack; i++) out[n++] = i;
        break;
    case PageMachine:
        for (int i = ItModel; i <= ItMachineBack; i++) out[n++] = i;
        break;
    case PagePrefs:
        for (int i = ItSpeed; i <= ItPrefsBack; i++) out[n++] = i;
        break;
    default:
        out[n++] = ItNetworkBack;
        break;
    }
    return n;
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
    OsdHost::FileKind kind = target == PickHardDisk ? OsdHost::HardDisks : target == PickRom ? OsdHost::Roms : OsdHost::Floppies;
    pickCount = host.listFiles(kind, names, kMaxFiles);
    for (int i = 1; i < pickCount; i++) {               // alphabetical
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
    switch (item) {
    case ItFloppy1: openPicker(PickFloppy1, 0); break;
    case ItFloppy2: openPicker(PickFloppy2, 1); break;
    case ItDisksPage: page = PageDisks; break;
    case ItMachinePage: page = PageMachine; break;
    case ItPrefsPage: page = PagePrefs; break;
    case ItNetworkPage: page = PageNetwork; break;
    case ItReset:
        host.resetMachine();
        disksChanged = false;
        isOpen = false;
        break;
    case ItInterrupt: host.interruptButton(); isOpen = false; break;
    case ItScreenshot: say(host.screenshot() ? "Screenshot saved" : "Could not save the screenshot"); break;
    case ItSave: say(host.saveSettings() ? "Settings saved to macemu.cfg" : "Could not write macemu.cfg"); break;
    case ItResume: isOpen = false; break;
    case ItRom: openPicker(PickRom, 0); break;
    case ItApply: host.saveAndReboot(); break;
    case ItDisksBack: case ItMachineBack: case ItPrefsBack: case ItNetworkBack:
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
    Page back = pickTarget == PickHardDisk ? PageDisks : pickTarget == PickRom ? PageMachine : PageMain;
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
            if (pickSel && !host.insertDisk(pickId, name)) say("Not a usable 400K/800K disk image");
            break;
        case PickHardDisk:
            strCopy(cfg.hd[pickId], pickSel ? name : "none", Config::kPath);
            disksChanged = true;
            say("Restart the Mac to use the new disks");
            break;
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

void Osd::key(Nav nav)
{
    if (!isOpen) return;
    dirty = true;
    if (page == PagePicker) { pickerKey(nav); return; }
    int list[kMaxItems];
    int n = items(page, list);
    int &s = sel[page];
    if (s >= n) s = n - 1;
    switch (nav) {
    case NavUp: s = cycle(s, -1, 0, n - 1); message[0] = 0; break;
    case NavDown: s = cycle(s, 1, 0, n - 1); message[0] = 0; break;
    case NavPageUp: s = 0; break;
    case NavPageDown: s = n - 1; break;
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
    case ItDisksPage: strAppend(out, "Hard disks  >", max); break;
    case ItMachinePage: strAppend(out, "Machine, ROM and memory  >", max); break;
    case ItPrefsPage: strAppend(out, "Display, sound and input  >", max); break;
    case ItNetworkPage: strAppend(out, "Network file transfer  >", max); break;
    case ItReset: strAppend(out, disksChanged ? "Restart the Mac (attach new disks)" : "Restart the Mac", max); break;
    case ItInterrupt: strAppend(out, "Press the interrupt switch", max); break;
    case ItScreenshot: strAppend(out, "Save a screenshot", max); break;
    case ItSave: strAppend(out, "Save settings", max); break;
    case ItResume: strAppend(out, "Resume", max); break;
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
    case ItApply: strAppend(out, pendingReboot ? "Apply: save and restart now" : "Save and restart", max); break;
    case ItSpeed:
        if (cfg.speed) { appendUint(v, (u32)cfg.speed, sizeof v); strAppend(v, "x", sizeof v); }
        else strAppend(v, "Unlimited", sizeof v);
        field("Speed", v);
        break;
    case ItPalette: field("B&W colours", paletteName(cfg.palette)); break;
    case ItSound: field("Sound", cfg.sound ? "On" : "Off"); break;
    case ItVolume: appendUint(v, (u32)cfg.volume, sizeof v); field("Volume", v); break;
    case ItMouse: appendUint(v, (u32)cfg.mouseSpeed, sizeof v); field("Mouse speed", v); break;
    case ItAlt: field("Alt key", cfg.altIsOption ? "Option (Win = Cmd)" : "Command (Win = Opt)"); break;
    case ItDisksBack: case ItMachineBack: case ItPrefsBack: case ItNetworkBack:
        strAppend(out, "<  Back", max);
        break;
    default:
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
    if (cols > 50) cols = 50;
    if (cols < 20) cols = 20;
    int list[kMaxItems];
    int n = page == PagePicker ? 0 : items(page, list);
    int rows = page == PagePicker ? kVisible + 2 : (page == PageNetwork ? 4 : n);
    int w = (cols + 2) * fw;
    int h = (rows + 6) * fh;
    if (h > s.height) h = s.height;
    int x = (s.width - w) / 2, y = (s.height - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    fillRect(s, x - 2, y - 2, w + 4, h + 4, ColFrame);
    fillRect(s, x, y, w, h, ColMenuBg);
    fillRect(s, x, y, w, fh + fh / 2, ColFrame);
    int tx = x + fw, ty = y + fh / 4;
    char line[96];

    static const char *titles[] = {"Mac Emulator", "Hard disks", "Machine, ROM and memory", "Display, sound and input",
                                   "Network file transfer", ""};
    if (page == PagePicker) {
        const char *t = pickTarget == PickHardDisk ? "Choose a hard disk image" : pickTarget == PickRom ? "Choose a ROM"
                      : "Choose a floppy disk";
        drawText(s, f, tx, ty, t, ColSelFg);
    } else {
        drawText(s, f, tx, ty, titles[page], ColSelFg);
    }
    ty = y + 2 * fh;

    if (page == PagePicker) {
        int total = pickCount + 1;
        for (int i = 0; i < kVisible && pickTop + i < total; i++) {
            int idx = pickTop + i;
            bool hot = idx == pickSel;
            if (hot) fillRect(s, x + fw / 2, ty, w - fw, fh, ColSelBg);
            strCopy(line, idx == 0 ? (pickTarget == PickRom ? "(keep the current ROM)" : "(none / eject)") : names[idx - 1], (u32)cols + 1);
            drawText(s, f, tx, ty, line, hot ? ColSelFg : idx == 0 ? ColDim : ColMenuFg);
            ty += fh;
        }
        if (pickCount == 0) drawText(s, f, tx, ty + fh, "No matching files on the SD card", ColDim);
        line[0] = 0;
        appendUint(line, (u32)pickCount, sizeof line);
        strAppend(line, " files   Enter: choose   Esc: back", sizeof line);
        drawText(s, f, tx, y + h - fh - fh / 4, line, ColDim);
        return;
    }

    if (page == PageNetwork) {
        for (int i = 0; i < 3; i++) {
            strCopy(line, host.networkLine(i), (u32)cols + 1);
            drawText(s, f, tx, ty, line, i == 0 ? ColMenuFg : ColDim);
            ty += fh;
        }
        ty += fh / 2;
    }

    int &cur = sel[page];
    if (cur >= n) cur = n - 1;
    for (int i = 0; i < n; i++) {
        bool hot = i == cur;
        if (hot) fillRect(s, x + fw / 2, ty, w - fw, fh, ColSelBg);
        label(list[i], line, (u32)cols + 1);
        drawText(s, f, tx, ty, line, hot ? ColSelFg : ColMenuFg);
        ty += fh;
    }
    ty += fh / 2;
    const char *msg = message[0] ? message : (page == PageMachine && pendingReboot) ? "Changes apply after a restart" : 0;
    if (msg) drawText(s, f, tx, ty, msg, ColSelBg);
    if (page == PageMain) {
        ty = y + h - 2 * fh - fh / 4;
        for (int i = 0; i < 2; i++) {
            strCopy(line, host.infoLine(i), (u32)cols + 1);
            drawText(s, f, tx, ty, line, ColDim);
            ty += fh;
        }
    }
}

} // namespace macemu
