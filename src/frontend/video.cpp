#include "video.h"

namespace macemu {

enum { kW = 512, kH = 342, kRowBytes = 64 };

static const struct {
    const char *name;
    u32 paper, ink;
} kPalettes[kPaletteCount] = {
    {"Black & white", 0xFFFFFF, 0x000000},
    {"Paper", 0xF2EFE4, 0x1C1B19},
    {"Green phosphor", 0x4AFF7A, 0x04180A},
    {"Amber", 0xFFB638, 0x1C1000},
    {"LCD", 0xC5CCB0, 0x2A3024},
    {"Inverted", 0x000000, 0xFFFFFF},
};

const char *paletteName(int index)
{
    return kPalettes[index < 0 || index >= kPaletteCount ? 0 : index].name;
}

void paletteColors(int index, u32 *rgb)
{
    if (index < 0 || index >= kPaletteCount) index = 0;
    rgb[ColPaper] = kPalettes[index].paper;
    rgb[ColInk] = kPalettes[index].ink;
    rgb[ColBorder] = 0x000000;
    rgb[ColMenuBg] = 0xECECEC;
    rgb[ColMenuFg] = 0x101010;
    rgb[ColSelBg] = 0x2A4FA8;
    rgb[ColSelFg] = 0xFFFFFF;
    rgb[ColDim] = 0x707070;
    rgb[ColFrame] = 0x101010;
}

Layout layoutFor(const Surface &s, int requestedScale)
{
    int fit = s.width / kW;
    if (s.height / kH < fit) fit = s.height / kH;
    if (fit < 1) fit = 1;
    Layout l;
    l.scale = requestedScale >= 1 && requestedScale <= fit ? requestedScale : fit;
    l.x0 = (s.width - kW * l.scale) / 2;
    l.y0 = (s.height - kH * l.scale) / 2;
    if (l.x0 < 0) l.x0 = 0;
    if (l.y0 < 0) l.y0 = 0;
    return l;
}

void fillRect(Surface &s, int x, int y, int w, int h, u8 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s.width) w = s.width - x;
    if (y + h > s.height) h = s.height - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++) memFill(s.pixels + (y + r) * s.pitch + x, color, (u32)w);
}

void drawText(Surface &s, const Font &f, int x, int y, const char *text, u8 fg, int bg)
{
    int fw = f.width(), fh = f.height();
    for (; *text; text++, x += fw) {
        if (x < 0 || y < 0 || x + fw > s.width || y + fh > s.height) continue;
        for (int r = 0; r < fh; r++) {
            u8 *p = s.pixels + (y + r) * s.pitch + x;
            for (int c = 0; c < fw; c++) {
                if (f.pixel(*text, c, r)) p[c] = fg;
                else if (bg >= 0) p[c] = (u8)bg;
            }
        }
    }
}

int blitMacScreen(Surface &s, const Layout &l, const u8 *mac, u8 *previous, bool force)
{
    int scale = l.scale;
    int visW = kW, visH = kH;
    if (l.x0 + visW * scale > s.width) visW = (s.width - l.x0) / scale;
    if (l.y0 + visH * scale > s.height) visH = (s.height - l.y0) / scale;
    int drawn = 0;
    for (int row = 0; row < visH; row++) {
        const u8 *src = mac + row * kRowBytes;
        if (previous) {
            u8 *prev = previous + row * kRowBytes;
            if (!force && !memCompare(prev, src, kRowBytes)) continue;
            memCopy(prev, src, kRowBytes);
        }
        drawn++;
        u8 *dst = s.pixels + (l.y0 + row * scale) * s.pitch + l.x0;
        u8 *p = dst;
        for (int x = 0; x < visW; x++) {
            u8 c = (src[x >> 3] & (0x80 >> (x & 7))) ? ColInk : ColPaper;
            for (int k = 0; k < scale; k++) *p++ = c;
        }
        for (int k = 1; k < scale; k++) memCopy(dst + k * s.pitch, dst, (u32)(visW * scale));
    }
    return drawn;
}

static void le16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void le32(u8 *p, u32 v) { le16(p, v); le16(p + 2, v >> 16); }

void makeBmp(const u8 *mac, u8 *out)
{
    memFill(out, 0, 62);
    out[0] = 'B'; out[1] = 'M';
    le32(out + 2, kBmpSize);
    le32(out + 10, 62);
    le32(out + 14, 40);
    le32(out + 18, kW);
    le32(out + 22, kH);
    le16(out + 26, 1);
    le16(out + 28, 1);
    le32(out + 34, kRowBytes * kH);
    le32(out + 38, 2835);
    le32(out + 42, 2835);
    le32(out + 46, 2);
    out[54] = out[55] = out[56] = 0xFF;                 // colour 0: white
    for (int row = 0; row < kH; row++)                  // BMP rows run bottom-up
        memCopy(out + 62 + row * kRowBytes, mac + (kH - 1 - row) * kRowBytes, kRowBytes);
}

} // namespace macemu
