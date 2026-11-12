// IWM floppy controller with two Sony 3.5" 800K drives.
//
// Disks are presented to the Mac at the lowest level: the image is encoded
// into GCR nibbles track by track, and the ROM's own .Sony driver reads and
// writes those nibbles through the IWM registers.  Nothing in the ROM is
// patched, so this works with any ROM that drives real floppy hardware.

#ifndef MACEMU_IWM_H
#define MACEMU_IWM_H

#include "types.h"

namespace macemu {

// GCR codec, exposed for the unit tests.
namespace gcr {
    extern const u8 kEncode[64];
    int sectorsPerTrack(int track);                     // 12..8
    u32 trackStartBlock(int track, int sides);          // first block of a track (side 0)
    // 12 tag bytes + 512 data bytes -> 699 data nibbles + 4 checksum nibbles (6-bit values)
    void encodeSector(const u8 *tagAndData, u8 *nibbles703);
    bool decodeSector(const u8 *nibbles703, u8 *tagAndData);
}

class Iwm {
public:
    enum { kDrives = 2, kMaxTrackBytes = 12 * 740 };

    Iwm();
    void reset();

    // CPU access: `line` is address bits 9..12 (0..15).
    u8 read(int line, u64 cpuCycles);
    void write(int line, u8 v, u64 cpuCycles);
    void setSel(bool sel) { this->sel = sel; }          // VIA PA5 (head select)

    // Returns false if the image is not a 400K/800K raw or DiskCopy 4.2 image.
    bool insert(int drive, Storage *image, bool forceReadOnly = false);
    // Flush and remove.  Returns the storage that was in the drive (or null).
    Storage *eject(int drive);
    bool inserted(int drive) const { return drv[drive].image != 0; }
    void setDrivePresent(int drive, bool present) { drv[drive].present = present; }
    void flush();

    // Set whenever the ROM ejects a disk; the frontend clears it.
    bool ejectedByGuest[kDrives];
    bool motorOn(int drive) const { return drv[drive].motor && enable; }
    u32 activityCounter;                                // increments on every sector touched
    bool superDrive = false;                            // FDHD drives (Quadra): 1.44 MB MFM disks too
    // Debug: every register access (line 0..15, or 16 + ISM register).
    void (*trace)(void *ctx, int line, bool write, u8 value) = 0;
    void *traceCtx = 0;

private:
    struct Drive {
        bool present;
        Storage *image;
        u32 dataOffset;                                 // DiskCopy 4.2 header size or 0
        int sides;
        bool writeProtect;
        bool motor;
        bool stepOut;                                   // direction: true = toward track 0
        int track;
        bool switched;
        bool hd;                                        // a 1.44 MB disk
        bool mfm;                                       // drive switched to MFM mode
        u8 nib[2][kMaxTrackBytes];
        int nibLen;
        bool nibValid[2];
        int nibTrack;
        int pos;
        // write session
        bool writing;
        int writeSide, writeStart, writeCount;
    };

    Drive drv[kDrives];

    // SWIM ISM mode (1.44 MB MFM disks).  The IWM half switches to it when
    // bit 6 of its mode register is written 1, 0, 1, 1.
    bool ism;
    u8 ismMode, ismSetup, ismPhases, ismError, ismParam[16];
    int paramIdx, modeSeq;
    struct FifoByte { u8 v; bool mark; };
    FifoByte fifo[2];
    int fifoCount;
    u16 crc;                            // running CRC of the field being read
    int mfmPos;                         // byte position in the MFM track
    bool hunting;                       // searching for the first mark
    int lastSector;                     // sector of the last ID field read (for writes)
    bool lastWasMark;                   // for the CRC: a run of marks starts a field
    u8 wbuf[16384];                     // bytes written in write mode
    int wlen;

    void ismReset();
    u8 ismRead(int reg);
    void ismWrite(int reg, u8 v);
    void ismPhase(u8 v);
    bool ismReading() const;
    Drive *ismDrive();
    bool mfmByte(u8 &v, bool &mark);    // next byte of the current MFM track
    void fillFifo();
    void mfmEndWrite();

    bool ca0, ca1, ca2, lstrb, enable, select, q6, q7;
    bool sel;
    u8 mode;
    int underrunCountdown;

    Drive &cur() { return drv[select ? 1 : 0]; }
    void setLine(int line, u64 cpuCycles);
    u8 readImpl(int line, u64 cpuCycles);
    bool sense(u64 cpuCycles);
    bool senseImpl(u64 cpuCycles);
    void command();
    void loadTrack(Drive &d, int side);
    void endWrite(Drive &d);
    u8 readData();
    void writeData(u8 v);
};

} // namespace macemu

#endif
