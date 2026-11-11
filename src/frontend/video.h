// Drawing on an 8-bit palettized surface: the scaled Macintosh screen,
// rectangles and text.  Independent of any particular framebuffer driver.

#ifndef MACEMU_VIDEO_H
#define MACEMU_VIDEO_H

#include "../core/types.h"

namespace macemu {

struct Surface {
    u8 *pixels;
    int width, height, pitch;
};

class Font {
public:
    virtual ~Font() {}
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual bool pixel(char c, int x, int y) const = 0;
};

// Palette indices.
enum {
    ColPaper = 0, ColInk = 1, ColBorder = 2,
    ColMenuBg = 3, ColMenuFg = 4, ColSelBg = 5, ColSelFg = 6, ColDim = 7, ColFrame = 8,
    kColorCount = 9
};

enum { kPaletteCount = 6 };
const char *paletteName(int index);
// Fills rgb[kColorCount] with 0xRRGGBB values.
void paletteColors(int index, u32 *rgb);

struct Layout {
    int scale, x0, y0;
};
// Largest integer zoom that fits (or the requested one, if it fits), centred.
Layout layoutFor(const Surface &s, int requestedScale);

void fillRect(Surface &s, int x, int y, int w, int h, u8 color);
void drawText(Surface &s, const Font &f, int x, int y, const char *text, u8 fg, int bg = -1);

// Draw the 512x342 one-bit Mac screen.  With `previous` (kScreenBytes long)
// only rows that changed since the last call are redrawn; `force` redraws
// everything.  Returns the number of Mac rows drawn.
int blitMacScreen(Surface &s, const Layout &l, const u8 *mac, u8 *previous, bool force);

// 1-bit Windows BMP of the Mac screen.  `out` must hold kBmpSize bytes.
enum { kBmpSize = 62 + 64 * 342 };
void makeBmp(const u8 *mac, u8 *out);

} // namespace macemu

#endif
