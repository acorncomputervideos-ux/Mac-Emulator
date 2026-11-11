#include "video.h"

namespace macemu {

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

static inline u32 swapRB(u32 c) { return ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }

void setPalette(Surface &s, int scheme)
{
    paletteColors(scheme, s.palette);
    if (s.swapRB) for (int i = 0; i < kColorCount; i++) s.palette[i] = swapRB(s.palette[i]);
}

Layout layoutFor(const Surface &s, int macWidth, int macHeight, int requestedScale)
{
    Layout l;
    l.width = macWidth;
    l.height = macHeight;
    int fit = macWidth > 0 ? s.width / macWidth : 1;
    if (macHeight > 0 && s.height / macHeight < fit) fit = s.height / macHeight;
    if (fit < 1) fit = 1;
    l.scale = requestedScale >= 1 && requestedScale <= fit ? requestedScale : fit;
    l.x0 = (s.width - macWidth * l.scale) / 2;
    l.y0 = (s.height - macHeight * l.scale) / 2;
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
    u32 c = s.palette[color % kColorCount];
    for (int r = 0; r < h; r++) {
        u32 *p = s.pixels + (y + r) * s.pitch + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void drawText(Surface &s, const Font &f, int x, int y, const char *text, u8 fg, int bg)
{
    int fw = f.width(), fh = f.height();
    u32 fc = s.palette[fg % kColorCount], bc = bg >= 0 ? s.palette[bg % kColorCount] : 0;
    for (; *text; text++, x += fw) {
        if (x < 0 || y < 0 || x + fw > s.width || y + fh > s.height) continue;
        for (int r = 0; r < fh; r++) {
            u32 *p = s.pixels + (y + r) * s.pitch + x;
            for (int c = 0; c < fw; c++) {
                if (f.pixel(*text, c, r)) p[c] = fc;
                else if (bg >= 0) p[c] = bc;
            }
        }
    }
}

static inline u32 pixelAt(const u8 *row, int x, int depth, const u32 *clut, const u32 *mono)
{
    switch (depth) {
    case 1: {
        u32 bit = (row[x >> 3] >> (7 - (x & 7))) & 1;
        return clut ? clut[bit] : mono[bit];
    }
    case 2: return clut[(row[x >> 2] >> (6 - 2 * (x & 3))) & 3];
    case 4: return clut[(row[x >> 1] >> (4 - 4 * (x & 1))) & 15];
    case 8: return clut[row[x]];
    case 16: {
        u32 v = ((u32)row[2 * x] << 8) | row[2 * x + 1];
        u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
    }
    default:
        return ((u32)row[4 * x + 1] << 16) | ((u32)row[4 * x + 2] << 8) | row[4 * x + 3];
    }
}

int blitScreen(Surface &s, const Layout &l, const VideoFrame &f, u8 *previous, bool force)
{
    int scale = l.scale;
    int visW = f.width, visH = f.height;
    if (l.x0 + visW * scale > s.width) visW = (s.width - l.x0) / scale;
    if (l.y0 + visH * scale > s.height) visH = (s.height - l.y0) / scale;
    if (visW <= 0 || visH <= 0) return 0;
    u32 usedBytes = ((u32)f.width * (u32)f.depth + 7) / 8;
    const u32 mono[2] = {s.palette[ColPaper], s.palette[ColInk]};
    int drawn = 0;
    for (int row = 0; row < visH; row++) {
        const u8 *src = f.base + (u32)row * f.rowBytes;
        if (previous) {
            u8 *prev = previous + (u32)row * usedBytes;
            if (!force && !memCompare(prev, src, usedBytes)) continue;
            memCopy(prev, src, usedBytes);
        }
        drawn++;
        u32 *dst = s.pixels + (l.y0 + row * scale) * s.pitch + l.x0;
        u32 *p = dst;
        bool swap = s.swapRB && !(f.depth == 1 && !f.clut);   // the mono palette is in device order already
        for (int x = 0; x < visW; x++) {
            u32 c = pixelAt(src, x, f.depth, f.clut, mono);
            if (swap) c = swapRB(c);
            for (int k = 0; k < scale; k++) *p++ = c;
        }
        for (int k = 1; k < scale; k++) memCopy(dst + k * s.pitch, dst, (u32)(visW * scale) * 4);
    }
    return drawn;
}

static void le16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void le32(u8 *p, u32 v) { le16(p, v); le16(p + 2, v >> 16); }

u32 bmpSize(const VideoFrame &f)
{
    return 54 + (((u32)f.width * 3 + 3) & ~3u) * (u32)f.height;
}

void makeBmp(const VideoFrame &f, const u32 *mono, u8 *out)
{
    u32 rowSize = ((u32)f.width * 3 + 3) & ~3u;
    memFill(out, 0, 54);
    out[0] = 'B'; out[1] = 'M';
    le32(out + 2, bmpSize(f));
    le32(out + 10, 54);
    le32(out + 14, 40);
    le32(out + 18, (u32)f.width);
    le32(out + 22, (u32)f.height);
    le16(out + 26, 1);
    le16(out + 28, 24);
    le32(out + 34, rowSize * (u32)f.height);
    le32(out + 38, 2835);
    le32(out + 42, 2835);
    for (int row = 0; row < f.height; row++) {          // BMP rows run bottom-up
        const u8 *src = f.base + (u32)row * f.rowBytes;
        u8 *dst = out + 54 + (u32)(f.height - 1 - row) * rowSize;
        for (int x = 0; x < f.width; x++) {
            u32 c = pixelAt(src, x, f.depth, f.clut, mono);
            dst[3 * x] = (u8)c;
            dst[3 * x + 1] = (u8)(c >> 8);
            dst[3 * x + 2] = (u8)(c >> 16);
        }
        memFill(dst + 3 * f.width, 0, rowSize - 3 * (u32)f.width);
    }
}

} // namespace macemu
