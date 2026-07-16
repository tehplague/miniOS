/* sys/statfs.h — filesystem statistics for miniOS/mlibc cross-build. */
#ifndef _SYS_STATFS_H
#define _SYS_STATFS_H

#include <sys/types.h>

typedef long fsword_t;

struct statfs {
    fsword_t  f_type;
    fsword_t  f_bsize;
    long long f_blocks;
    long long f_bfree;
    long long f_bavail;
    long long f_files;
    long long f_ffree;
    struct { int __val[2]; } f_fsid;
    fsword_t  f_namelen;
    fsword_t  f_frsize;
    fsword_t  f_flags;
    fsword_t  f_spare[4];
};

struct statfs64 {
    fsword_t           f_type;
    fsword_t           f_bsize;
    unsigned long long f_blocks;
    unsigned long long f_bfree;
    unsigned long long f_bavail;
    unsigned long long f_files;
    unsigned long long f_ffree;
    struct { int __val[2]; } f_fsid;
    fsword_t           f_namelen;
    fsword_t           f_frsize;
    fsword_t           f_flags;
    fsword_t           f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif /* _SYS_STATFS_H */
