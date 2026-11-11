#include "config.h"
#include "util.h"

namespace macemu {

void Config::setDefaults()
{
    memFill(this, 0, sizeof *this);
    ramKB = 4096;
    strCopy(diskDir, "disks", kPath);
    speed = 1;
    palette = 0;
    sound = 1;
    volume = 8;
    strCopy(soundDevice, "hdmi", sizeof soundDevice);
    mouseSpeed = 4;
}

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void Config::sanitize()
{
    if (ramKB != 128 && ramKB != 512 && ramKB != 1024 && ramKB != 2048 && ramKB != 4096) ramKB = 4096;
    if (speed != 0 && speed != 1 && speed != 2 && speed != 4 && speed != 8) speed = 1;
    scale = clampInt(scale, 0, 8);
    palette = clampInt(palette, 0, 15);
    volume = clampInt(volume, 0, 10);
    mouseSpeed = clampInt(mouseSpeed, 1, 8);
    sound = sound ? 1 : 0;
    altIsOption = altIsOption ? 1 : 0;
    floppyReadOnly = floppyReadOnly ? 1 : 0;
    if (width < 0 || width > 4096) width = 0;
    if (height < 0 || height > 4096) height = 0;
    if (!strEqNoCase(soundDevice, "pwm")) strCopy(soundDevice, "hdmi", sizeof soundDevice);
}

static int toInt(const char *s)
{
    int v = 0;
    bool neg = *s == '-';
    if (neg) s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

void Config::parse(const char *text, u32 len)
{
    u32 i = 0;
    while (i < len) {
        char line[160];
        u32 n = 0;
        while (i < len && text[i] != '\n') {
            if (text[i] != '\r' && n + 1 < sizeof line) line[n++] = text[i];
            i++;
        }
        i++;
        line[n] = 0;

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || !*p) continue;
        char *eq = p;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        char *kend = eq;
        while (kend > p && (kend[-1] == ' ' || kend[-1] == '\t')) kend--;
        *kend = 0;
        char *val = eq + 1;
        while (*val == ' ' || *val == '\t') val++;
        u32 vl = strLen(val);
        while (vl && (val[vl - 1] == ' ' || val[vl - 1] == '\t')) val[--vl] = 0;

        if (strEqNoCase(p, "rom")) strCopy(rom, val, kPath);
        else if (strEqNoCase(p, "ram")) ramKB = (u32)toInt(val);
        else if (strEqNoCase(p, "floppy1")) strCopy(floppy[0], val, kPath);
        else if (strEqNoCase(p, "floppy2")) strCopy(floppy[1], val, kPath);
        else if (lower(p[0]) == 'h' && lower(p[1]) == 'd' && p[2] >= '0' && p[2] <= '6' && !p[3])
            strCopy(hd[p[2] - '0'], val, kPath);
        else if (strEqNoCase(p, "disk_dir")) strCopy(diskDir, val, kPath);
        else if (strEqNoCase(p, "speed")) speed = strEqNoCase(val, "max") ? 0 : toInt(val);
        else if (strEqNoCase(p, "scale")) scale = toInt(val);
        else if (strEqNoCase(p, "width")) width = toInt(val);
        else if (strEqNoCase(p, "height")) height = toInt(val);
        else if (strEqNoCase(p, "palette")) palette = toInt(val);
        else if (strEqNoCase(p, "sound")) sound = toInt(val);
        else if (strEqNoCase(p, "volume")) volume = toInt(val);
        else if (strEqNoCase(p, "sound_device")) strCopy(soundDevice, val, sizeof soundDevice);
        else if (strEqNoCase(p, "mouse_speed")) mouseSpeed = toInt(val);
        else if (strEqNoCase(p, "alt_is_option")) altIsOption = toInt(val);
        else if (strEqNoCase(p, "floppy_readonly")) floppyReadOnly = toInt(val);
        else if (strEqNoCase(p, "date")) strCopy(date, val, sizeof date);
    }
    sanitize();
}

static void line(char *out, u32 max, const char *key, const char *val)
{
    strAppend(out, key, max);
    strAppend(out, "=", max);
    strAppend(out, val, max);
    strAppend(out, "\n", max);
}

static void lineInt(char *out, u32 max, const char *key, int v)
{
    char num[16] = "";
    if (v < 0) { strAppend(num, "-", sizeof num); v = -v; }
    appendUint(num, (u32)v, sizeof num);
    line(out, max, key, num);
}

u32 Config::serialize(char *out, u32 max) const
{
    out[0] = 0;
    strAppend(out, "# Mac Emulator settings (written by the F12 menu)\n", max);
    line(out, max, "rom", rom);
    lineInt(out, max, "ram", (int)ramKB);
    line(out, max, "floppy1", floppy[0]);
    line(out, max, "floppy2", floppy[1]);
    for (int i = 0; i < kHardDisks; i++) {
        if (!hd[i][0]) continue;
        char key[4] = {'h', 'd', (char)('0' + i), 0};
        line(out, max, key, hd[i]);
    }
    line(out, max, "disk_dir", diskDir);
    if (speed) lineInt(out, max, "speed", speed); else line(out, max, "speed", "max");
    lineInt(out, max, "scale", scale);
    lineInt(out, max, "width", width);
    lineInt(out, max, "height", height);
    lineInt(out, max, "palette", palette);
    lineInt(out, max, "sound", sound);
    lineInt(out, max, "volume", volume);
    line(out, max, "sound_device", soundDevice);
    lineInt(out, max, "mouse_speed", mouseSpeed);
    lineInt(out, max, "alt_is_option", altIsOption);
    lineInt(out, max, "floppy_readonly", floppyReadOnly);
    line(out, max, "date", date);
    return strLen(out);
}

static int num(const char *s, int digits)
{
    int v = 0;
    for (int i = 0; i < digits; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

// The Pi has no battery-backed clock, so the Mac's clock starts from the
// configured date, or failing that from the compiler's __DATE__/__TIME__.
u32 Config::clockSeconds(const char *buildDate, const char *buildTime) const
{
    if (strLen(date) >= 10) {
        int y = num(date, 4), mo = num(date + 5, 2), d = num(date + 8, 2);
        int h = 12, mi = 0;
        if (strLen(date) >= 16) { h = num(date + 11, 2); mi = num(date + 14, 2); }
        if (y >= 1904 && y <= 2039 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 && h < 24 && mi >= 0 && mi < 60)
            return macTime(y, mo, d, h, mi, 0);
    }
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int mo = 1;
    for (int i = 0; i < 12; i++)
        if (buildDate[0] == months[i * 3] && buildDate[1] == months[i * 3 + 1] && buildDate[2] == months[i * 3 + 2]) mo = i + 1;
    int d = (buildDate[4] == ' ' ? 0 : buildDate[4] - '0') * 10 + (buildDate[5] - '0');
    int y = num(buildDate + 7, 4);
    int h = num(buildTime, 2), mi = num(buildTime + 3, 2), s = num(buildTime + 6, 2);
    // The classic Mac clock is a 32-bit count that runs out in February 2040.
    if (y < 1904 || y > 2039) y = 2039;
    return macTime(y, mo, d, h, mi, s);
}

} // namespace macemu
