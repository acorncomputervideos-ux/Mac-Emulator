// The shared folder: a folder of the host (on the Pi, one on the SD card)
// as a disk on the Mac's desktop, read and written by both sides.
//
// Mac OS 7.5 and later have the File System Manager, through which a
// program can add a file system of its own.  At the first WaitNextEvent
// after start-up (Mac OS is then fully up) the emulator registers one and
// mounts a volume with it; from then on the File Manager hands every call
// for that volume (open, read, write, get catalog info, create, delete,
// rename, move, folders ...) to the emulator, which answers it from the
// host's folder.
//
// The Mac's side is a little 68k glue in the system heap: calls into the
// emulator are the opcodes $71xx, illegal on every 68k.  The emulator calls Mac OS routines (Memory
// Manager, File System Manager utilities) by running 68k code until it
// returns to a marker opcode.
//
// Files: the data fork is the host file.  The resource fork and the Finder
// info (type, creator, flags, icon position) go in an AppleDouble file
// "._name" beside it, as macOS keeps them on FAT disks, so a folder copied
// to a modern Mac keeps them too.  Names: Mac Roman on the Mac, Unicode on
// the card; characters the card cannot take become %XX.

#ifndef MACEMU_SHAREDFS_H
#define MACEMU_SHAREDFS_H

#include "m68k.h"
#include "hostfs.h"

namespace macemu {

class StateIO;

class SharedFolder {
public:
    SharedFolder(M68K &cpu, HostFS &fs);

    enum { kGlueSize = 0x400 };                         // the 68k glue, copied to the system heap

    void setVolumeName(const char *name);               // "Shared" by default
    void attach();                                      // take the CPU's hooks
    void reset();                                       // the Mac restarted: mount again later

    // What happened, for the menu and the log.
    bool mounted = false;
    const char *status = "waiting for Mac OS";
    u32 callsServed = 0;

    // Running Mac OS code from the host (tests; the installation).
    struct Regs { u32 d[8], a[8]; };
    bool run68k(u32 addr, Regs &r);                     // false: it never returned
    bool runTrap(u16 trap, Regs &r);
    s16 volumeRefNum() const { return vRefNum; }
    u16 driveNumber() const { return driveNum; }

    void state(StateIO &s);                             // hibernation (state.cpp keeps the IDs)

    // Called at WaitNextEvent once the volume is up (tests run Mac OS calls
    // from there: a safe moment for them).
    void (*onIdle)(void *ctx) = 0;
    void *idleCtx = 0;
    void forgetListings() { invalidate(); }

private:
    M68K &cpu;
    HostFS &fs;
    u8 glue[kGlueSize];
    u32 glueAddr = 0;                                   // where it is in the Mac
    char volName[28];
    bool tried = false;
    int depth = 0;
    bool returned[8];

    // Mac OS addresses of our structures (in the system heap).
    u32 data = 0, stack = 0;
    s16 vRefNum = 0, drvrRefNum = 0;
    u32 lastPoll = 0, volDate = 0;
    void poll();                                        // the host's changes: the volume's date moves on
    u16 driveNum = 0;
    u32 vcbAddr = 0;

    static bool hostCallHook(void *ctx, M68K &c, u16 op);
    static void trapHookFn(void *ctx, M68K &c, u16 op);
    void install();
    void buildGlue();

    // Mac memory
    u8 r8(u32 a);
    u16 r16(u32 a);
    u32 r32(u32 a);
    void w8(u32 a, u8 v);
    void w16(u32 a, u16 v);
    void w32(u32 a, u32 v);
    void macToHostMem(u32 addr, void *dst, u32 n);
    void hostToMacMem(u32 addr, const void *src, u32 n);

    // The calls
    s16 hfsProc(u32 vcb, u16 selectCode, u32 pb);
    s16 commProc(u16 message, u32 pb);
    s16 utCall(u32 glueOffset, Regs &r);

