// The shared folder (see sharedfs.h).
//
// Record layouts and trap conventions are Apple's (Inside Macintosh:
// Files, and the File System Manager chapter of the Mac OS 7.5 docs).
#include "sharedfs.h"
#include "macnames.h"
#include "state.h"
#ifdef SHARED_DEBUG
#include <cstdio>
#endif

namespace macemu {

namespace {

// ---------------------------------------------------------------- Mac OS

enum : s16 {
    noErr = 0, dirFulErr = -33, dskFulErr = -34, nsvErr = -35, ioErr = -36, bdNamErr = -37,
    fnOpnErr = -38, eofErr = -39, posErr = -40, tmfoErr = -42, fnfErr = -43, wPrErr = -44,
    fLckdErr = -45, fBsyErr = -47, dupFNErr = -48, opWrErr = -49, paramErr = -50, rfNumErr = -51,
    permErr = -54, extFSErr = -58, wrPermErr = -61, dirNFErr = -120, badMovErr = -122,
    afpItemNotFound = -5012, fsmUnknownFSMMessageErr = -438,
};

// Parameter blocks
enum {
    ioCompletion = 12, ioResult = 16, ioNamePtr = 18, ioVRefNum = 22, ioRefNum = 24, ioVersNum = 26,
    ioPermssn = 27, ioMisc = 28, ioBuffer = 32, ioReqCount = 36, ioActCount = 40, ioPosMode = 44,
    ioPosOffset = 46,
    ioFRefNum = 24, ioFDirIndex = 28, ioFlAttrib = 30, ioACUser = 31, ioFlFndrInfo = 32, ioDirID = 48,
    ioFlStBlk = 52, ioFlLgLen = 54, ioFlPyLen = 58, ioFlRStBlk = 62, ioFlRLgLen = 64, ioFlRPyLen = 68,
    ioFlCrDat = 72, ioFlMdDat = 76, ioFlBkDat = 80, ioFlXFndrInfo = 84, ioFlParID = 100, ioFlClpSiz = 104,
    ioDrDirID = 48, ioDrNmFls = 52, ioDrParID = 100,
    ioWDIndex = 26, ioWDProcID = 28, ioWDVRefNum = 32, ioWDDirID = 48,
    ioVolIndex = 28, ioVCrDate = 30, ioVLsMod = 34, ioVAtrb = 38, ioVNmFls = 40, ioVBitMap = 42,
    ioAllocPtr = 44, ioVNmAlBlks = 46, ioVAlBlkSiz = 48, ioVClpSiz = 52, ioAlBlSt = 56, ioVNxtCNID = 58,
    ioVFrBlk = 62, ioVSigWord = 64, ioVDrvInfo = 66, ioVDRefNum = 68, ioVFSID = 70, ioVBkUp = 72,
    ioVSeqNum = 76, ioVWrCnt = 78, ioVFilCnt = 82, ioVDirCnt = 86, ioVFndrInfo = 90,
    ioNewName = 28, ioNewDirID = 36,
    ioFCBIndx = 28, ioFCBFlNm = 32, ioFCBFlags = 36, ioFCBStBlk = 38, ioFCBEOF = 40, ioFCBPLen = 44,
    ioFCBCrPs = 48, ioFCBVRefNum = 52, ioFCBClpSiz = 54, ioFCBParID = 58,
    ioDestNamePtr = 36, ioDestDirID = 48,               // (ExchangeFiles: FIDParam)
};

// VCB, FCB, WDCB, drive queue, File System Manager records
enum {
    vcbSigWord = 8, vcbCrDate = 10, vcbLsMod = 14, vcbAtrb = 18, vcbNmFls = 20, vcbNmAlBlks = 26,
    vcbAlBlkSiz = 28, vcbClpSiz = 32, vcbNxtCNID = 38, vcbFreeBks = 42, vcbVN = 44, vcbDrvNum = 72,
    vcbDRefNum = 74, vcbFSID = 76, vcbVRefNum = 78, vcbNmRtDirs = 112, vcbFilCnt = 114, vcbDirCnt = 118,
    fcbFlNm = 0, fcbFlags = 4, fcbSBlk = 6, fcbEOF = 8, fcbPLen = 12, fcbCrPs = 16, fcbVPtr = 20,
    fcbClmpSize = 30, fcbFType = 50, fcbCatPos = 54, fcbDirID = 58, fcbCName = 62,
    wdVCBPtr = 0, wdDirID = 4, wdProcID = 12,
    dsDiskInPlace = 3, dsInstalled = 4, dsQLink = 6, dsQType = 10, dsQDrive = 12, dsQRefNum = 14,
    dsQFSID = 16, dsDriveSize = 18, dsDriveS1 = 20,
    fsdLength = 4, fsdVersion = 6, fileSystemFSID = 8, fileSystemName = 10, fileSystemCommProc = 116,
    fsdHFSCI = 132, kFSDSize = 196,
    compInterfMask = 0, compInterfProc = 4, stackTop = 12, stackSize = 16, idSector = 28,
    vmiLength = 0, vmiMedia = 2,
    ppNamePtr = 0, ppStartOffset = 4, ppComponentLength = 6, ppMoreName = 8, ppFoundDelimiter = 9,
    vMVersion = 0, vMAttrib = 2, vMLocalHand = 6, vMServerAdr = 10, vMVolumeGrade = 14, vMForeignPrivID = 18,
    kVolParmsSize = 20,
};

enum : u16 { kFSID = 0x4D45 };                          // 'ME'
enum : u32 { kMedia = 0x6D655348u, kAlBlk = 0x8000, kClump = 0x8000 };   // 'meSH'
const u32 kMacEpoch = 2082844800u;                      // 1904 -> 1970

// Our data block in the system heap.
enum {
    dDrive = 0x000,                                     // drive status / queue element (~30 bytes)
    dFSD = 0x040,                                       // FSDRec (196)
    dPB = 0x110,                                        // a parameter block (108)
    dVMI = 0x180,                                       // VolumeMountInfoHeader (8)
    dParse = 0x190,                                     // ParsePathRec (10)
    dRet = 0x1A0,                                       // results of utilities (32)
    dWD = 0x1C0,                                        // a WDParam (52)
    dName = 0x200,                                      // a Str255 (256)
    kDataSize = 0x300,
    kStackSize = 0x8000,
};

// The glue page: 68k code.
enum {
    gMarker = 0x000,                                    // $71FF: back to the host
    gComm = 0x010,                                      // the CommProc
    gHFS = 0x020,                                       // the HFS component procedure
    gTraps = 0x040,                                     // [trap, $71FF] per call depth
    gUT = 0x100,                                        // FSM utility wrappers, 32 bytes each
    gDrvr = 0x340,                                      // a do-nothing driver for the drive queue
};

enum UT {
    utAllocateVCB, utAddNewVCB, utDetermineVol, utResolveWDCB, utGetDefaultVol, utGetPathComponentName,
    utParsePathname, utDisposeVCB, utCheckWDRefNum, utSetDefaultVol, utAllocateFCB, utReleaseFCB,
    utIndexFCB, utResolveFCB, utAdjustEOF, utAllocateWDCB, utReleaseWDCB, utCount
};

// Each: clr.w -(sp) (the result), the arguments, moveq #selector,d0,
// _FSMgr, move.w (sp)+,d0, rts.
const u16 kUTCode[utCount][12] = {
    {0x4267, 0x2F08, 0x2F09, 0x4267, 0x7006, 0xA824, 0x301F, 0x4E75},             // (&sysVCBLength a0, &vcb a1, addSize 0)
    {0x4267, 0x3F00, 0x2F08, 0x2F09, 0x7007, 0xA824, 0x301F, 0x4E75},             // (driveNum d0, &vRefNum a0, vcb a1)
    {0x4267, 0x2F08, 0x2F09, 0x2F0A, 0x2F0B, 0x2F0C, 0x701D, 0xA824, 0x301F, 0x4E75},   // (pb, &status, &more, &vRefNum, &vcb)
    {0x4267, 0x2F00, 0x3F01, 0x3F02, 0x2F08, 0x700E, 0xA824, 0x301F, 0x4E75},     // (procID d0, index d1, vRefNum d2, &wdcb a0)
    {0x4267, 0x2F08, 0x7012, 0xA824, 0x301F, 0x4E75},                             // (wdpb a0)
    {0x4267, 0x2F08, 0x701C, 0xA824, 0x301F, 0x4E75},                             // (parseRec a0)
    {0x4267, 0x2F08, 0x2F09, 0x701B, 0xA824, 0x301F, 0x4E75},                     // (&startOffset a0, name a1)
    {0x4267, 0x2F08, 0x7008, 0xA824, 0x301F, 0x4E75},                             // (vcb a0)
    {0x4267, 0x3F00, 0x7013, 0xA824, 0x301F, 0x4E75},                             // (refNum d0)
    {0x4267, 0x2F00, 0x2F01, 0x3F02, 0x7011, 0xA824, 0x301F, 0x4E75},             // (nodeHint d0, dirID d1, refNum d2)
    {0x4267, 0x2F08, 0x2F09, 0x7000, 0xA824, 0x301F, 0x4E75},                     // (&refNum a0, &fcb a1)
    {0x4267, 0x3F00, 0x7001, 0xA824, 0x301F, 0x4E75},                             // (refNum d0)
    {0x4267, 0x2F08, 0x2F09, 0x2F0A, 0x7004, 0xA824, 0x301F, 0x4E75},             // (vcb a0, &refNum a1, &fcb a2)
    {0x4267, 0x3F00, 0x2F08, 0x7005, 0xA824, 0x301F, 0x4E75},                     // (refNum d0, &fcb a0)
    {0x4267, 0x3F00, 0x7010, 0xA824, 0x301F, 0x4E75},                             // (refNum d0)
    {0x4267, 0x2F08, 0x700C, 0xA824, 0x301F, 0x4E75},                             // (pb a0)
    {0x4267, 0x3F00, 0x700D, 0xA824, 0x301F, 0x4E75},                             // (vRefNum d0)
};

// ---------------------------------------------------------------- names


u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
void scopy(char *d, const char *s, u32 max) { u32 i = 0; for (; s[i] && i + 1 < max; i++) d[i] = s[i]; d[i] = 0; }
void sappend(char *d, const char *s, u32 max) { u32 n = slen(d); scopy(d + n, s, max > n ? max - n : 0); }
bool seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
bool endsNoCase(const char *s, const char *e)
{
    u32 ls = slen(s), le = slen(e);
    if (le > ls) return false;
    for (u32 i = 0; i < le; i++) {
        char a = s[ls - le + i], b = e[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != b) return false;
    }
    return true;
}


// Files the Mac does not see: dot files (AppleDouble "._", .DS_Store ...;
// a name starting with '.' would also look like a driver to Mac OS), and
// the web page's uploads still on their way ("NAME.part").
bool hidden(const char *name)
{
    if (name[0] == '.') return true;
    u32 n = 0;
    while (name[n]) n++;
    return n > 5 && name[n - 5] == '.' && name[n - 4] == 'p' && name[n - 3] == 'a' && name[n - 2] == 'r' && name[n - 1] == 't';
}

// Type and creator for files without Finder info.
void guessType(const char *name, u32 &type, u32 &creator)
{
    static const struct { const char *ext; u32 type, creator; } kTypes[] = {
        {".txt", 0x54455854, 0x74747874}, {".text", 0x54455854, 0x74747874}, {".c", 0x54455854, 0x74747874},
        {".h", 0x54455854, 0x74747874}, {".html", 0x54455854, 0x4D4F5353}, {".htm", 0x54455854, 0x4D4F5353},
        {".hqx", 0x54455854, 0x53495478}, {".sit", 0x53495444, 0x53495421}, {".sea", 0x4150504C, 0x3F3F3F3F},
        {".cpt", 0x50414354, 0x45585452}, {".zip", 0x5A495020, 0x535A4950},
        {".gif", 0x47494666, 0x3F3F3F3F}, {".jpg", 0x4A504547, 0x3F3F3F3F}, {".jpeg", 0x4A504547, 0x3F3F3F3F},
        {".pict", 0x50494354, 0x74747874}, {".pct", 0x50494354, 0x74747874}, {".png", 0x504E4766, 0x3F3F3F3F},
        {".pdf", 0x50444620, 0x4341524F}, {".mov", 0x4D6F6F56, 0x54564F44}, {".aiff", 0x41494646, 0x3F3F3F3F},
        {".wav", 0x57415645, 0x3F3F3F3F}, {".bin", 0x42494E41, 0x3F3F3F3F}, {".dsk", 0x6466696C, 0x64437079},
        {".img", 0x6466696C, 0x64437079}, {".image", 0x6466696C, 0x64437079},
    };
    type = creator = 0x3F3F3F3F;
    for (const auto &t : kTypes) if (endsNoCase(name, t.ext)) { type = t.type; creator = t.creator; }
}

// AppleDouble, as written here: header, two entries (Finder info, resource
// fork), 32 bytes of Finder info at 50, the resource fork from 82 on.
enum { adMagic = 0x00051607, adVersion = 0x00020000, adFinfo = 50, adRsrc = 82, adRsrcLenAt = 46 };

void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u16 get16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

void ourHeader(u8 h[adRsrc], const u8 finfo[32], u32 rsrcLen)
{
    for (int i = 0; i < adRsrc; i++) h[i] = 0;
    put32(h, adMagic); put32(h + 4, adVersion);
    put16(h + 24, 2);
    put32(h + 26, 9); put32(h + 30, adFinfo); put32(h + 34, 32);
    put32(h + 38, 2); put32(h + 42, adRsrc); put32(h + 46, rsrcLen);
    for (int i = 0; i < 32; i++) h[adFinfo + i] = finfo ? finfo[i] : 0;
}

} // namespace

// ================================================================ setup

SharedFolder::SharedFolder(M68K &c, HostFS &f) : cpu(c), fs(f)
{
    scopy(volName, "Shared", sizeof volName);
    items = new Item[kMaxItems];
    namePool = new char[kNamePool];
    hash = new u32[kHashSize];
    for (auto &l : listings) { l.ids = new u32[kListMax]; l.valid = false; l.used = 0; }
    // Nothing open yet (reset() closes what is: on the Pi, fresh memory is
    // not zeros).
    for (u32 i = 0; i < kMaxOpen; i++) { open[i].used = false; open[i].f = -1; }
    for (int i = 0; i < 8; i++) returned[i] = false;
    reset();
    buildGlue();
}

void SharedFolder::setVolumeName(const char *name)
{
    u8 m[32];
    int n = hostToMac(name, m, fs.namesUtf8());
    for (int i = 0; i < n && i < 27; i++) volName[i] = (char)m[i];
    volName[n < 27 ? n : 27] = 0;
    if (!volName[0]) scopy(volName, "Shared", sizeof volName);
}

void SharedFolder::reset()
{
    closeAll();
    for (u32 i = 0; i < kMaxOpen; i++) open[i].used = false;
    for (u32 i = 0; i < kHashSize; i++) hash[i] = 0;
    itemCount = 0;
    poolUsed = 0;
    nextID = 16;
    // The root: ID 2, its parent 1.
    Item &r = items[itemCount++];
    r.id = kRootID; r.parent = 1; r.nameOff = 0; r.nameLen = 0; r.used = true; r.sig = r.date = 0;
    namePool[poolUsed++] = 0;
    invalidate();
    tried = mounted = false;
    data = stack = glueAddr = 0;
    clipCode = 0;
    scrapSeen = false;
    drvrRefNum = 0;
    lastPoll = volDate = 0;
    vRefNum = 0;
    driveNum = 0;
    vcbAddr = 0;
    depth = 0;
    status = "waiting for Mac OS";
}

void SharedFolder::attach()
{
    cpu.hostCall = &hostCallHook;
    cpu.trapHook = &trapHookFn;
    cpu.hookCtx = this;
}

void SharedFolder::buildGlue()
{
    for (u32 i = 0; i < kGlueSize; i++) glue[i] = 0;
    auto w = [&](u32 off, u16 v) { put16(glue + off, v); };
    w(gMarker, 0x71FF);
    w(gComm, 0x71F1); w(gComm + 2, 0x4E74); w(gComm + 4, 10);        // RTD #10
    w(gHFS, 0x71F0); w(gHFS + 2, 0x4E74); w(gHFS + 4, 16);           // RTD #16
    static_assert(gUT + 32 * utCount <= gDrvr, "the glue does not fit");
    // The driver: the Finder looks the drive's driver up (for its icon),
    // so the drive needs a real one.  Open and close succeed; read, write,
    // control and status are refused (the file system does the work).
    static const char kDrvrName[] = ".MacEmuShare";
    const u32 nameLen = sizeof kDrvrName - 1;
    u32 code = gDrvr + 18 + ((1 + nameLen + 1) & ~1u);   // after the header and the name
    w(gDrvr + 0, 0x4F00);                               // dNeedLock, status, control, write, read
    w(gDrvr + 8, (u16)(code - gDrvr));                  // open
    w(gDrvr + 10, (u16)(code + 4 - gDrvr));             // prime
    w(gDrvr + 12, (u16)(code + 12 - gDrvr));            // control
    w(gDrvr + 14, (u16)(code + 20 - gDrvr));            // status
    w(gDrvr + 16, (u16)(code - gDrvr));                 // close
    glue[gDrvr + 18] = (u8)nameLen;
    for (u32 i = 0; i < nameLen; i++) glue[gDrvr + 19 + i] = (u8)kDrvrName[i];
    static const u16 kDrvrCode[] = {
        0x7000, 0x4E75,                                 // open, close: moveq #0,d0; rts
        0x70DC, 0x2F38, 0x08FC, 0x4E75,                 // prime: moveq #ioErr,d0; move.l JIODone,-(sp); rts
        0x70EF, 0x2F38, 0x08FC, 0x4E75,                 // control: controlErr
        0x70EE, 0x2F38, 0x08FC, 0x4E75,                 // status: statusErr
    };
    for (u32 i = 0; i < sizeof kDrvrCode / 2; i++) w(code + 2 * i, kDrvrCode[i]);
    static_assert(gDrvr + 18 + 14 + 28 <= kGlueSize, "the driver does not fit");
    for (int u = 0; u < utCount; u++)
        for (int i = 0; i < 12 && kUTCode[u][i]; i++) w(gUT + 32 * (u32)u + 2 * (u32)i, kUTCode[u][i]);
}

// ================================================================ the CPU's hooks

bool SharedFolder::hostCallHook(void *ctx, M68K &c, u16 op)
{
    SharedFolder &s = *(SharedFolder *)ctx;
    u32 sp = c.a[7];
    switch (op) {
    case 0x71FF:                                        // back from 68k code we ran
        if (s.depth > 0) { s.returned[s.depth] = true; return true; }
        return false;
    case 0x71F0: {                                      // HFSCIProc(vcb, selectCode, pb, globals, fsid)
        s16 r = s.hfsProc(s.r32(sp + 16), s.r16(sp + 14), s.r32(sp + 10));
#ifdef SHARED_DEBUG
        { u32 pb = s.r32(sp + 10), np = s.r32(pb + 18); char nm[64] = ""; if (np) { int n = s.r8(np); for (int i = 0; i < n && i < 63; i++) nm[i] = (char)s.r8(np + 1 + i); nm[n < 63 ? n : 63] = 0; }
          fprintf(stderr, "    -> %d  name '%s' vref %d index %d dirid %u\n", r, nm, (s16)s.r16(pb + 22), (s16)s.r16(pb + 28), s.r32(pb + 48)); }
#endif
        s.w16(sp + 20, (u16)r);
        s.callsServed++;
        return true;
    }
    case 0x71F1: {                                      // CommProc(message, pb, globals)
        s16 r = s.commProc(s.r16(sp + 12), s.r32(sp + 8));
        s.w16(sp + 14, (u16)r);
        return true;
    }
    default:
        return false;
    }
}

void SharedFolder::trapHookFn(void *ctx, M68K &c, u16 op)
{
    SharedFolder &s = *(SharedFolder *)ctx;
    (void)c;
#ifdef SHARED_DEBUG
    static int traced = 0;
    if (s.mounted && traced < 4000000 && s.depth == 0) { traced++; fprintf(stderr, "trap %04X pc %08X d0 %08X a0 %08X\n", op, c.instPC, c.d[0], c.a[0]); }
#endif
    // The first WaitNextEvent / GetNextEvent: Mac OS is up.
    if (!s.tried && (op == 0xA860 || op == 0xA970) && s.depth == 0) {
        s.tried = true;
        s.install();
    } else if (s.glueAddr && (op == 0xA860 || op == 0xA970) && s.depth == 0) {
        s.clipboardSync();
        if (s.mounted) {
            s.poll();
            if (s.onIdle) s.onIdle(s.idleCtx);
        }
    }
}

bool SharedFolder::run68k(u32 addr, Regs &r)
{
    if (depth >= 7) return false;
    M68K::Context saved;
    cpu.save(saved);
    for (int i = 0; i < 8; i++) cpu.d[i] = r.d[i];
    for (int i = 0; i < 7; i++) cpu.a[i] = r.a[i];
    cpu.a[7] -= 4;                                      // the return address: the marker
    w32(cpu.a[7], glueAddr + gMarker);
    cpu.pc = addr;
    cpu.stopped = false;
    depth++;
    returned[depth] = false;
    bool ok = true;
    for (u32 n = 0; !returned[depth]; n++) {
        if (n > 50000000u) { ok = false; break; }       // it never came back
        cpu.step();
    }
    for (int i = 0; i < 8; i++) r.d[i] = cpu.d[i];
    for (int i = 0; i < 7; i++) r.a[i] = cpu.a[i];
    depth--;
    cpu.restore(saved);
    return ok;
}

bool SharedFolder::runTrap(u16 trap, Regs &r)
{
    if (!glueAddr) return false;
    u32 slot = gTraps + 8 * (u32)(depth & 7);
    w16(glueAddr + slot, trap);
    w16(glueAddr + slot + 2, 0x71FF);
    return run68k(glueAddr + slot, r);
}

s16 SharedFolder::utCall(u32 ut, Regs &r)
{
    if (!run68k(glueAddr + gUT + 32 * ut, r)) return ioErr;
    return (s16)(r.d[0] & 0xFFFF);
}

// ================================================================ the clipboard

// Low-memory globals of the Scrap Manager.
enum { ScrapSize = 0x960, ScrapHandle = 0x964, ScrapCount = 0x968, ScrapState = 0x96A };

void SharedFolder::setClipText(const char *macRoman, u32 len)
{
    if (len > kClipMax) len = kClipMax;
    clipPending = false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    for (u32 i = 0; i < len; i++) pend[i] = macRoman[i];
    pendLen = len;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    clipPending = true;                                 // (taken by the Mac's core at its next idle moment)
}

// The Scrap Manager's calls take their arguments on the stack: a few
// lines of 68k for them, in the system heap (made the first time).
s32 SharedFolder::scrapCall(u32 offset, Regs &r)
{
    if (!clipCode) {
        Regs a = {};
        a.d[0] = 32;
        if (!runTrap(0xA71E, a) || !a.a[0]) return -108;   // NewPtrSysClear
        clipCode = a.a[0];
        static const u16 code[16] = {
            0x42A7, 0xA9FC, 0x201F, 0x4E75,                 // 0:  ZeroScrap
            0x42A7, 0x2F00, 0x2F01, 0x2F08, 0xA9FE, 0x201F, 0x4E75, 0,   // 8:  PutScrap(d0 length, d1 type, a0 data)
            0x42A7, 0xA9FB, 0x201F, 0x4E75,                 // 24: LoadScrap
        };
        for (u32 i = 0; i < 16; i++) w16(clipCode + 2 * i, code[i]);
    }
    if (!run68k(clipCode + offset, r)) return -1;
    return (s32)r.d[0];
}

void SharedFolder::clipboardSync()
{
    // From the host: onto the Mac's clipboard (as text).
    if (clipPending) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        Regs r = {};
        r.d[0] = pendLen ? pendLen : 1;
        if (runTrap(0xA71E, r) && r.a[0]) {             // NewPtrSysClear: the text, for PutScrap
            u32 buf = r.a[0];
            hostToMacMem(buf, pend, pendLen);
            Regs z = {};
            scrapCall(0, z);
            Regs p = {};
            p.d[0] = pendLen; p.d[1] = 0x54455854; p.a[0] = buf;   // 'TEXT'
            scrapCall(8, p);
            Regs d = {};
            d.a[0] = buf;
            runTrap(0xA01F, d);                         // DisposePtr
        }
        clipPending = false;
        lastScrapCount = (s16)r16(ScrapCount);          // ours: not news for the host
        scrapSeen = true;
    }

