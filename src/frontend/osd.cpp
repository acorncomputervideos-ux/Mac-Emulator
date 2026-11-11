#include "osd.h"
#include "util.h"

namespace macemu {

Osd::Osd(Config &config, OsdHost &h) : cfg(config), host(h)
{
    isOpen = dirty = false;
    sel = 0;
    picking = false;
    pickDrive = pickCount = pickSel = pickTop = 0;
    message[0] = 0;
}

void Osd::open()
{
    isOpen = true;
    dirty = true;
    picking = false;
    message[0] = 0;
}

static int cycle(int v, int dir, int lo, int hi)
{
    v += dir;
    if (v > hi) v = lo;
    if (v < lo) v = hi;
    return v;
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
    case ItScale: cfg.scale = cycle(cfg.scale, dir, 0, 8); break;
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
    default: return;
    }
    host.settingsChanged();
}

void Osd::openPicker(int drive)
{
    pickDrive = drive;
    pickCount = host.listDisks(names, kMaxDisks);
    // Simple insertion sort so the list reads alphabetically.
    for (int i = 1; i < pickCount; i++) {
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
    picking = true;
}

void Osd::activate(int item)
{
    switch (item) {
    case ItFloppy1: case ItFloppy2: openPicker(item - ItFloppy1); break;
    case ItReset: host.resetMachine(); isOpen = false; break;
    case ItInterrupt: host.interruptButton(); isOpen = false; break;
    case ItScreenshot:
        strCopy(message, host.screenshot() ? "Screenshot saved" : "Could not save screenshot", sizeof message);
        break;
    case ItSave:
        strCopy(message, host.saveSettings() ? "Settings saved to macemu.cfg" : "Could not write macemu.cfg", sizeof message);
        break;
    case ItResume: isOpen = false; break;
    default: adjust(item, 1); break;
    }
}

// Entry 0 of the picker is "eject"; disk images follow.
void Osd::pickerKey(Nav nav)
{
    int total = pickCount + 1;
    switch (nav) {
    case NavUp: pickSel = cycle(pickSel, -1, 0, total - 1); break;
    case NavDown: pickSel = cycle(pickSel, 1, 0, total - 1); break;
    case NavPageUp: pickSel = pickSel > kVisible ? pickSel - kVisible : 0; break;
    case NavPageDown: pickSel = pickSel + kVisible < total ? pickSel + kVisible : total - 1; break;
    case NavBack: case NavLeft: picking = false; return;
    case NavSelect: case NavRight:
        host.ejectDisk(pickDrive);
        message[0] = 0;
        if (pickSel > 0 && !host.insertDisk(pickDrive, names[pickSel - 1]))
            strCopy(message, "Not a usable 400K/800K disk image", sizeof message);
        picking = false;
        return;
    }
    if (pickSel < pickTop) pickTop = pickSel;
    if (pickSel >= pickTop + kVisible) pickTop = pickSel - kVisible + 1;
}

void Osd::key(Nav nav)
{
    if (!isOpen) return;
    dirty = true;
    if (picking) { pickerKey(nav); return; }
    switch (nav) {
    case NavUp: sel = cycle(sel, -1, 0, kItems - 1); message[0] = 0; break;
    case NavDown: sel = cycle(sel, 1, 0, kItems - 1); message[0] = 0; break;
    case NavPageUp: sel = 0; break;
    case NavPageDown: sel = kItems - 1; break;
    case NavLeft: adjust(sel, -1); break;
    case NavRight: adjust(sel, 1); break;
    case NavSelect: activate(sel); break;
    case NavBack: isOpen = false; break;
    }
}

void Osd::label(int item, char *out, u32 max)
{
    out[0] = 0;
    switch (item) {
    case ItFloppy1: case ItFloppy2: {
        const char *n = host.diskName(item - ItFloppy1);
        strAppend(out, item == ItFloppy1 ? "Internal floppy   " : "External floppy   ", max);
        strAppend(out, n[0] ? n : "(empty)", max);
        break;
    }
    case ItSpeed:
        strAppend(out, "Speed             ", max);
        if (cfg.speed) { appendUint(out, (u32)cfg.speed, max); strAppend(out, "x", max); }
        else strAppend(out, "Unlimited", max);
        break;
    case ItScale:
        strAppend(out, "Screen zoom       ", max);
        if (cfg.scale) { appendUint(out, (u32)cfg.scale, max); strAppend(out, "x", max); }
        else strAppend(out, "Largest that fits", max);
        break;
    case ItPalette:
        strAppend(out, "Colours           ", max);
        strAppend(out, paletteName(cfg.palette), max);
        break;
    case ItSound:
        strAppend(out, "Sound             ", max);
        strAppend(out, cfg.sound ? "On" : "Off", max);
        break;
    case ItVolume:
        strAppend(out, "Volume            ", max);
        appendUint(out, (u32)cfg.volume, max);
        break;
    case ItMouse:
        strAppend(out, "Mouse speed       ", max);
        appendUint(out, (u32)cfg.mouseSpeed, max);
        break;
    case ItAlt:
        strAppend(out, "Alt key           ", max);
        strAppend(out, cfg.altIsOption ? "Option (Win = Command)" : "Command (Win = Option)", max);
        break;
    case ItReset: strAppend(out, "Restart the Macintosh", max); break;
    case ItInterrupt: strAppend(out, "Press the interrupt switch", max); break;
    case ItScreenshot: strAppend(out, "Save a screenshot", max); break;
    case ItSave: strAppend(out, "Save settings", max); break;
    case ItResume: strAppend(out, "Resume", max); break;
    }
}

void Osd::draw(Surface &s, const Font &f)
{
    if (!isOpen) return;
    enum { kCols = 46 };
    int fw = f.width(), fh = f.height();
    int rowsWanted = picking ? kVisible + 5 : kItems + 7;
    int w = (kCols + 4) * fw, h = (rowsWanted + 2) * fh;
    int x = (s.width - w) / 2, y = (s.height - h) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    fillRect(s, x - 2, y - 2, w + 4, h + 4, ColFrame);
    fillRect(s, x, y, w, h, ColMenuBg);
    fillRect(s, x, y, w, fh + fh / 2, ColFrame);
    int tx = x + 2 * fw, ty = y + fh / 4;
    char line[80];

    if (picking) {
        line[0] = 0;
        strAppend(line, pickDrive ? "External floppy: choose a disk" : "Internal floppy: choose a disk", sizeof line);
        drawText(s, f, tx, ty, line, ColSelFg);
        ty = y + 2 * fh + fh / 2;
        int total = pickCount + 1;
        for (int i = 0; i < kVisible && pickTop + i < total; i++) {
            int idx = pickTop + i;
            bool hot = idx == pickSel;
            if (hot) fillRect(s, x + fw, ty, w - 2 * fw, fh, ColSelBg);
            char name[kCols + 1];
            strCopy(name, idx == 0 ? "(Eject / leave empty)" : names[idx - 1], sizeof name);
            drawText(s, f, tx, ty, name, hot ? ColSelFg : idx == 0 ? ColDim : ColMenuFg);
            ty += fh;
        }
        if (pickCount == 0) drawText(s, f, tx, ty + fh, "No disk images in the disk folder", ColDim);
        line[0] = 0;
        appendUint(line, (u32)pickCount, sizeof line);
        strAppend(line, " images   Enter: insert   Esc: back", sizeof line);
        drawText(s, f, tx, y + h - fh - fh / 2, line, ColDim);
        return;
    }

    drawText(s, f, tx, ty, "Mac Emulator", ColSelFg);
    ty = y + 2 * fh + fh / 2;
    for (int i = 0; i < kItems; i++) {
        bool hot = i == sel;
        if (hot) fillRect(s, x + fw, ty, w - 2 * fw, fh, ColSelBg);
        label(i, line, kCols + 1);
        drawText(s, f, tx, ty, line, hot ? ColSelFg : ColMenuFg);
        ty += fh;
        if (i == ItFloppy2 || i == ItAlt) ty += fh / 2;
    }
    ty += fh / 2;
    if (message[0]) drawText(s, f, tx, ty, message, ColSelBg);
    ty += fh + fh / 4;
    for (int n = 0; n < 2; n++) {
        strCopy(line, host.infoLine(n), kCols + 1);
        drawText(s, f, tx, ty, line, ColDim);
        ty += fh;
    }
}

} // namespace macemu
