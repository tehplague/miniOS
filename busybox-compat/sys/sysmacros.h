/* sys/sysmacros.h — major/minor device number macros for miniOS/mlibc cross-build. */
#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#include <sys/types.h>

#define major(dev) \
    ((unsigned int)(((dev) >> 8) & 0xfff) | (((dev) >> 32) & ~0xfff))
#define minor(dev) \
    ((unsigned int)((dev) & 0xff) | (((dev) >> 12) & ~0xff))
#define makedev(maj, min) \
    ((((unsigned long long)(maj) & 0xfff) << 8) | \
     (((unsigned long long)(maj) & ~0xfff) << 32) | \
     (((unsigned long long)(min) & 0xff)) | \
     (((unsigned long long)(min) & ~0xff) << 12))

#endif /* _SYS_SYSMACROS_H */