    // From the Mac: its clipboard changed (ZeroScrap counts).
    s16 count = (s16)r16(ScrapCount);
    if (scrapSeen && count == lastScrapCount) return;
    lastScrapCount = count;

    scrapSeen = true;
    if ((s16)r16(ScrapState) == 0) {                    // in the Clipboard file: into memory
        Regs l = {};
        scrapCall(24, l);
    }
    u32 h = r32(ScrapHandle), size = r32(ScrapSize);
    if ((s16)r16(ScrapState) <= 0 || !h || !r32(h) || size > (1u << 24)) return;
    u32 p = r32(h), end = p + size;
    while (p + 8 <= end) {
        u32 type = r32(p), len = r32(p + 4);
        if (len > end - p - 8) break;
        if (type == 0x54455854) {                       // 'TEXT'
            u32 n = len < kClipMax - 1 ? len : kClipMax - 1;
            clipSerial = clipSerial + 1;                // odd: being written
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            macToHostMem(p + 8, clip, n);
            clip[n] = 0;
            clipLen = n;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            clipSerial = clipSerial + 1;
            return;
        }
        p += 8 + ((len + 1) & ~1u);
    }
}

// ================================================================ Mac memory

u8 SharedFolder::r8(u32 a) { return (u8)cpu.rd(a, 0); }
u16 SharedFolder::r16(u32 a) { return (u16)cpu.rd(a, 1); }
u32 SharedFolder::r32(u32 a) { return cpu.rd(a, 2); }
void SharedFolder::w8(u32 a, u8 v) { cpu.wr(a, 0, v); }
void SharedFolder::w16(u32 a, u16 v) { cpu.wr(a, 1, v); }
void SharedFolder::w32(u32 a, u32 v) { cpu.wr(a, 2, v); }
void SharedFolder::macToHostMem(u32 a, void *dst, u32 n) { u8 *d = (u8 *)dst; for (u32 i = 0; i < n; i++) d[i] = r8(a + i); }
void SharedFolder::hostToMacMem(u32 a, const void *src, u32 n)
{
    const u8 *s = (const u8 *)src;
    u32 i = 0;
    for (; i < n && ((a + i) & 3); i++) w8(a + i, s[i]);
    for (; i + 4 <= n; i += 4) w32(a + i, get32(s + i));
    for (; i < n; i++) w8(a + i, s[i]);
}

