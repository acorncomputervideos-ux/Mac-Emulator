// HostFS on a POSIX directory (the host runner, tests).
#ifndef MACEMU_HOSTFS_POSIX_H
#define MACEMU_HOSTFS_POSIX_H

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include "../core/hostfs.h"

namespace macemu {

class PosixHostFS : public HostFS {
public:
    explicit PosixHostFS(const std::string &root) : root(root) {}

    bool stat(const char *path, HostStat &st) override {
        struct stat s;
        if (::stat(full(path).c_str(), &s)) return false;
        st.dir = S_ISDIR(s.st_mode);
        st.size = (u32)s.st_size;
        st.mtime = (u32)s.st_mtime;
        st.readOnly = access(full(path).c_str(), W_OK) != 0;
        return true;
    }
    int openDir(const char *path) override {
        DIR *d = opendir(full(path).c_str());
        if (!d) return -1;
        dirs.push_back(d);
        return (int)dirs.size() - 1;
    }
    bool readDir(int h, char *name, u32 max, u32 *mtime) override {
        for (;;) {
            struct dirent *e = readdir(dirs[h]);
            if (!e) return false;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(name, max, "%s", e->d_name);
            if (mtime) { struct stat s; *mtime = fstatat(dirfd(dirs[h]), e->d_name, &s, 0) ? 0 : (u32)s.st_mtime; }
            return true;
        }
    }
    void closeDir(int h) override { closedir(dirs[h]); dirs[h] = 0; }
    int open(const char *path, int mode) override {
        int flags = mode == ReadOnly ? O_RDONLY : O_RDWR;
        if (mode == Create) flags |= O_CREAT;
        int fd = ::open(full(path).c_str(), flags, 0644);
        return fd;
    }
    s32 read(int f, u32 off, void *buf, u32 len) override { return (s32)pread(f, buf, len, off); }
    s32 write(int f, u32 off, const void *buf, u32 len) override { return (s32)pwrite(f, buf, len, off); }
    bool truncate(int f, u32 size) override { return ftruncate(f, size) == 0; }
    u32 size(int f) override { struct stat s; return fstat(f, &s) ? 0 : (u32)s.st_size; }
    void close(int f) override { ::close(f); }
    bool makeDir(const char *path) override { return mkdir(full(path).c_str(), 0755) == 0; }
    bool remove(const char *path) override { return ::remove(full(path).c_str()) == 0; }
    bool rename(const char *from, const char *to) override { return ::rename(full(from).c_str(), full(to).c_str()) == 0; }
    u64 freeBytes() override { struct statvfs v; return statvfs(root.c_str(), &v) ? 0 : (u64)v.f_bavail * v.f_frsize; }
    u32 milliseconds() override { struct timeval t; gettimeofday(&t, 0); return (u32)(t.tv_sec * 1000 + t.tv_usec / 1000); }
    bool namesUtf8() const override { return true; }

private:
    std::string root;
    std::vector<DIR *> dirs;
    std::string full(const char *path) const { return *path ? root + "/" + path : root; }
};

} // namespace macemu

#endif
