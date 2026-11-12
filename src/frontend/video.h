// Drawing on a 32-bit (xRGB) surface: the scaled Macintosh screen in any of
// its pixel depths, plus rectangles and text for the menu.  Independent of
// any particular framebuffer driver.

#ifndef MACEMU_VIDEO_H
#define MACEMU_VIDEO_H

#include "../core/types.h"
#include "../core/emulator.h"

namespace macemu {

// Palette indices for frontend drawing.
enum {
    ColPaper = 0, ColInk = 1, ColBorder = 2,
    ColMenuBg = 3, ColMenuFg = 4, ColSelBg = 5, ColSelFg = 6, ColDim = 7, ColFrame = 8,
    kColorCount = 9
};

struct Surface {
    u32 *pixels;
    int width, height, pitch;   // pitch in pixels
    u32 palette[kColorCount];   // the indices above, already in the device's byte order
    bool swapRB;                // device stores 0xBBGGRR: swap Mac colours to match
    int dirtyTop, dirtyBottom;  // rows changed since the frontend last presented them
};

inline void markDirty(Surface &s, int top, int bottom)
{
    if (top < s.dirtyTop) s.dirtyTop = top;
    if (bottom > s.dirtyBottom) s.dirtyBottom = bottom;
}

// Put palette (0xRRGGBB) into the surface in device order.
void setPalette(Surface &s, int scheme);

class Font {
public:
    virtual ~Font() {}
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual bool pixel(char c, int x, int y) const = 0;
};

enum { kPaletteCount = 6 };
const char *paletteName(int index);
// Fills rgb[kColorCount]; the scheme colours the paper and ink of one-bit screens.
void paletteColors(int index, u32 *rgb);

struct Layout {
    int scale, x0, y0;
    int width, height;          // Mac screen size the layout was made for
};
// Largest integer zoom that fits (or the requested one, if it fits), centred.
Layout layoutFor(const Surface &s, int macWidth, int macHeight, int requestedScale);

void fillRect(Surface &s, int x, int y, int w, int h, u8 color);
void drawText(Surface &s, const Font &f, int x, int y, const char *text, u8 fg, int bg = -1);

// Draw the Mac screen.  With `previous` (rowBytes * height bytes) only rows
// that changed since the last call are redrawn; `force` redraws everything.
// Returns the number of Mac rows drawn.
// `previous` holds kPreviousFrameBytes: the largest frame (1152 x 870 in
// millions of colours).  Rows beyond it are always redrawn.
enum { kPreviousFrameBytes = 1152 * 870 * 4 };
int blitScreen(Surface &s, const Layout &l, const VideoFrame &f, u8 *previous, bool force);

// 24-bit Windows BMP of the Mac screen.
u32 bmpSize(const VideoFrame &f);
void makeBmp(const VideoFrame &f, const u32 *monoPalette, u8 *out);

} // namespace macemu

#endif