// ================================================================ installing

void SharedFolder::install()
{
    Regs r = {};
    // The glue, in the system heap.  The call that makes room for it runs
    // from ToolScratch ($9CE, 8 bytes of scratch for the Toolbox), which
    // is put back afterwards.
    u8 scratch[8];
    for (u32 i = 0; i < 8; i++) scratch[i] = r8(0x9CE + i);
    w16(0x9CE, 0xA71E);                                 // NewPtrSysClear
    w16(0x9D0, 0x71FF);
    r.d[0] = kGlueSize;
    glueAddr = 0x9CE;                                   // (run68k's return address: unused here)
    bool ran = run68k(0x9CE, r);
    for (u32 i = 0; i < 8; i++) w8(0x9CE + i, scratch[i]);
    glueAddr = 0;
    if (!ran || !r.a[0]) { status = "out of memory"; return; }
    glueAddr = r.a[0];
    hostToMacMem(glueAddr, glue, kGlueSize);
    r = Regs();

    if (!wantVolume) { status = "off (no folder chosen)"; return; }

    // The File System Manager (Mac OS 7.5, or 7.1 with it installed).
    r.d[0] = 0x66732020;                                // Gestalt('fs  ')
    if (!runTrap(0xA1AD, r) || (r.d[0] & 0xFFFF) || !(r.a[0] & (1u << 2))) {
        status = "needs Mac OS 7.5 or later (the File System Manager)";
        return;
    }
    r = Regs();
    r.d[0] = 0x66736D20;                                // Gestalt('fsm ')
    if (!runTrap(0xA1AD, r) || (r.d[0] & 0xFFFF) || r.a[0] < 0x0120) {
        status = "needs File System Manager 1.2 (Mac OS 7.5 or later)";
        return;
    }

    // Our data and a stack for the file system, in the system heap.
    r = Regs();
    r.d[0] = kStackSize;
    if (!runTrap(0xA71E, r) || !r.a[0]) { status = "out of memory"; return; }   // NewPtrSysClear
    stack = r.a[0];
    r = Regs();
    r.d[0] = kDataSize;
    if (!runTrap(0xA71E, r) || !r.a[0]) { status = "out of memory"; return; }
    data = r.a[0];

    // The driver, in a free unit table entry, opened.
    s16 refNum = 0;
    {
        u32 table = r32(0x11C);                         // UTableBase
        u16 entries = r16(0x1D2);                       // UnitNtryCnt
        u16 unit = 48;
        while (unit < entries && r32(table + 4u * unit)) unit++;
        if (unit >= entries) { status = "no free driver slot"; return; }
        refNum = (s16)~unit;
        r = Regs();
        r.d[0] = (u32)(s32)refNum;
        r.a[0] = glueAddr + gDrvr;
        if (!runTrap(0xA43D, r) || (r.d[0] & 0xFFFF)) { status = "could not install the driver"; return; }   // DrvrInstallRsrvMem
        u32 dce = r32(r32(table + 4u * unit));
        w32(dce + 0, glueAddr + gDrvr);                 // dCtlDriver: our code (not a resource)
        w16(dce + 4, (u16)((r16(glueAddr + gDrvr) & 0xFF00) | 0x20));   // flags, opened
        drvrRefNum = refNum;
    }

    // A drive for the volume: the next free number from 8.
    u16 drive = 8;
    for (bool clash = true; clash; ) {
        clash = false;
        for (u32 q = r32(0x308 + 2); q; q = r32(q)) if (r16(q + 6) == drive) { clash = true; drive++; break; }
    }
    driveNum = drive;
    u32 ds = data + dDrive;
    w8(ds + dsDiskInPlace, 8);                          // a fixed disk
    w8(ds + dsInstalled, 1);
    w16(ds + dsQType, 1);
    w16(ds + dsDriveSize, 0xFFFF);
    w16(ds + dsDriveS1, 0);
    w16(ds + dsQFSID, kFSID);
    r = Regs();
    r.d[0] = ((u32)drive << 16) | (u16)refNum;
    r.a[0] = ds + dsQLink;
    runTrap(0xA04E, r);                                 // AddDrive

    // The file system.
    u32 fsd = data + dFSD;
    w16(fsd + fsdLength, kFSDSize);
    w16(fsd + fsdVersion, 1);
    w16(fsd + fileSystemFSID, kFSID);
    static const char kName[] = "Mac Emulator shared folder";
    w8(fsd + fileSystemName, (u8)(sizeof kName - 1));
    for (u32 i = 0; i + 1 < sizeof kName; i++) w8(fsd + fileSystemName + 1 + i, (u8)kName[i]);
    w32(fsd + fileSystemCommProc, glueAddr + gComm);
    w32(fsd + fsdHFSCI + compInterfProc, glueAddr + gHFS);
    w32(fsd + fsdHFSCI + stackTop, stack + kStackSize);
    w32(fsd + fsdHFSCI + stackSize, kStackSize);
    w32(fsd + fsdHFSCI + idSector, 0xFFFFFFFFu);
    r = Regs();
    r.d[0] = 0;                                         // InstallFS
    r.a[0] = fsd;
    if (!runTrap(0xA0AC, r) || (r.d[0] & 0xFFFF)) { status = "the File System Manager refused it"; return; }
    w32(fsd + fsdHFSCI + compInterfMask, r32(fsd + fsdHFSCI + compInterfMask) | 0x80000000u | 0x00080000u | 0x00800000u);
    r = Regs();
    r.d[0] = 5;                                         // SetFSInfo
    r.d[3] = kFSDSize;
    r.d[4] = kFSID;
    r.a[0] = fsd;
    runTrap(0xA0AC, r);

    // Mount the volume.
    u32 pb = data + dPB;
    for (u32 i = 0; i < 108; i++) w8(pb + i, 0);
    w32(pb + ioBuffer, data + dVMI);
    w16(data + dVMI + vmiLength, 8);
    w32(data + dVMI + vmiMedia, kMedia);
    r = Regs();
    r.d[0] = 0x41;                                      // PBVolumeMount
    r.a[0] = pb;
    runTrap(0xA260, r);
    if ((s16)(r.d[0] & 0xFFFF) != noErr) { status = "could not mount the volume"; return; }
    mounted = true;
    status = "on the desktop";
}

// Every couple of seconds (at WaitNextEvent): have files come or gone on
// the host (the web page, a computer) in the root or the folders the Finder
// looked at last?  Then the volume's date moves on, so the Finder looks again.
void SharedFolder::poll()
{
    u32 now = fs.milliseconds();
    if (now - lastPoll < 2000 || !vcbAddr) return;
    lastPoll = now;
    bool changed = false;
    u32 dirs[kListings + 1], n = 0;
    dirs[n++] = kRootID;
    for (auto &l : listings) if (l.valid && l.dir != kRootID) dirs[n++] = l.dir;
    for (u32 i = 0; i < n; i++) {
        u32 before = 0;
        for (auto &l : listings) if (l.valid && l.dir == dirs[i]) before = l.sig;
        Listing *L = list(dirs[i]);
        if (L && before && L->sig != before) changed = true;
        if (L && dirs[i] == kRootID && L->newest + kMacEpoch > volDate) { if (volDate) changed = true; volDate = L->newest + kMacEpoch; }
    }
    if (changed) {
        volDate++;
        w32(vcbAddr + vcbLsMod, volDate);
    }
}