    s16 volumeMount(u32 pb);
    s16 getVolInfo(u32 pb, bool hfs);
    s16 getVolParms(u32 pb);
    s16 getCatInfo(u32 pb);
    s16 setCatInfo(u32 pb);
    s16 getFileInfo(u32 pb, bool hfs, u32 dirID);
    s16 setFileInfo(u32 pb, bool hfs, u32 dirID);
    s16 openFork(u32 pb, u32 dirID, u32 vcb, bool rsrc);
    s16 closeFork(u32 pb);
    s16 readWrite(u32 pb, bool write);
    s16 getEOF(u32 pb);
    s16 setEOF(u32 pb);
    s16 getFPos(u32 pb);
    s16 setFPos(u32 pb);
    s16 getFCBInfo(u32 pb, u32 vcb);
    s16 create(u32 pb, u32 dirID, bool dir);
    s16 removeItem(u32 pb, u32 dirID);
    s16 renameItem(u32 pb, u32 dirID);
    s16 catMove(u32 pb);
    s16 openWD(u32 pb);
    s16 closeWD(u32 pb);
    s16 getWDInfo(u32 pb, u32 vcb);
    s16 getVol(u32 pb);
    s16 setVol(u32 pb, bool hfs, u32 vcb);
    s16 makeFSSpec(u32 pb);
    s16 exchangeFiles(u32 pb);

public:
    // ---- the catalog: IDs for the host's files and folders (public for tests)
    enum { kRootID = 2, kMaxItems = 16384, kNamePool = 1 << 20, kMaxOpen = 96 };
    struct Item { u32 id, parent; u32 nameOff; u16 nameLen; bool used; u32 sig, date; };   // folders: contents, date shown
    // The item for a host name in a folder (made if new), or 0.
    u32 itemFor(u32 parent, const char *hostName);
    bool pathOf(u32 id, char *out, u32 max);            // host path ("" for the root)
    const char *hostName(u32 id);
    u32 parentOf(u32 id);
    static int macToHost(const u8 *mac, int len, char *out, int max, bool utf8);
    static int hostToMac(const char *host, u8 *mac, bool utf8);   // length (<= 31)
    static bool sameMacName(const u8 *a, int la, const u8 *b, int lb);

private:
    Item *items = 0;
    u32 itemCount = 0, nextID = 16;
    char *namePool = 0;
    u32 poolUsed = 0;
    u32 *hash = 0;                                      // (parent, name) -> item index + 1
    enum { kHashSize = 32768 };
    u32 hashOf(u32 parent, const char *name, u32 len) const;
    Item *find(u32 id);
    void forget(u32 id);
    void move(u32 id, u32 newParent, const char *newName);

    // Directory listings, cached for a moment (index calls come in order).
    struct Listing { u32 dir; u32 when; int count; u32 *ids; bool valid; u32 used; u32 sig, newest; };
    enum { kListings = 4, kListMax = 8192 };
    Listing listings[kListings];
    u32 listClock = 0;
    Listing *list(u32 dir);                             // null if it cannot be read
    void invalidate() { for (auto &l : listings) l.valid = false; }

    // Resolving names: the item a parameter block names.
    s16 currentDir(u32 pb, u32 dirID, u32 &dir, bool noVolName = false);
    s16 resolve(u32 pb, u32 dirID, u32 &parent, u8 *leaf, int &leafLen, bool noVolName = false);
    u32 findInDir(u32 dir, const u8 *macName, int len);   // the item, or 0
    s16 itemOf(u32 pb, u32 dirID, u32 &id, bool noVolName = false);   // existing item

    // Finder info and resource forks (AppleDouble "._name").
    bool sidecarPath(u32 id, char *out, u32 max);
    void getFinderInfo(u32 id, bool dir, u8 info[32]);
    bool setFinderInfo(u32 id, bool dir, const u8 info[32]);
    u32 rsrcSize(u32 id);
    struct Double { u32 finfoOff, finfoLen, rsrcOff, rsrcLen; bool ours; };
    bool readDouble(int f, Double &d);
    int openDouble(u32 id, bool write, Double &d);      // our layout when writing

    struct OpenFork { bool used; u32 id; bool rsrc, write; int f; u32 base, len; u32 mark; u16 refNum; };
    OpenFork open[kMaxOpen];
    OpenFork *forkFor(u32 fcb);
    void updateDoubleLength(OpenFork &o);
    void closeAll();

    char path[1024], path2[1024];
};

} // namespace macemu

#endif
