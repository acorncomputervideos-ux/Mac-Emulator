// The host's files, as the shared folder sees them: a folder on the SD card
// (FatFs) on the Pi, a directory on the host runner.  Paths are relative to
// the shared folder, with '/' between names ("" is the folder itself).

#ifndef MACEMU_HOSTFS_H
#define MACEMU_HOSTFS_H

#include "types.h"

namespace macemu {

struct HostStat {
    bool dir;
    u32 size;                           // files (to 4 GB)
    u32 mtime;                          // Unix time
    bool readOnly;
};

class HostFS {
public:
    virtual ~HostFS() {}

    virtual bool stat(const char *path, HostStat &st) = 0;

    // A directory's names, one by one (all of them: the caller filters).
    virtual int openDir(const char *path) = 0;          // a handle, or -1
    virtual bool readDir(int dir, char *name, u32 max, u32 *mtime = 0) = 0;   // false at the end
    virtual void closeDir(int dir) = 0;

    enum { ReadOnly = 0, ReadWrite = 1, Create = 2 };   // Create: make it (empty) if absent
    virtual int open(const char *path, int mode) = 0;   // a handle, or -1
    virtual s32 read(int f, u32 offset, void *buf, u32 len) = 0;     // bytes read, -1 on error
    virtual s32 write(int f, u32 offset, const void *buf, u32 len) = 0;
    virtual bool truncate(int f, u32 size) = 0;
    virtual u32 size(int f) = 0;
    virtual void close(int f) = 0;

    virtual bool makeDir(const char *path) = 0;
    virtual bool remove(const char *path) = 0;          // a file, or an empty directory
    virtual bool rename(const char *from, const char *to) = 0;

    virtual u64 freeBytes() = 0;
    virtual u32 milliseconds() = 0;                     // for caching directory listings
    virtual bool namesUtf8() const = 0;                 // else code page 850 (FatFs on the Pi)
};

} // namespace macemu

#endif
