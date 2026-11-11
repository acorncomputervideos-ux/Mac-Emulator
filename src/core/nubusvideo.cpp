#include "nubusvideo.h"
#include "videodrv.h"

namespace macemu {

enum { noErr = 0, controlErr = -17, statusErr = -18, paramErr = -50 };

// Parameter block and DCE offsets.
enum { kCsCode = 26, kCsParam = 28, kDCtlDevBase = 42 };

NubusVideo::NubusVideo(int s, int w, int h, GuestMemory &m) : slot(s), width(w), height(h), mem(m)
{
    if ((u32)width * (u32)height > kRegs) { width = 640; height = 480; }
    memFill(frame, 0, sizeof frame);
    pb = dce = result = cmdLatch = 0;
    opened = irqEnabled = irqPending = gray = false;
    mode = defaultMode = kFirstMode;
    clutVersion = 0;
    defaultColors();
    buildRom();
}

// ---------------------------------------------------------------- declaration ROM

namespace {

struct RomWriter {
    u8 *rom;
    u32 pos;
    void byte(u8 v) { rom[pos++] = v; }
    void word(u16 v) { putBe16(rom + pos, v); pos += 2; }
    void lng(u32 v) { putBe32(rom + pos, v); pos += 4; }
    void align() { while (pos & 3) byte(0); }
    void cstring(const char *s) { while (*s) byte((u8)*s++); byte(0); align(); }
    // An sResource list entry pointing at `target` (offset relative to the entry).
    void entry(u8 id, u32 target) { lng(((u32)id << 24) | ((target - pos) & 0xFFFFFF)); }
    void inlineValue(u8 id, u32 value) { lng(((u32)id << 24) | (value & 0xFFFFFF)); }
    void end() { lng(0xFF000000); }
};

}

void NubusVideo::buildRom()
{
    memFill(rom, 0, sizeof rom);
    RomWriter w{rom, 0};
    // Two passes: the first lays everything out to learn the addresses.
    u32 a[32] = {0};
    enum {
        Dir, Board, Video, BoardType, BoardName, VendorList, VendorId, VendorRev, VendorPart,
        VideoType, VideoName, DrvrDir, DrvrBlock, MinorBase, MinorLength, ModeList, VPBlock = ModeList + kModes,
        Count = VPBlock + kModes
    };
    for (int pass = 0; pass < 2; pass++) {
        w.pos = 0;
        a[Dir] = w.pos;
        w.entry(0x01, a[Board]);
        w.entry(0x80, a[Video]);
        w.end();

        a[Board] = w.pos;
        w.entry(1, a[BoardType]);                       // sRsrcType
        w.entry(2, a[BoardName]);                       // sRsrcName
        w.inlineValue(32, 0x0E3E);                      // BoardId
        w.entry(36, a[VendorList]);                     // VendorInfo
        w.end();

        a[Video] = w.pos;
        w.entry(1, a[VideoType]);
        w.entry(2, a[VideoName]);
        w.entry(4, a[DrvrDir]);                         // sRsrcDrvrDir
        w.inlineValue(7, 2);                            // sRsrcFlags: fOpenAtStart
        w.inlineValue(8, 1);                            // sRsrcHWDevId
        w.entry(10, a[MinorBase]);                      // MinorBaseOS
        w.entry(11, a[MinorLength]);                    // MinorLength
        for (int m = 0; m < kModes; m++) w.entry((u8)(kFirstMode + m), a[ModeList + m]);
        w.end();

        for (int m = 0; m < kModes; m++) {
            a[ModeList + m] = w.pos;
            w.entry(1, a[VPBlock + m]);                 // mVidParams
            w.inlineValue(3, 1);                        // mPageCnt
            w.inlineValue(4, 0);                        // mDevType: CLUT
            w.end();
        }

        a[BoardType] = w.pos;
        w.word(1); w.word(0); w.word(0); w.word(0);     // catBoard, typeBoard
        a[VideoType] = w.pos;
        w.word(3); w.word(1); w.word(1); w.word(0xBEEF);   // catDisplay, typeVideo, drSwApple, drHw
        a[BoardName] = w.pos;
        w.cstring("Mac Emulator Video Card");
        a[VideoName] = w.pos;
        w.cstring("Display_Video_Emu");
        a[VendorList] = w.pos;
        w.entry(1, a[VendorId]);
        w.entry(3, a[VendorRev]);
        w.entry(4, a[VendorPart]);
        w.end();
        a[VendorId] = w.pos;
        w.cstring("Mac Emulator");
        a[VendorRev] = w.pos;
        w.cstring("1.0");
        a[VendorPart] = w.pos;
        w.cstring("MEVC-1");
        a[MinorBase] = w.pos;
        w.lng(0);
        a[MinorLength] = w.pos;
        w.lng(kRegs);

        for (int m = 0; m < kModes; m++) {
            u16 bits = (u16)(1 << m);
            a[VPBlock + m] = w.pos;
            w.lng(4 + 42);                              // sBlock length, then the VPBlock
            w.lng(0);                                   // vpBaseOffset
            w.word((u16)(width * bits / 8));            // vpRowBytes
            w.word(0); w.word(0); w.word((u16)height); w.word((u16)width);   // vpBounds
            w.word(0);                                  // vpVersion
            w.word(0);                                  // vpPackType
            w.lng(0);                                   // vpPackSize
            w.lng(0x00480000);                          // vpHRes 72 dpi
            w.lng(0x00480000);                          // vpVRes
            w.word(0);                                  // vpPixelType: chunky
            w.word(bits);                               // vpPixelSize
            w.word(1);                                  // vpCmpCount
            w.word(bits);                               // vpCmpSize
            w.lng(0);                                   // vpPlaneBytes
            w.align();
        }

        a[DrvrDir] = w.pos;
        w.entry(1, a[DrvrBlock]);                       // sMacOS68000
        w.entry(2, a[DrvrBlock]);                       // sMacOS68020
        w.end();
        a[DrvrBlock] = w.pos;
        w.lng(4 + sizeof kVideoDriver);
        for (u32 i = 0; i < sizeof kVideoDriver; i++) w.byte(kVideoDriver[i]);
        w.align();
        (void)Count;
    }

    // Move the image to the top of the ROM, under the format block.
    u32 used = w.pos;
    u32 start = kRomSize - 20 - used;
    for (u32 i = used; i-- > 0;) rom[start + i] = rom[i];
    memFill(rom, 0, start);

    u8 *fb = rom + kRomSize - 20;
    u32 dirAddr = start + a[Dir];
    putBe32(fb, (dirAddr - (u32)(fb - rom)) & 0xFFFFFF);  // fbDirOffset
    putBe32(fb + 4, kRomSize);                          // fbLength
    putBe32(fb + 8, 0);                                 // fbCRC, filled in below
    fb[12] = 1;                                         // fbRevisionLevel
    fb[13] = 1;                                         // fbFormat: Apple
    putBe32(fb + 14, 0x5A932BC7);                       // fbTstPattern
    fb[18] = 0;
    fb[19] = 0x0F;                                      // fbByteLanes: all four
    u32 crc = 0;
    for (u32 i = 0; i < kRomSize; i++) crc = ((crc << 1) | (crc >> 31)) + rom[i];
    putBe32(fb + 8, crc);
}

// ---------------------------------------------------------------- registers

u8 NubusVideo::read(u32 off)
{
    off &= 0xFFFFFF;
    if (off < kRegs) return frame[off];
    if (off < kVramSize) {
        u32 r = off - kRegs;
        u32 v = r < 4 ? (u32)slot : r >= 12 && r < 16 ? result : 0;
        return (u8)(v >> (8 * (3 - (r & 3))));
    }
    if (off >= 0x1000000 - kRomSize) return rom[off - (0x1000000 - kRomSize)];
    return 0;
}

void NubusVideo::write(u32 off, u8 v)
{
    off &= 0xFFFFFF;
    if (off < kRegs) { frame[off] = v; return; }
    if (off >= kVramSize) return;
    u32 r = off - kRegs;
    u32 *reg = r < 4 ? 0 : r < 8 ? &pb : r < 12 ? &dce : 0;
    int sh = 8 * (3 - (r & 3));
    if (reg) { *reg = (*reg & ~(0xFFu << sh)) | ((u32)v << sh); return; }
    if (r < 4) {
        cmdLatch = (cmdLatch & ~(0xFFu << sh)) | ((u32)v << sh);
        if ((r & 3) == 3) { command(cmdLatch); cmdLatch = 0; }   // the low byte completes the write
    }
}

void NubusVideo::vbl()
{
    if (opened && irqEnabled) irqPending = true;
}

bool NubusVideo::video(VideoFrame &f)
{
    if (!opened) return false;
    f.width = width;
    f.height = height;
    f.depth = depth();
    f.rowBytes = rowBytes();
    f.base = frame;
    f.clut = clut;
    f.clutVersion = clutVersion;
    return true;
}

// ---------------------------------------------------------------- commands

u32 NubusVideo::baseAddress()
{
    return mem.peek32(dce + kDCtlDevBase);
}

void NubusVideo::defaultColors()
{
    int n = 1 << depth();
    for (int i = 0; i < 256; i++) {
        u32 level = i < n ? 255 - (u32)i * 255 / (u32)(n - 1) : 0;
        clut[i] = level * 0x010101;
    }
    clutVersion++;
}

void NubusVideo::setMode(int m)
{
    mode = m;
    defaultColors();
}

void NubusVideo::grayPage()
{
    static const u8 pattern[kModes] = {0xAA, 0xCC, 0xF0, 0xFF};
    int d = depth();
    u32 rb = rowBytes();
    for (int y = 0; y < height; y++) {
        u8 *row = frame + (u32)y * rb;
        if (d == 8) {
            for (u32 x = 0; x < rb; x++) row[x] = ((x + (u32)y) & 1) ? 0xFF : 0x00;
        } else {
            u8 p = pattern[mode - kFirstMode];
            memFill(row, (y & 1) ? (u8)~p : p, rb);
        }
    }
}

void NubusVideo::command(u32 cmd)
{
    switch (cmd) {
    case 1:                                             // open
        opened = true;
        irqEnabled = true;
        irqPending = false;
        gray = false;
        setMode(kFirstMode);
        grayPage();
        result = noErr;
        break;
    case 2: result = (u32)(s32)control(mem.peek16(pb + kCsCode), mem.peek32(pb + kCsParam)); break;
    case 3: result = (u32)(s32)status(mem.peek16(pb + kCsCode), mem.peek32(pb + kCsParam)); break;
    case 4:                                             // close
        irqEnabled = false;
        irqPending = false;
        result = noErr;
        break;
    case 0xA0:                                          // interrupt acknowledge
        irqPending = false;
        break;
    default:
        result = (u32)(s32)controlErr;
        break;
    }
}

// VDPageInfo: csMode (word), csData (long), csPage (word), csBaseAddr (long)
s16 NubusVideo::control(u16 code, u32 p)
{
    switch (code) {
    case 0:                                             // cscReset
        setMode(kFirstMode);
        grayPage();
        mem.poke16(p, (u16)mode);
        mem.poke16(p + 6, 0);
        mem.poke32(p + 8, baseAddress());
        return noErr;
    case 1:                                             // cscKillIO
        return noErr;
    case 2: {                                           // cscSetMode
        int m = mem.peek16(p);
        if (m < kFirstMode || m >= kFirstMode + kModes || mem.peek16(p + 6) != 0) return controlErr;
        setMode(m);
        mem.poke32(p + 8, baseAddress());
        return noErr;
    }
    case 3: {                                           // cscSetEntries
        u32 table = mem.peek32(p);
        s16 start = (s16)mem.peek16(p + 4);
        int count = mem.peek16(p + 6) + 1;
        int n = 1 << depth();
        if (start != -1 && (start < 0 || start + count > n)) return controlErr;
        for (int i = 0; i < count; i++) {
            u32 spec = table + (u32)i * 8;
            int index = start == -1 ? mem.peek16(spec) : start + i;
            if (index >= n) continue;
            u32 r = mem.peek16(spec + 2) >> 8, g = mem.peek16(spec + 4) >> 8, b = mem.peek16(spec + 6) >> 8;
            if (gray) r = g = b = (r * 30 + g * 59 + b * 11) / 100;
            clut[index] = (r << 16) | (g << 8) | b;
        }
        clutVersion++;
        return noErr;
    }
    case 4:                                             // cscSetGamma: tables are linear
        return noErr;
    case 5:                                             // cscGrayPage
        if (mem.peek16(p + 6) != 0) return controlErr;
        grayPage();
        return noErr;
    case 6:                                             // cscSetGray
        gray = mem.peek(p) != 0;
        return noErr;
    case 7:                                             // cscSetInterrupt
        irqEnabled = mem.peek(p) == 0;
        if (!irqEnabled) irqPending = false;
        return noErr;
    case 9:                                             // cscSetDefaultMode
        defaultMode = mem.peek(p);
        return noErr;
    default:
        return controlErr;
    }
}

s16 NubusVideo::status(u16 code, u32 p)
{
    switch (code) {
    case 2:                                             // cscGetMode
        mem.poke16(p, (u16)mode);
        mem.poke16(p + 6, 0);
        mem.poke32(p + 8, baseAddress());
        return noErr;
    case 3: {                                           // cscGetEntries
        u32 table = mem.peek32(p);
        s16 start = (s16)mem.peek16(p + 4);
        int count = mem.peek16(p + 6) + 1;
        int n = 1 << depth();
        if (start != -1 && (start < 0 || start + count > n)) return statusErr;
        for (int i = 0; i < count; i++) {
            u32 spec = table + (u32)i * 8;
            int index = start == -1 ? mem.peek16(spec) : start + i;
            if (start != -1) mem.poke16(spec, (u16)index);
            if (index >= n) continue;
            u32 c = clut[index];
            mem.poke16(spec + 2, (u16)(((c >> 16) & 0xFF) * 0x101));
            mem.poke16(spec + 4, (u16)(((c >> 8) & 0xFF) * 0x101));
            mem.poke16(spec + 6, (u16)((c & 0xFF) * 0x101));
        }
        return noErr;
    }
    case 4:                                             // cscGetPageCnt
        mem.poke16(p + 6, 1);
        return noErr;
    case 5:                                             // cscGetPageBase
        if (mem.peek16(p + 6) != 0) return statusErr;
        mem.poke32(p + 8, baseAddress());
        return noErr;
    case 6:                                             // cscGetGray
        mem.poke(p, gray ? 1 : 0);
        return noErr;
    case 7:                                             // cscGetInterrupt
        mem.poke(p, irqEnabled ? 0 : 1);
        return noErr;
    case 9:                                             // cscGetDefaultMode
        mem.poke(p, (u8)defaultMode);
        return noErr;
    default:
        return statusErr;
    }
}

} // namespace macemu