s16 SharedFolder::commProc(u16 message, u32 pb)
{
    switch (message) {
    case 0: case 3: case 4: return noErr;               // nop, load, unload
    case 2: return (s16)r16(pb + ioVRefNum) == (s16)driveNum ? noErr : extFSErr;   // IDDisk
    case 5: return r32(r32(pb + ioBuffer) + vmiMedia) == kMedia ? noErr : extFSErr;   // IDVolMount
    case 1: {                                           // GetIcon: a disk with a folder tab, large (32 x 32)
        if (r8(pb + 14) != 1 || r32(pb + 6) < 256) return afpItemNotFound;
        u32 buf = r32(pb + 2);
        for (u32 y = 0; y < 32; y++) {
            u32 icon = 0, mask = 0;
            if (y >= 8 && y <= 25) {                    // the disk: a box with a dark band and a light
                mask = 0x7FFFFFFE;
                icon = (y == 8 || y == 25) ? 0x7FFFFFFE : 0x40000002;
                if (y >= 19 && y <= 22) icon |= 0x5FFFFFFA;
                if (y == 21) icon = (icon & ~0x0000000Cu) | 0x00000008;
            }
            if (y >= 3 && y <= 8) {                     // a folder tab on top: shared
                u32 tab = y == 3 ? 0x00FF0000 : 0x01008000;
                if (y == 8) tab = 0;
                icon |= tab;
                mask |= y == 3 ? 0x00FF0000 : 0x01FF8000;
            }
            if (y >= 11 && y <= 16) icon |= y == 11 || y == 16 ? 0x00FFFF00 : 0x00800100;   // a folder shape on its face
            w32(buf + 4 * y, icon);
            w32(buf + 128 + 4 * y, mask);
        }
        w32(pb + 10, 256);
        return noErr;
    }
    default: return fsmUnknownFSMMessageErr;
    }
}

// (The trap slots are written into the Mac's copy of the glue.)
s16 SharedFolder::volumeMount(u32 pb)
{
    Regs r = {};
    r.a[0] = data + dRet;
    r.a[1] = data + dRet + 2;
    s16 e = utCall(utAllocateVCB, r);
    if (e) return e;
    u32 vcb = r32(data + dRet + 2);
    HostStat st = {};
    fs.stat("", st);
    u32 now = st.mtime + kMacEpoch;
    w16(vcb + vcbSigWord, 0x4244);
    w32(vcb + vcbCrDate, now);
    w32(vcb + vcbLsMod, now);
    w16(vcb + vcbNmFls, 1);
    w16(vcb + vcbNmRtDirs, 1);
    w16(vcb + vcbNmAlBlks, 0xFFFF);
    w32(vcb + vcbAlBlkSiz, kAlBlk);
    w32(vcb + vcbClpSiz, kClump);
    w32(vcb + vcbNxtCNID, nextID);
    u64 freeBlocks = fs.freeBytes() / kAlBlk;
    w16(vcb + vcbFreeBks, (u16)(freeBlocks > 0xFFFF ? 0xFFFF : freeBlocks));
    w8(vcb + vcbVN, (u8)slen(volName));
    for (u32 i = 0; i < slen(volName); i++) w8(vcb + vcbVN + 1 + i, (u8)volName[i]);
    w16(vcb + vcbFSID, kFSID);
    w32(vcb + vcbFilCnt, 1);
    w32(vcb + vcbDirCnt, 1);

    r = Regs();
    r.d[0] = driveNum;
    r.a[0] = data + dRet;
    r.a[1] = vcb;
    e = utCall(utAddNewVCB, r);
    if (e) return e;
    vRefNum = (s16)r16(data + dRet);
    vcbAddr = vcb;

    // (No disk-inserted event: Mac OS would try to mount the drive again
    // through its driver, which it has none of.  The Finder finds new
    // volumes by itself, as with file servers.)
    w16(pb + ioVRefNum, (u16)vRefNum);
    return noErr;
}

// ================================================================ names

int SharedFolder::macToHost(const u8 *mac, int len, char *out, int max, bool utf8) { return macToHostName(mac, len, out, max, utf8); }
int SharedFolder::hostToMac(const char *host, u8 *mac, bool utf8) { return hostToMacName(host, mac, utf8); }
bool SharedFolder::sameMacName(const u8 *a, int la, const u8 *b, int lb) { return macemu::sameMacName(a, la, b, lb); }

// ================================================================ the catalog

u32 SharedFolder::hashOf(u32 parent, const char *name, u32 len) const
{
    u32 h = parent * 2654435761u;
    for (u32 i = 0; i < len; i++) h = (h ^ (u8)name[i]) * 16777619u;
    return h & (kHashSize - 1);
}

SharedFolder::Item *SharedFolder::find(u32 id)
{
    // The root is item 0; the others are made in order from ID 16 on.
    u32 index = id == kRootID ? 0 : id >= 16 ? id - 15 : (u32)kMaxItems;
    if (index >= itemCount || !items[index].used) return 0;
    return &items[index];
}

u32 SharedFolder::itemFor(u32 parent, const char *name)
{
    u32 len = slen(name);
    u32 h = hashOf(parent, name, len);
    u32 freeSlot = kHashSize;
    for (u32 k = 0; k < kHashSize; k++, h = (h + 1) & (kHashSize - 1)) {
        u32 v = hash[h];
        if (v == 0) break;
        if (v == 0xFFFFFFFFu) { if (freeSlot == kHashSize) freeSlot = h; continue; }
        Item &it = items[v - 1];
        if (it.used && it.parent == parent && it.nameLen == len) {
            bool same = true;
            for (u32 i = 0; i < len && same; i++) same = namePool[it.nameOff + i] == name[i];
            if (same) return it.id;
        }
    }
    if (freeSlot == kHashSize) freeSlot = h;
    if (itemCount >= kMaxItems || poolUsed + len + 1 > kNamePool) return 0;   // full (very many files)
    Item &it = items[itemCount];
    it.sig = it.date = 0;
    it.id = nextID++;
    it.parent = parent;
    it.nameOff = poolUsed;
    it.nameLen = (u16)len;
    it.used = true;
    for (u32 i = 0; i <= len; i++) namePool[poolUsed++] = name[i];
    hash[freeSlot] = ++itemCount;
    return it.id;
}

void SharedFolder::forget(u32 id)
{
    Item *it = find(id);
    if (!it || id == kRootID) return;
    u32 index = (u32)(it - items) + 1;
    for (u32 h = 0; h < kHashSize; h++) if (hash[h] == index) { hash[h] = 0xFFFFFFFFu; break; }
    it->used = false;
}

void SharedFolder::move(u32 id, u32 newParent, const char *newName)
{
    Item *it = find(id);
    if (!it) return;
    u32 index = (u32)(it - items) + 1;
    for (u32 h = 0; h < kHashSize; h++) if (hash[h] == index) { hash[h] = 0xFFFFFFFFu; break; }
    u32 len = slen(newName);
    if (poolUsed + len + 1 <= kNamePool) {
        it->nameOff = poolUsed;
        it->nameLen = (u16)len;
        for (u32 i = 0; i <= len; i++) namePool[poolUsed++] = newName[i];
    }
    it->parent = newParent;
    u32 h = hashOf(newParent, newName, len);
    while (hash[h] != 0 && hash[h] != 0xFFFFFFFFu) h = (h + 1) & (kHashSize - 1);
    hash[h] = index;
}

const char *SharedFolder::hostName(u32 id)
{
    Item *it = find(id);
    return it ? namePool + it->nameOff : "";
}

u32 SharedFolder::parentOf(u32 id)
{
    Item *it = find(id);
    return it ? it->parent : 0;
}

bool SharedFolder::pathOf(u32 id, char *out, u32 max)
{
    u32 chain[64];
    int n = 0;
    while (id != kRootID) {
        Item *it = find(id);
        if (!it || n == 64) return false;
        chain[n++] = id;
        id = it->parent;
    }
    out[0] = 0;
    for (int i = n - 1; i >= 0; i--) {
        if (out[0]) sappend(out, "/", max);
        sappend(out, hostName(chain[i]), max);
    }
    return slen(out) + 1 < max;
}

SharedFolder::Listing *SharedFolder::list(u32 dir)
{
    // A folder's items, read again after a moment in case the host changed
    // it (the web page); the last few folders are kept.
    u32 now = fs.milliseconds();
    Listing *slot = &listings[0];
    for (auto &l : listings) {
        if (l.valid && l.dir == dir && now - l.when < 1500) { l.used = ++listClock; return &l; }
        if (!l.valid ? slot->valid : l.used < slot->used) slot = &l;
    }
    Listing &L = *slot;
    L.valid = false;
    char where[1024];
    if (!pathOf(dir, where, sizeof where)) return 0;
    int d = fs.openDir(where);
    if (d < 0) return 0;
    L.count = 0;
    L.sig = 2166136261u;
    L.newest = 0;
    char name[512];
    u32 mtime;
    while (fs.readDir(d, name, sizeof name, &mtime) && L.count < kListMax) {
        if (hidden(name)) continue;
        u32 id = itemFor(dir, name);
        if (!id) continue;
        L.ids[L.count++] = id;
        L.sig = (L.sig ^ id) * 16777619u;
        if (mtime > L.newest) L.newest = mtime;
    }
    L.sig ^= (u32)L.count;
    fs.closeDir(d);
    L.dir = dir;
    L.when = now;
    L.valid = true;
    L.used = ++listClock;
    return &L;
}

u32 SharedFolder::findInDir(u32 dir, const u8 *mac, int len)
{
    Listing *L = list(dir);
    if (!L) return 0;
    u8 m[32];
    for (int i = 0; i < L->count; i++) {
        int n = hostToMac(hostName(L->ids[i]), m, fs.namesUtf8());
        if (sameMacName(m, n, mac, len)) return L->ids[i];
    }
    return 0;
}

// ================================================================ resolving names

s16 SharedFolder::currentDir(u32 pb, u32 dirID, u32 &dir, bool noVolName)
{
    Regs r = {};
    r.a[0] = pb;
    r.a[1] = data + dRet;
    r.a[2] = data + dRet + 2;
    r.a[3] = data + dRet + 4;
    r.a[4] = data + dRet + 6;
    u32 name = 0;
    if (noVolName) { name = r32(pb + ioNamePtr); w32(pb + ioNamePtr, 0); }
    s16 e = utCall(utDetermineVol, r);
    if (noVolName) w32(pb + ioNamePtr, name);
    if (e) return e;
    switch (r16(data + dRet)) {
    case 1: dir = kRootID; return noErr;                // a full pathname
    case 2: case 4: dir = dirID ? dirID : (u32)kRootID; return noErr;   // vRefNum, drive number
    case 3:                                             // a working directory
        if (dirID) { dir = dirID; return noErr; }
        r = Regs();
        r.d[2] = (u32)(s32)(s16)r16(pb + ioVRefNum);
        r.a[0] = data + dRet;
        e = utCall(utResolveWDCB, r);
        if (e) return e;
        dir = r32(r32(data + dRet) + wdDirID);
        return noErr;
    case 5:                                             // the default volume
        if (dirID) { dir = dirID; return noErr; }
        w32(data + dWD + ioNamePtr, 0);
        r = Regs();
        r.a[0] = data + dWD;
        e = utCall(utGetDefaultVol, r);
        if (e) return e;
        dir = r32(data + dWD + ioWDDirID);
        return noErr;
    default:
        return paramErr;
    }
}

