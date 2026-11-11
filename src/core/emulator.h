// The interface every emulated Macintosh model offers to the frontends.

#ifndef MACEMU_EMULATOR_H
#define MACEMU_EMULATOR_H

#include "types.h"

namespace macemu {

class Iwm;
class Scsi;
class Rtc;

struct VideoFrame {
    int width, height;
    int depth;                  // bits per pixel: 1, 2, 4, 8 (indexed), 16 (x555) or 32 (xRGB)
    u32 rowBytes;
    const u8 *base;
    const u32 *clut;            // 0xRRGGBB entries for indexed depths; null = monochrome (1 = black)
    u32 clutVersion;            // changes whenever the colour table changes
};

class Emulator {
public:
    virtual ~Emulator() {}

    virtual const char *modelName() const = 0;
    virtual void reset() = 0;
    virtual void runFrame() = 0;                        // about 1/60 s of emulated time

    // False while no display is set up (a Mac II before its video driver runs).
    virtual bool video(VideoFrame &frame) = 0;

    // Signed 16-bit mono samples produced by the last runFrame().
    virtual const s16 *sound(u32 &count, u32 &sampleRate) = 0;

    // Key codes are Apple ADB key codes (keys.h).
    virtual void keyEvent(int macKey, bool down) = 0;
    virtual void mouseMove(int dx, int dy) = 0;
    virtual void mouseButton(bool down) = 0;
    virtual void interruptButton() = 0;

    virtual Iwm &floppy() = 0;
    virtual void attachDisk(int scsiId, Storage *disk) = 0;
    virtual void flushDisks() = 0;
    virtual Rtc &clockChip() = 0;
    virtual u64 cycles() const = 0;
    virtual u32 diskActivity() const = 0;
    virtual bool poweredOff() const { return false; }  // the Mac shut itself down
    virtual u32 debugPC() const { return 0; }          // for logs
};

} // namespace macemu

#endif
