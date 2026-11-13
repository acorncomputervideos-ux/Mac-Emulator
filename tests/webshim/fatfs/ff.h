#ifndef SHIM_FF_H
#define SHIM_FF_H
#include <stdio.h>
#include <dirent.h>
typedef unsigned UINT; typedef unsigned DWORD;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_NO_FILE, FR_EXIST } FRESULT;
typedef struct { FILE *f; unsigned long long size; } FIL;
typedef struct { DIR *d; char path[300]; } DIR_;
#define DIR DIR_
typedef struct { char fname[256]; unsigned long long fsize; unsigned char fattrib; } FILINFO;
typedef struct { unsigned csize; } FATFS;
#define AM_DIR 0x10
#define AM_HID 0x02
#define AM_SYS 0x04
#define FA_READ 1
#define FA_WRITE 2
#define FA_CREATE_NEW 4
#define FA_CREATE_ALWAYS 8
#define f_size(fp) ((fp)->size)
FRESULT f_open(FIL *fp, const char *path, int mode);
FRESULT f_read(FIL *fp, void *buf, UINT n, UINT *got);
FRESULT f_write(FIL *fp, const void *buf, UINT n, UINT *done);
FRESULT f_close(FIL *fp);
FRESULT f_unlink(const char *path);
FRESULT f_rename(const char *from, const char *to);
FRESULT f_findfirst(DIR *dp, FILINFO *fi, const char *path, const char *pattern);
FRESULT f_findnext(DIR *dp, FILINFO *fi);
FRESULT f_closedir(DIR *dp);
FRESULT f_mkdir(const char *path);
FRESULT f_lseek(FIL *fp, unsigned long long ofs);
FRESULT f_stat(const char *path, FILINFO *fi);
FRESULT f_getfree(const char *path, DWORD *clusters, FATFS **fs);
#endif