// The folder a pathname leads to, and its last name if it names something
// in that folder (leafLen 0: it names the folder).
s16 SharedFolder::resolve(u32 pb, u32 dirID, u32 &parent, u8 *leaf, int &leafLen, bool noVolName)
{
    leafLen = 0;
    s16 e = currentDir(pb, dirID, parent, noVolName);
    if (e) return e;
    if (!find(parent)) return dirNFErr;
    u32 namePtr = r32(pb + ioNamePtr);
    if (!namePtr || r8(namePtr) == 0) return noErr;     // no name: the folder itself

    u32 pr = data + dParse;
    w32(pr + ppNamePtr, namePtr);
    w16(pr + ppStartOffset, 0);
    w16(pr + ppComponentLength, 0);
    w8(pr + ppMoreName, 0);
    w8(pr + ppFoundDelimiter, 0);
    Regs r = {};
    r.a[0] = pr + ppStartOffset;
    r.a[1] = namePtr;
    e = utCall(utParsePathname, r);
    if (e) return bdNamErr;
    r = Regs();
    r.a[0] = pr;
    e = utCall(utGetPathComponentName, r);
    if (e) return e;
    if (r16(pr + ppComponentLength) == 0 && r8(pr + ppFoundDelimiter))
        w16(pr + ppStartOffset, (u16)(r16(pr + ppStartOffset) + 1));   // past a leading ':'
    while (r8(pr + ppMoreName)) {
        r = Regs();
        r.a[0] = pr;
        e = utCall(utGetPathComponentName, r);
        if (e) return e;
        u16 start = r16(pr + ppStartOffset), len = r16(pr + ppComponentLength);
        if (len == 0) {                                 // "::": up one
            if (parent == kRootID) return bdNamErr;
            parent = parentOf(parent);
            w16(pr + ppStartOffset, (u16)(start + 1));
        } else if (r8(pr + ppMoreName)) {               // a folder on the way
            u8 name[32];
            int n = len > 31 ? 31 : len;
            for (int i = 0; i < n; i++) name[i] = r8(namePtr + 1 + start + i);
            u32 id = findInDir(parent, name, n);
            HostStat st;
            if (!id || !pathOf(id, path2, sizeof path2) || !fs.stat(path2, st) || !st.dir) return dirNFErr;
            parent = id;
            w16(pr + ppStartOffset, (u16)(start + len + 1));
        }
    }
    u16 start = r16(pr + ppStartOffset), len = r16(pr + ppComponentLength);
    if (len == 0) return noErr;                         // ended with ':': the folder
    if (len > 31) return bdNamErr;
    for (int i = 0; i < len; i++) leaf[i] = r8(namePtr + 1 + start + i);
    leafLen = len;
    return noErr;
}

s16 SharedFolder::itemOf(u32 pb, u32 dirID, u32 &id, bool noVolName)
{
    u32 parent;
    u8 leaf[32];
    int n;
    s16 e = resolve(pb, dirID, parent, leaf, n, noVolName);
    if (e) return e;
    if (n == 0) { id = parent; return noErr; }
    id = findInDir(parent, leaf, n);
    return id ? noErr : fnfErr;
}

// ================================================================ Finder info, resource forks

bool SharedFolder::sidecarPath(u32 id, char *out, u32 max)
{
    if (id == kRootID) return false;
    if (!pathOf(parentOf(id), out, max)) return false;
    if (out[0]) sappend(out, "/", max);
    sappend(out, "._", max);
    sappend(out, hostName(id), max);
    return slen(out) + 1 < max;
}

bool SharedFolder::readDouble(int f, Double &d)
{
    u8 h[26 + 12 * 8];
    s32 n = fs.read(f, 0, h, sizeof h);
    d.finfoOff = d.finfoLen = d.rsrcOff = d.rsrcLen = 0;
    d.ours = false;
    if (n < 26 || get32(h) != adMagic) return false;
    u32 count = get16(h + 24);
    for (u32 i = 0; i < count && 26 + 12 * (i + 1) <= (u32)n; i++) {
        const u8 *e = h + 26 + 12 * i;
        if (get32(e) == 9) { d.finfoOff = get32(e + 4); d.finfoLen = get32(e + 8); }
        if (get32(e) == 2) { d.rsrcOff = get32(e + 4); d.rsrcLen = get32(e + 8); }
    }
    d.ours = count == 2 && d.finfoOff == adFinfo && d.finfoLen == 32 && d.rsrcOff == adRsrc;
    return true;
}

int SharedFolder::openDouble(u32 id, bool write, Double &d)
{
    char sp[1024];
    if (!sidecarPath(id, sp, sizeof sp)) return -1;
    int f = fs.open(sp, write ? HostFS::ReadWrite : HostFS::ReadOnly);
    if (f >= 0 && readDouble(f, d) && (d.ours || !write)) return f;
    if (!write) { if (f >= 0) fs.close(f); return -1; }
    // Write it in our layout, keeping what a foreign one held (macOS
    // writes its own: extended attributes after the Finder info).
    u8 finfo[32] = {};
    u8 *rsrc = 0;
    u32 rsrcLen = 0;
    if (f >= 0) {
        if (d.finfoLen >= 32) fs.read(f, d.finfoOff, finfo, 32);
        if (d.rsrcLen && d.rsrcLen < 64u * 1024 * 1024) {
            rsrc = new u8[d.rsrcLen];
            rsrcLen = (u32)fs.read(f, d.rsrcOff, rsrc, d.rsrcLen);
        }
        fs.close(f);
    }
    f = fs.open(sp, HostFS::Create);
    if (f < 0) { delete[] rsrc; return -1; }
    fs.truncate(f, 0);
    u8 h[adRsrc];
    ourHeader(h, finfo, rsrcLen);
    fs.write(f, 0, h, adRsrc);
    if (rsrcLen) fs.write(f, adRsrc, rsrc, rsrcLen);
    delete[] rsrc;
    d.finfoOff = adFinfo; d.finfoLen = 32; d.rsrcOff = adRsrc; d.rsrcLen = rsrcLen; d.ours = true;
    return f;
}

void SharedFolder::getFinderInfo(u32 id, bool dir, u8 info[32])
{
    for (int i = 0; i < 32; i++) info[i] = 0;
    Double d;
    int f = openDouble(id, false, d);
    if (f >= 0) {
        if (d.finfoLen) fs.read(f, d.finfoOff, info, d.finfoLen < 32 ? d.finfoLen : 32);
        fs.close(f);
        if (dir || get32(info) || get32(info + 4)) return;
    }
    if (!dir) {
        u32 type, creator;
        guessType(hostName(id), type, creator);
        put32(info, type);
        put32(info + 4, creator);
    }
}

bool SharedFolder::setFinderInfo(u32 id, bool dir, const u8 info[32])
{
    (void)dir;
    u8 now[32];
    getFinderInfo(id, dir, now);
    bool same = true;
    for (int i = 0; i < 32; i++) same = same && now[i] == info[i];
    if (same) return true;                              // (no sidecar for nothing)
    Double d;
    int f = openDouble(id, true, d);
    if (f < 0) return false;
    bool ok = fs.write(f, adFinfo, info, 32) == 32;
    fs.close(f);
    return ok;
}

u32 SharedFolder::rsrcSize(u32 id)
{
    for (u32 i = 0; i < kMaxOpen; i++) if (open[i].used && open[i].id == id && open[i].rsrc) return open[i].len;
    Double d;
    int f = openDouble(id, false, d);
    if (f < 0) return 0;
    fs.close(f);
    return d.rsrcLen;
}

// ================================================================ the HFS component

s16 SharedFolder::hfsProc(u32 vcb, u16 selectCode, u32 pb)
{
    u16 trap = selectCode & 0xF0FF;
    bool hfs = (selectCode & 0x0200) != 0;
#ifdef SHARED_DEBUG
    fprintf(stderr, "  hfs %04X pb %08X vcb %08X\n", selectCode, pb, vcb);
#endif
    u32 dirID = hfs ? r32(pb + ioDirID) : 0;
    switch (trap) {
    case 0xA000: return openFork(pb, dirID, vcb, false);
    case 0xA001: return closeFork(pb);
    case 0xA002: return readWrite(pb, false);
    case 0xA003: return readWrite(pb, true);
    case 0xA007: return getVolInfo(pb, hfs);
    case 0xA008: return create(pb, dirID, false);
    case 0xA009: return removeItem(pb, dirID);
    case 0xA00A: return openFork(pb, dirID, vcb, true);
    case 0xA00B: return renameItem(pb, dirID);
    case 0xA00C: return getFileInfo(pb, hfs, dirID);
    case 0xA00D: return setFileInfo(pb, hfs, dirID);
    case 0xA00E: {                                      // UnmountVol
        closeAll();
        Regs r = {};
        r.a[0] = vcb;
        s16 e = utCall(utDisposeVCB, r);
        if (!e) { mounted = false; status = "put away (restart the Mac to get it back)"; }
        return e;
    }
    case 0xA00F: return (s16)r16(pb + ioVRefNum) == (s16)driveNum ? noErr : extFSErr;   // MountVol
    case 0xA010: w32(pb + ioActCount, r32(pb + ioReqCount)); return noErr;   // Allocate
    case 0xA011: return getEOF(pb);
    case 0xA012: return setEOF(pb);
    case 0xA013: case 0xA045: return noErr;             // FlushVol, FlushFile
    case 0xA014: return getVol(pb);
    case 0xA015: return setVol(pb, hfs, vcb);
    case 0xA017: case 0xA035: return noErr;             // Eject, Offline
    case 0xA018: return getFPos(pb);
    case 0xA041: case 0xA042: case 0xA043: return noErr;   // SetFilLock, RstFilLock, SetFilType
    case 0xA044: return setFPos(pb);
    case 0x0001: return openWD(pb);
    case 0x0002: return closeWD(pb);
    case 0x0005: return catMove(pb);
    case 0x0006: return create(pb, r32(pb + ioDirID), true);
    case 0x0007: return getWDInfo(pb, vcb);
    case 0x0008: return getFCBInfo(pb, vcb);
    case 0x0009: return getCatInfo(pb);
    case 0x000A: return setCatInfo(pb);
    case 0x000B: return noErr;                          // SetVolInfo
    case 0x0017: return exchangeFiles(pb);
    case 0x001A: return openFork(pb, r32(pb + ioDirID), vcb, false);   // OpenDF
    case 0x001B: return makeFSSpec(pb);
    case 0x0030: return getVolParms(pb);
    case 0x0041: return volumeMount(pb);
    default: return paramErr;
    }
}

s16 SharedFolder::getVolInfo(u32 pb, bool hfs)
{
    u32 np = r32(pb + ioNamePtr);
    if (np) {
        w8(np, (u8)slen(volName));
        for (u32 i = 0; i < slen(volName); i++) w8(np + 1 + i, (u8)volName[i]);
    }
    HostStat st = {};
    fs.stat("", st);
    u32 t = st.mtime + kMacEpoch;
    if (volDate > t) t = volDate;
    int files = 0, dirs = 0;
    if (Listing *L = list(kRootID))
        for (int i = 0; i < L->count; i++) {
            HostStat s2;
            if (pathOf(L->ids[i], path, sizeof path) && fs.stat(path, s2)) { if (s2.dir) dirs++; else files++; }
        }
    u64 freeBlocks = fs.freeBytes() / kAlBlk;
    // The creation date never changes while mounted: the Finder tells disks
    // apart by it (a new one closed the Shared disk's windows).
    w32(pb + ioVCrDate, vcbAddr ? r32(vcbAddr + vcbCrDate) : t);
    w32(pb + ioVLsMod, t);
    w16(pb + ioVAtrb, 0);
    w16(pb + ioVNmFls, (u16)files);
    w16(pb + ioVBitMap, 0);
    w16(pb + ioAllocPtr, 0);
    w16(pb + ioVNmAlBlks, 0xFFFF);
    w32(pb + ioVAlBlkSiz, kAlBlk);
    w32(pb + ioVClpSiz, kClump);
    w16(pb + ioAlBlSt, 0);
    w32(pb + ioVNxtCNID, nextID);
    w16(pb + ioVFrBlk, (u16)(freeBlocks > 0xFFFF ? 0xFFFF : freeBlocks));
    if (hfs) {
        w16(pb + ioVSigWord, 0x4244);
        w16(pb + ioVDrvInfo, driveNum);
        w16(pb + ioVDRefNum, (u16)drvrRefNum);
        w16(pb + ioVFSID, kFSID);
        w32(pb + ioVBkUp, 0);
        w16(pb + ioVSeqNum, 0);
        w32(pb + ioVWrCnt, 0);
        w32(pb + ioVFilCnt, (u32)files);
        w32(pb + ioVDirCnt, (u32)dirs);
        for (u32 i = 0; i < 32; i++) w8(pb + ioVFndrInfo + i, 0);
    }
    return noErr;
}

