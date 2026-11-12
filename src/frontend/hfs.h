// HFS (Mac OS Standard) volumes: reading any disk image (browse folders,
// read either fork of a file), and building a fresh 800K floppy holding a
// list of files.  Also MacBinary II, the usual way to carry a Mac file
// (both forks, type and creator) through other computers.
//
// Freestanding: no C library, so it runs in the Pi kernel and on the host.

#ifndef MACEMU_HFS_H
#define MACEMU_HFS_H

#include "../core/types.h"

namespace macemu {

struct HfsEntry {
    u32 id;                     // CNID
    u32 parent;
    bool folder;
    char name[32];              // Mac Roman, NUL-terminated
    u32 type, creator;          // files
    u32 dataLen, rsrcLen;       // files
    u32 items;                  // folders: valence
    u32 created, modified;      // Mac time
    u16 finderFlags;
    u16 dataExt[6], rsrcExt[6]; // files: first three extents (start, count)
};

class HfsVolume {
public:
    // Finds the HFS volume in a disk image: a partition map (hard disks,
    // CDs), a bare volume (floppies) or a DiskCopy 4.2 image.
    bool open(Storage *image);
    const char *name() const { return volName; }
    u32 freeBytes() const { return freeBlocks * blockSize; }

    // Entries whose parent is `folder` (2 = the root); returns how many.
    int list(u32 folder, HfsEntry *out, int max);
    bool find(u32 id, HfsEntry &out);       // a file or folder by CNID
    // Read `len` bytes of a fork at `offset`.
    bool readFork(const HfsEntry &file, bool rsrc, u32 offset, void *buf, u32 len);

private:
    Storage *img = 0;
    u32 base = 0;               // byte offset of the volume in the image
    u32 blockSize = 0, firstBlock = 0, freeBlocks = 0;
    u16 catExt[6] = {}, extExt[6] = {};
    u32 catSize = 0, extSize = 0;
    char volName[28] = {};
    u16 nodeSize = 512;
    u32 catFirstLeaf = 0, extFirstLeaf = 0;

    bool readAt(u32 off, void *buf, u32 len) { return img->read(base + off, buf, len); }
    bool readNode(const u16 *ext, u32 fileSize, u32 node, u8 *out);
    bool decode(const u8 *rec, u32 len, HfsEntry &e, u16 *dataExt, u16 *rsrcExt);
    bool forkExtents(u32 id, bool rsrc, u32 startBlock, u16 *ext);
};

// One file for a new floppy.  Names are cleaned up to letters, digits,
// space, '.' and '-' (whose HFS sort order is plain ASCII, case-insensitive).
struct HfsNewFile {
    char name[32];
    u32 type, creator;
    const u8 *data; u32 dataLen;
    const u8 *rsrc; u32 rsrcLen;
};

// Build an 800K (819200-byte) HFS floppy image in `out`.  Returns false if
// the files do not fit.  `macNow` is the creation time (Mac epoch).
bool hfsMakeFloppy(u8 *out, const char *volumeName, const HfsNewFile *files, int count, u32 macNow);
enum { kHfsFloppyBytes = 819200, kHfsFloppyMaxFiles = 64 };

// MacBinary II.
bool macBinaryParse(const u8 *file, u32 len, HfsNewFile &out);     // false: not MacBinary
void macBinaryHeader(const HfsEntry &e, u8 *hdr128);
inline u32 macBinaryPad(u32 n) { return (n + 127) & ~127u; }

// HFS name order (for names cleaned up as above).
int hfsCompareNames(const char *a, const char *b);
void hfsCleanName(char *name);

} // namespace macemu

#endif