s16 SharedFolder::getVolParms(u32 pb)
{
    u32 n = r32(pb + ioReqCount);
    if (n > kVolParmsSize) n = kVolParmsSize;
    w32(pb + ioActCount, n);
    u32 p = r32(pb + ioBuffer);
    // No mini-Finder, no renaming the volume, no local sync, trash offline,
    // no switching to it, no boot blocks or System Folder; an external FS.
    u32 attrib = (1u << 29) | (1u << 28) | (1u << 27) | (1u << 26) | (1u << 25) | (1u << 19) | (1u << 17) | (1u << 16);
    if (n > vMVersion) w16(p + vMVersion, 2);
    if (n > vMAttrib) w32(p + vMAttrib, attrib);
    if (n > vMLocalHand) w32(p + vMLocalHand, 0);
    if (n > vMServerAdr) w32(p + vMServerAdr, 0);
    if (n > vMVolumeGrade) w32(p + vMVolumeGrade, 0);
    if (n > vMForeignPrivID) w16(p + vMForeignPrivID, 0);
    return noErr;
}

// The catalog fields of an item into a CInfoPBRec / HFileParam.
static inline u32 physical(u32 n) { return (n + kAlBlk - 1) / kAlBlk * kAlBlk; }

s16 SharedFolder::getCatInfo(u32 pb)
{
    s16 index = (s16)r16(pb + ioFDirIndex);
    u32 id;
    if (index < 0) {                                    // the folder ioDrDirID
        id = r32(pb + ioDrDirID);
        if (!find(id)) return dirNFErr;
    } else if (index == 0) {
        s16 e = itemOf(pb, r32(pb + ioDirID), id);
        if (e) return e;
    } else {                                            // the index-th item in a folder
        u32 dir;
        s16 e = currentDir(pb, r32(pb + ioDirID), dir, true);
        if (e) return e;
        Listing *L = find(dir) ? list(dir) : 0;
        if (!L) return dirNFErr;
        if (index > L->count) return fnfErr;
        id = L->ids[index - 1];
    }
    HostStat st;
    if (!pathOf(id, path, sizeof path) || !fs.stat(path, st)) { forget(id); invalidate(); return fnfErr; }
    if (index < 0 && !st.dir) return dirNFErr;

    u32 np = r32(pb + ioNamePtr);
    if (np && index != 0) {
        u8 m[32];
        int n = id == kRootID ? (int)slen(volName) : hostToMac(hostName(id), m, fs.namesUtf8());
        if (id == kRootID) for (int i = 0; i < n; i++) m[i] = (u8)volName[i];
        w8(np, (u8)n);
        for (int i = 0; i < n; i++) w8(np + 1 + i, m[i]);
#ifdef SHARED_DEBUG
        fprintf(stderr, "    name at %08X: %d '%c%c%c' read back %d %02X\n", np, n, m[0], m[1], m[2], r8(np), r8(np + 1));
#endif
    }
    u8 attrib = st.dir ? 0x10 : 0;
    if (st.readOnly) attrib |= 0x01;
    for (u32 i = 0; i < kMaxOpen; i++)
        if (open[i].used && open[i].id == id) attrib |= (u8)(0x80 | (open[i].rsrc ? 0x04 : 0x08));
    w16(pb + ioFRefNum, 0);
    w8(pb + ioFlAttrib, attrib);
    w8(pb + ioACUser, 0);
    w32(pb + ioDirID, id);
    w32(pb + ioFlParID, parentOf(id));
    u32 t = st.mtime + kMacEpoch;
    Listing *L = st.dir ? list(id) : 0;
    if (L) {
        // A folder's date: its newest item's (FAT does not change a folder's
        // date when files come and go), moved on when items vanish too, so
        // the Finder notices and redraws an open window.
        Item *it = find(id);
        u32 newest = (L->newest > st.mtime ? L->newest : st.mtime) + kMacEpoch;
        if (it && it->sig != L->sig) { it->date = it->sig && it->date >= newest ? it->date + 1 : newest; it->sig = L->sig; }
        if (it && it->date > t) t = it->date;
    }
    w32(pb + ioFlCrDat, st.mtime + kMacEpoch);
    w32(pb + ioFlMdDat, t);
    w32(pb + ioFlBkDat, 0);
    u8 info[32];
    getFinderInfo(id, st.dir, info);
    for (int i = 0; i < 16; i++) w8(pb + ioFlFndrInfo + (u32)i, info[i]);
    for (int i = 0; i < 16; i++) w8(pb + ioFlXFndrInfo + (u32)i, info[16 + i]);
    if (st.dir) {
        w16(pb + ioDrNmFls, (u16)(L ? L->count : 0));
    } else {
        u32 rs = rsrcSize(id);
        u32 ds = st.size;
        for (u32 i = 0; i < kMaxOpen; i++) if (open[i].used && open[i].id == id && !open[i].rsrc) ds = fs.size(open[i].f);
        w16(pb + ioFlStBlk, 0);
        w32(pb + ioFlLgLen, ds);
        w32(pb + ioFlPyLen, physical(ds));
        w16(pb + ioFlRStBlk, 0);
        w32(pb + ioFlRLgLen, rs);
        w32(pb + ioFlRPyLen, physical(rs));
        w32(pb + ioFlClpSiz, 0);
    }
    return noErr;
}

s16 SharedFolder::setCatInfo(u32 pb)
{
    u32 id;
    s16 e = itemOf(pb, r32(pb + ioDirID), id);
    if (e) return e;
    HostStat st;
    if (!pathOf(id, path, sizeof path) || !fs.stat(path, st)) return fnfErr;
    if (id == kRootID) return noErr;
    u8 info[32];
    for (int i = 0; i < 16; i++) info[i] = r8(pb + ioFlFndrInfo + (u32)i);
    for (int i = 0; i < 16; i++) info[16 + i] = r8(pb + ioFlXFndrInfo + (u32)i);
    return setFinderInfo(id, st.dir, info) ? noErr : ioErr;
}

s16 SharedFolder::getFileInfo(u32 pb, bool hfs, u32 dirID)
{
    s16 index = (s16)r16(pb + ioFDirIndex);
    u32 id;
    if (index <= 0) {
        s16 e = itemOf(pb, dirID, id);
        if (e) return e;
    } else {                                            // the index-th file (folders do not count)
        u32 dir;
        s16 e = currentDir(pb, dirID, dir, true);
        if (e) return e;
        Listing *L = find(dir) ? list(dir) : 0;
        if (!L) return dirNFErr;
        int k = 0;
        id = 0;
        for (int i = 0; i < L->count && !id; i++) {
            HostStat s2;
            if (pathOf(L->ids[i], path, sizeof path) && fs.stat(path, s2) && !s2.dir && ++k == index) id = L->ids[i];
        }
        if (!id) return fnfErr;
    }
    HostStat st;
    if (!pathOf(id, path, sizeof path) || !fs.stat(path, st) || st.dir) return fnfErr;
    u32 np = r32(pb + ioNamePtr);
    if (np && index > 0) {
        u8 m[32];
        int n = hostToMac(hostName(id), m, fs.namesUtf8());
        w8(np, (u8)n);
        for (int i = 0; i < n; i++) w8(np + 1 + i, m[i]);
    }
    w16(pb + ioFRefNum, 0);
    u8 attrib = st.readOnly ? 1 : 0;
    for (u32 i = 0; i < kMaxOpen; i++)
        if (open[i].used && open[i].id == id) attrib |= (u8)(0x80 | (open[i].rsrc ? 0x04 : 0x08));
    w8(pb + ioFlAttrib, attrib);
    w32(pb + ioDirID, id);
    u32 t = st.mtime + kMacEpoch;
    w32(pb + ioFlCrDat, t);
    w32(pb + ioFlMdDat, t);
    u8 info[32];
    getFinderInfo(id, false, info);
    for (int i = 0; i < 16; i++) w8(pb + ioFlFndrInfo + (u32)i, info[i]);
    u32 rs = rsrcSize(id);
    w16(pb + ioFlStBlk, 0);
    w32(pb + ioFlLgLen, st.size);
    w32(pb + ioFlPyLen, physical(st.size));
    w16(pb + ioFlRStBlk, 0);
    w32(pb + ioFlRLgLen, rs);
    w32(pb + ioFlRPyLen, physical(rs));
    if (hfs) {
        for (int i = 0; i < 16; i++) w8(pb + ioFlXFndrInfo + (u32)i, info[16 + i]);
        w32(pb + ioFlBkDat, 0);
        w32(pb + ioFlParID, parentOf(id));
        w32(pb + ioFlClpSiz, 0);
    }
    return noErr;
}

s16 SharedFolder::setFileInfo(u32 pb, bool hfs, u32 dirID)
{
    u32 id;
    s16 e = itemOf(pb, dirID, id);
    if (e) return e;
    HostStat st;
    if (!pathOf(id, path, sizeof path) || !fs.stat(path, st) || st.dir) return fnfErr;
    u8 info[32];
    getFinderInfo(id, false, info);
    for (int i = 0; i < 16; i++) info[i] = r8(pb + ioFlFndrInfo + (u32)i);
    if (hfs) for (int i = 0; i < 16; i++) info[16 + i] = r8(pb + ioFlXFndrInfo + (u32)i);
    return setFinderInfo(id, false, info) ? noErr : ioErr;
}

// ================================================================ files

SharedFolder::OpenFork *SharedFolder::forkFor(u32 fcb)
{
    if (!fcb) return 0;
    u32 index = r32(fcb + fcbCatPos);
    if (index >= kMaxOpen || !open[index].used) return 0;
    return &open[index];
}

void SharedFolder::closeAll()
{
    for (u32 i = 0; i < kMaxOpen; i++)
        if (open[i].used) { updateDoubleLength(open[i]); fs.close(open[i].f); open[i].used = false; }
}

void SharedFolder::updateDoubleLength(OpenFork &o)
{
    if (!o.rsrc || !o.write) return;
    u8 b[4];
    put32(b, o.len);
    fs.write(o.f, adRsrcLenAt, b, 4);
}

s16 SharedFolder::openFork(u32 pb, u32 dirID, u32 vcb, bool rsrc)
{
    u32 id;
    s16 e = itemOf(pb, dirID, id);
    if (e) return e;
    HostStat st;
    if (!pathOf(id, path, sizeof path) || !fs.stat(path, st)) return fnfErr;
    if (st.dir) return fnfErr;
    u8 perm = r8(pb + ioPermssn);
    bool write = perm != 1 && !st.readOnly;
    if ((perm == 2 || perm == 3 || perm == 4) && st.readOnly) return permErr;
    u32 slot = 0;
    while (slot < kMaxOpen && open[slot].used) slot++;
    if (slot == kMaxOpen) return tmfoErr;
    OpenFork &o = open[slot];
    o.id = id; o.rsrc = rsrc; o.write = write; o.mark = 0;
    if (rsrc) {
        Double d;
        o.f = openDouble(id, write, d);
        if (o.f < 0 && write) return ioErr;
        o.base = d.rsrcOff;
        o.len = o.f >= 0 ? d.rsrcLen : 0;              // no sidecar: an empty fork
    } else {
        o.f = fs.open(path, write ? HostFS::ReadWrite : HostFS::ReadOnly);
        if (o.f < 0 && write) { o.f = fs.open(path, HostFS::ReadOnly); o.write = write = false; }
        if (o.f < 0) return ioErr;
        o.base = 0;
        o.len = fs.size(o.f);
    }

    Regs r = {};
    r.a[0] = pb + ioRefNum;
    r.a[1] = data + dRet;
    e = utCall(utAllocateFCB, r);
    if (e) { if (o.f >= 0) fs.close(o.f); return e; }
    u32 fcb = r32(data + dRet);
    o.used = true;
    o.refNum = r16(pb + ioRefNum);
    w32(fcb + fcbFlNm, id);
    w8(fcb + fcbFlags, (u8)((write ? 0x01 : 0) | (rsrc ? 0x02 : 0) | (st.readOnly ? 0x20 : 0)));
    w16(fcb + fcbSBlk, 0);
    w32(fcb + fcbEOF, o.len);
    w32(fcb + fcbPLen, physical(o.len));
    w32(fcb + fcbCrPs, 0);
    w32(fcb + fcbVPtr, vcb);
    w32(fcb + fcbClmpSize, kClump);
    u8 info[32];
    getFinderInfo(id, false, info);
    w32(fcb + fcbFType, get32(info));
    w32(fcb + fcbCatPos, slot);
    w32(fcb + fcbDirID, parentOf(id));
    u8 m[32];
    int n = hostToMac(hostName(id), m, fs.namesUtf8());
    w8(fcb + fcbCName, (u8)n);
    for (int i = 0; i < n; i++) w8(fcb + fcbCName + 1 + (u32)i, m[i]);
    return noErr;
}

s16 SharedFolder::closeFork(u32 pb)
{
    Regs r = {};
    r.d[0] = r16(pb + ioRefNum);
    r.a[0] = data + dRet;
    if (utCall(utResolveFCB, r)) return rfNumErr;
    u32 fcb = r32(data + dRet);
    OpenFork *o = forkFor(fcb);
    if (!o) return fnOpnErr;
    updateDoubleLength(*o);
    if (o->f >= 0) fs.close(o->f);
    o->used = false;
    w32(fcb + fcbCatPos, 0xFFFFFFFFu);
    invalidate();
    r = Regs();
    r.d[0] = r16(pb + ioRefNum);
    return utCall(utReleaseFCB, r);
}

s16 SharedFolder::readWrite(u32 pb, bool write)
{
    if ((s32)r32(pb + ioReqCount) < 0) return paramErr;
    Regs r = {};
    r.d[0] = r16(pb + ioRefNum);
    r.a[0] = data + dRet;
    if (utCall(utResolveFCB, r)) return rfNumErr;
    u32 fcb = r32(data + dRet);
    OpenFork *o = forkFor(fcb);
    if (!o) return fnOpnErr;
    if (write && !o->write) return wrPermErr;

    u16 mode = r16(pb + ioPosMode);
    s32 off = (s32)r32(pb + ioPosOffset);
    s64 pos;
    switch (mode & 3) {
    case 1: pos = off; break;                           // from the start
    case 2: pos = (s64)o->len + off; break;             // from the end
    case 3: pos = (s64)o->mark + off; break;            // from the mark
    default: pos = o->mark; break;                      // at the mark
    }
    if (pos < 0) return posErr;
    u32 want = r32(pb + ioReqCount), done = 0;
    u32 buf = r32(pb + ioBuffer);
    static u8 *chunk = 0;                               // (on the heap: the Pi's kernel image is limited)
    enum { kChunk = 16384 };
    if (!chunk) chunk = new u8[kChunk];
    s16 result = noErr;
    if (!write) {
        bool newline = (mode & 0x80) != 0;
        u8 nl = (u8)(mode >> 8);
        if ((u64)pos > o->len) { result = eofErr; want = 0; }
        while (done < want) {
            u32 n = want - done;
            if (n > kChunk) n = kChunk;
            if ((u64)pos + done + n > o->len) n = o->len - (u32)pos - done;
            if (n == 0) { result = eofErr; break; }
            s32 got = o->f >= 0 ? fs.read(o->f, o->base + (u32)pos + done, chunk, n) : 0;
            if (got <= 0) { result = got < 0 ? ioErr : eofErr; break; }
            u32 take = (u32)got;
            if (newline) for (u32 i = 0; i < take; i++) if (chunk[i] == nl) { take = i + 1; newline = false; want = done + take; break; }
            hostToMacMem(buf + done, chunk, take);
            done += take;
        }
    } else {
        while (done < want) {
            u32 n = want - done;
            if (n > kChunk) n = kChunk;
            macToHostMem(buf + done, chunk, n);
            s32 put = fs.write(o->f, o->base + (u32)pos + done, chunk, n);
            if (put != (s32)n) { result = dskFulErr; if (put > 0) done += (u32)put; break; }
            done += n;
        }
        if ((u32)pos + done > o->len) {
            o->len = (u32)pos + done;
            w32(fcb + fcbEOF, o->len);
            w32(fcb + fcbPLen, physical(o->len));
            if (o->rsrc) updateDoubleLength(*o);
            Regs a = {};
            a.d[0] = r16(pb + ioRefNum);
            utCall(utAdjustEOF, a);
        }
        invalidate();
    }
    o->mark = (u32)pos + done;
    w32(pb + ioActCount, done);
    w32(pb + ioPosOffset, o->mark);
    w32(fcb + fcbCrPs, o->mark);
    return result;
}

s16 SharedFolder::getEOF(u32 pb)
{
    Regs r = {};
    r.d[0] = r16(pb + ioRefNum);
    r.a[0] = data + dRet;
    if (utCall(utResolveFCB, r)) return rfNumErr;
    OpenFork *o = forkFor(r32(data + dRet));
    if (!o) return fnOpnErr;
    w32(pb + ioMisc, o->len);
    return noErr;
}

s16 SharedFolder::setEOF(u32 pb)
{
    Regs r = {};
    r.d[0] = r16(pb + ioRefNum);
    r.a[0] = data + dRet;
    if (utCall(utResolveFCB, r)) return rfNumErr;
    u32 fcb = r32(data + dRet);
    OpenFork *o = forkFor(fcb);
    if (!o) return fnOpnErr;
    if (!o->write) return wrPermErr;
    u32 size = r32(pb + ioMisc);
    if (!fs.truncate(o->f, o->base + size)) return dskFulErr;
    o->len = size;
    if (o->mark > size) o->mark = size;
    updateDoubleLength(*o);
    w32(fcb + fcbEOF, size);
    w32(fcb + fcbPLen, physical(size));
    r = Regs();
    r.d[0] = r16(pb + ioRefNum);
    utCall(utAdjustEOF, r);
    invalidate();
    return noErr;
}

s16 SharedFolder::getFPos(u32 pb)
{
    Regs r = {};
    r.d[0] = r16(pb + ioRefNum);
    r.a[0] = data + dRet;
    if (utCall(utResolveFCB, r)) return rfNumErr;
    OpenFork *o = forkFor(r32(data + dRet));
    if (!o) return fnOpnErr;
    w32(pb + ioReqCount, 0);
    w32(pb + ioActCount, 0);
    w16(pb + ioPosMode, 0);
    w32(pb + ioPosOffset, o->mark);
    return noErr;
}

s16 SharedFolder::setFPos(u32 pb)
{
    Regs r = {};
    r.d[0] = r16(pb + ioRefNum);
    r.a[0] = data + dRet;
    if (utCall(utResolveFCB, r)) return rfNumErr;
    u32 fcb = r32(data + dRet);
    OpenFork *o = forkFor(fcb);
    if (!o) return fnOpnErr;
    s32 off = (s32)r32(pb + ioPosOffset);
    s64 pos;
    switch (r16(pb + ioPosMode) & 3) {
    case 1: pos = off; break;
    case 2: pos = (s64)o->len + off; break;
    case 3: pos = (s64)o->mark + off; break;
    default: pos = o->mark; break;
    }
    if (pos < 0) return posErr;
    s16 result = noErr;
    if ((u64)pos > o->len) { pos = o->len; result = eofErr; }
    o->mark = (u32)pos;
    w32(pb + ioPosOffset, o->mark);
    w32(fcb + fcbCrPs, o->mark);
    return result;
}

s16 SharedFolder::getFCBInfo(u32 pb, u32 vcb)
{
    u32 fcb = 0;
    s16 index = (s16)r16(pb + ioFCBIndx);
    Regs r = {};
    if (index == 0) {
        r.d[0] = r16(pb + ioRefNum);
        r.a[0] = data + dRet;
        if (utCall(utResolveFCB, r)) return rfNumErr;
        fcb = r32(data + dRet);
    } else {
        w16(pb + ioRefNum, 0);
        for (int i = 0; i < index; i++) {
            r = Regs();
            r.a[0] = vcb;
            r.a[1] = pb + ioRefNum;
            r.a[2] = data + dRet;
            s16 e = utCall(utIndexFCB, r);
            if (e) return e;
            fcb = r32(data + dRet);
        }
    }
    if (!fcb) return rfNumErr;
    u32 np = r32(pb + ioNamePtr);
    if (np) for (u32 i = 0; i < 32; i++) w8(np + i, r8(fcb + fcbCName + i));
    w32(pb + ioFCBFlNm, r32(fcb + fcbFlNm));
    w8(pb + ioFCBFlags, r8(fcb + fcbFlags));
    w16(pb + ioFCBStBlk, 0);
    w32(pb + ioFCBEOF, r32(fcb + fcbEOF));
    w32(pb + ioFCBPLen, r32(fcb + fcbPLen));
    w32(pb + ioFCBCrPs, r32(fcb + fcbCrPs));
    w16(pb + ioFCBVRefNum, r16(r32(fcb + fcbVPtr) + vcbVRefNum));
    w32(pb + ioFCBClpSiz, r32(fcb + fcbClmpSize));
    w32(pb + ioFCBParID, r32(fcb + fcbDirID));
    return noErr;
}

// ================================================================ the catalog's changes

s16 SharedFolder::create(u32 pb, u32 dirID, bool dir)
{
    u32 parent;
    u8 leaf[32];
    int n;
    s16 e = resolve(pb, dirID, parent, leaf, n);
    if (e) return e;
    if (n == 0) return bdNamErr;
    if (findInDir(parent, leaf, n)) return dupFNErr;
    char host[512];
    macToHost(leaf, n, host, sizeof host, fs.namesUtf8());
    if (!pathOf(parent, path, sizeof path)) return dirNFErr;
    if (path[0]) sappend(path, "/", sizeof path);
    sappend(path, host, sizeof path);
    if (dir) {
        if (!fs.makeDir(path)) return dskFulErr;
    } else {
        int f = fs.open(path, HostFS::Create);
        if (f < 0) return dskFulErr;
        fs.close(f);
    }
    invalidate();
    u32 id = itemFor(parent, host);
    if (dir) w32(pb + ioDirID, id);
    return noErr;
}

s16 SharedFolder::removeItem(u32 pb, u32 dirID)
{
    u32 id;
    s16 e = itemOf(pb, dirID, id);
    if (e) return e;
    if (id == kRootID) return fBsyErr;
    for (u32 i = 0; i < kMaxOpen; i++) if (open[i].used && open[i].id == id) return fBsyErr;
    HostStat st;
    if (!pathOf(id, path, sizeof path) || !fs.stat(path, st)) return fnfErr;
    if (st.readOnly) return fLckdErr;
    if (st.dir) {
        // Only an empty folder; hidden files in it (AppleDouble, .DS_Store) go too.
        Listing *L = list(id);
        if (!L) return ioErr;
        if (L->count) return fBsyErr;
        int d = fs.openDir(path);
        char name[512];
        while (d >= 0 && fs.readDir(d, name, sizeof name)) {
            if (seq(name, ".") || seq(name, "..")) continue;
            scopy(path2, path, sizeof path2);
            sappend(path2, "/", sizeof path2);
            sappend(path2, name, sizeof path2);
            fs.remove(path2);
        }
        if (d >= 0) fs.closeDir(d);
    }
    if (!fs.remove(path)) return st.dir ? fBsyErr : ioErr;
    if (sidecarPath(id, path2, sizeof path2)) fs.remove(path2);
    forget(id);
    invalidate();
    return noErr;
}

s16 SharedFolder::renameItem(u32 pb, u32 dirID)
{
    u32 id;
    s16 e = itemOf(pb, dirID, id);
    if (e) return e;
    if (id == kRootID) return paramErr;                 // (the volume's name is fixed)
    u32 np = r32(pb + ioMisc);
    if (!np) return bdNamErr;
    int n = r8(np);
    if (n < 1 || n > 31) return bdNamErr;
    u8 leaf[32];
    for (int i = 0; i < n; i++) {
        leaf[i] = r8(np + 1 + (u32)i);
        if (leaf[i] == ':') return bdNamErr;
    }
    u32 parent = parentOf(id);
    u32 other = findInDir(parent, leaf, n);
    if (other && other != id) return dupFNErr;
    char host[512];
    macToHost(leaf, n, host, sizeof host, fs.namesUtf8());
    if (!pathOf(id, path, sizeof path) || !pathOf(parent, path2, sizeof path2)) return fnfErr;
    if (path2[0]) sappend(path2, "/", sizeof path2);
    sappend(path2, host, sizeof path2);
    if (!fs.rename(path, path2)) return ioErr;
    char oldSide[1024], newSide[1024];
    bool side = sidecarPath(id, oldSide, sizeof oldSide);
    move(id, parent, host);
    if (side && sidecarPath(id, newSide, sizeof newSide)) fs.rename(oldSide, newSide);
    invalidate();
    return noErr;
}

s16 SharedFolder::catMove(u32 pb)
{
    u32 id;
    s16 e = itemOf(pb, r32(pb + ioDirID), id);
    if (e) return e;
    if (id == kRootID) return badMovErr;
    // The destination: ioNewName (a folder name or none) from ioNewDirID.
    u32 tmp = data + dPB;
    for (u32 i = 0; i < 52; i++) w8(tmp + i, r8(pb + i));
    w32(tmp + ioNamePtr, r32(pb + ioNewName));
    u32 dest;
    e = itemOf(tmp, r32(pb + ioNewDirID), dest);
    if (e) return e == fnfErr ? (s16)dirNFErr : e;
    HostStat st;
    if (!pathOf(dest, path2, sizeof path2) || !fs.stat(path2, st) || !st.dir) return dirNFErr;
    for (u32 d = dest; d != 1 && d; d = parentOf(d)) if (d == id) return badMovErr;   // into itself
    u8 m[32];
    int n = hostToMac(hostName(id), m, fs.namesUtf8());
    if (findInDir(dest, m, n)) return dupFNErr;
    if (!pathOf(id, path, sizeof path)) return fnfErr;
    char host[512];
    scopy(host, hostName(id), sizeof host);
    if (path2[0]) sappend(path2, "/", sizeof path2);
    sappend(path2, host, sizeof path2);
    if (!fs.rename(path, path2)) return ioErr;
    char oldSide[1024], newSide[1024];
    bool side = sidecarPath(id, oldSide, sizeof oldSide);
    move(id, dest, host);
    if (side && sidecarPath(id, newSide, sizeof newSide)) fs.rename(oldSide, newSide);
    invalidate();
    return noErr;
}

s16 SharedFolder::exchangeFiles(u32 pb)
{
    // Swap two files' contents (data, resource fork, Finder info stay with
    // the contents); the names and IDs stay where they were.
    u32 a, b;
    s16 e = itemOf(pb, r32(pb + ioDirID), a);
    if (e) return e;
    u32 tmp = data + dPB;
    for (u32 i = 0; i < 52; i++) w8(tmp + i, r8(pb + i));
    w32(tmp + ioNamePtr, r32(pb + ioDestNamePtr));
    e = itemOf(tmp, r32(pb + ioDestDirID), b);
    if (e) return e;
    for (u32 i = 0; i < kMaxOpen; i++) if (open[i].used && (open[i].id == a || open[i].id == b)) return fBsyErr;
    char pa[1024], pbp[1024], ptmp[1024], sa[1024], sb[1024], stmp[1024];
    if (!pathOf(a, pa, sizeof pa) || !pathOf(b, pbp, sizeof pbp)) return fnfErr;
    scopy(ptmp, pa, sizeof ptmp);
    sappend(ptmp, ".exchange", sizeof ptmp);
    if (!fs.rename(pa, ptmp) || !fs.rename(pbp, pa) || !fs.rename(ptmp, pbp)) return ioErr;
    if (sidecarPath(a, sa, sizeof sa) && sidecarPath(b, sb, sizeof sb)) {
        HostStat st;
        bool hasA = fs.stat(sa, st), hasB = fs.stat(sb, st);
        scopy(stmp, sa, sizeof stmp);
        sappend(stmp, ".exchange", sizeof stmp);
        if (hasA) fs.rename(sa, stmp);
        if (hasB) fs.rename(sb, sa);
        if (hasA) fs.rename(stmp, sb);
    }
    invalidate();
    return noErr;
}

// ================================================================ working directories, default volume

s16 SharedFolder::openWD(u32 pb)
{
    Regs r = {};
    r.a[0] = pb;
    return utCall(utAllocateWDCB, r);
}

s16 SharedFolder::closeWD(u32 pb)
{
    Regs r = {};
    r.d[0] = r16(pb + ioVRefNum);
    return utCall(utReleaseWDCB, r);
}

s16 SharedFolder::getWDInfo(u32 pb, u32 vcb)
{
    u32 np = r32(pb + ioNamePtr);
    if (r16(pb + ioWDIndex) == 0 && r16(pb + ioVRefNum) == r16(vcb + vcbVRefNum)) {
        w32(pb + ioWDProcID, 0);
        w16(pb + ioWDVRefNum, r16(vcb + vcbVRefNum));
        if (np) for (u32 i = 0; i < 28; i++) w8(np + i, r8(vcb + vcbVN + i));
        w32(pb + ioWDDirID, kRootID);
        return noErr;
    }
    Regs r = {};
    r.d[0] = r32(pb + ioWDProcID);
    r.d[1] = r16(pb + ioWDIndex);
    r.d[2] = (u32)(s32)(s16)r16(pb + ioVRefNum);
    r.a[0] = data + dRet;
    s16 e = utCall(utResolveWDCB, r);
    if (e) return e;
    u32 wdcb = r32(data + dRet);
    w32(pb + ioWDProcID, r32(wdcb + wdProcID));
    w16(pb + ioWDVRefNum, r16(r32(wdcb + wdVCBPtr) + vcbVRefNum));
    if (np) for (u32 i = 0; i < 28; i++) w8(np + i, r8(r32(wdcb + wdVCBPtr) + vcbVN + i));
    w32(pb + ioWDDirID, r32(wdcb + wdDirID));
    return noErr;
}

s16 SharedFolder::getVol(u32 pb)
{
    Regs r = {};
    r.a[0] = pb;
    return utCall(utGetDefaultVol, r);
}

s16 SharedFolder::setVol(u32 pb, bool hfs, u32 vcb)
{
    u32 dir;
    s16 refNum;
    Regs r = {};
    if (hfs) {
        u32 id;
        s16 e = itemOf(pb, r32(pb + ioWDDirID), id);
        if (e) return e;
        HostStat st;
        if (!pathOf(id, path, sizeof path) || !fs.stat(path, st) || !st.dir) return dirNFErr;
        dir = id;
        refNum = (s16)r16(vcb + vcbVRefNum);
    } else {
        r.d[0] = (u32)(s32)(s16)r16(pb + ioVRefNum);
        if (utCall(utCheckWDRefNum, r)) { dir = kRootID; refNum = (s16)r16(vcb + vcbVRefNum); }
        else { dir = 0; refNum = (s16)r16(pb + ioVRefNum); }
    }
    r = Regs();
    r.d[0] = 0;
    r.d[1] = dir;
    r.d[2] = (u32)(s32)refNum;
    return utCall(utSetDefaultVol, r);
}

s16 SharedFolder::makeFSSpec(u32 pb)
{
    u32 parent;
    u8 leaf[32];
    int n;
    s16 e = resolve(pb, r32(pb + ioDirID), parent, leaf, n);
    if (e) return e;
    u32 spec = r32(pb + ioMisc);
    u32 parID = parent;
    s16 result = noErr;
    if (n == 0) {                                       // a folder (or the volume)
        if (parent == kRootID) {
            parID = 1;
            n = (int)slen(volName);
            for (int i = 0; i < n; i++) leaf[i] = (u8)volName[i];
        } else {
            parID = parentOf(parent);
            n = hostToMac(hostName(parent), leaf, fs.namesUtf8());
        }
    } else if (!findInDir(parent, leaf, n)) result = fnfErr;   // (the spec is still good for creating it)
    w16(spec, (u16)vRefNum);
    w32(spec + 2, parID);
    w8(spec + 6, (u8)n);
    for (int i = 0; i < n; i++) w8(spec + 7 + (u32)i, leaf[i]);
    return result;
}

// ================================================================ hibernation

void SharedFolder::state(StateIO &s)
{
    s.tag("SHFS");
    s.b(tried); s.b(mounted); s.v(glueAddr); s.v(data); s.v(stack); s.v(drvrRefNum); s.v(vRefNum); s.v(driveNum); s.v(vcbAddr);
    s.v(nextID); s.v(itemCount); s.v(poolUsed);
    if (itemCount > kMaxItems || poolUsed > kNamePool) { s.ok = false; return; }
    for (u32 i = 0; i < itemCount; i++) {
        Item &it = items[i];
        s.v(it.id); s.v(it.parent); s.v(it.nameOff); s.v(it.nameLen); s.b(it.used); s.v(it.sig); s.v(it.date);
    }
    s.raw(namePool, poolUsed);
    for (u32 i = 0; i < kMaxOpen; i++) {
        OpenFork &o = open[i];
        if (s.saving() && o.used) updateDoubleLength(o);
        s.b(o.used);
        if (!o.used) continue;
        s.v(o.id); s.b(o.rsrc); s.b(o.write); s.v(o.base); s.v(o.len); s.v(o.mark); s.v(o.refNum);
        if (!s.saving()) {                              // open the files again
            o.f = -1;
            if (o.rsrc) { Double d; o.f = openDouble(o.id, o.write, d); }
            else if (pathOf(o.id, path, sizeof path)) o.f = fs.open(path, o.write ? HostFS::ReadWrite : HostFS::ReadOnly);
        }
    }
    if (!s.saving()) {
        for (u32 i = 0; i < kHashSize; i++) hash[i] = 0;
        for (u32 i = 1; i < itemCount; i++) {
            if (!items[i].used) continue;
            u32 h = hashOf(items[i].parent, namePool + items[i].nameOff, items[i].nameLen);
            while (hash[h]) h = (h + 1) & (kHashSize - 1);
            hash[h] = i + 1;
        }
        invalidate();
        status = mounted ? "on the desktop" : "waiting for Mac OS";
    }
}

} // namespace macemu
