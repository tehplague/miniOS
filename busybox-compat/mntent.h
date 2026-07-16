/* mntent.h — mount entry types for miniOS/mlibc cross-build.
 * Provides the subset used by busybox mount/umount applets.
 */
#ifndef _MNTENT_H
#define _MNTENT_H

#include <stdio.h>

#define MOUNTED     "/etc/mtab"
#define MNTTAB      "/etc/fstab"

#define MNTTYPE_IGNORE  "ignore"
#define MNTTYPE_NFS     "nfs"
#define MNTTYPE_SWAP    "swap"

#define MNTOPT_DEFAULTS "defaults"
#define MNTOPT_RO       "ro"
#define MNTOPT_RW       "rw"
#define MNTOPT_SUID     "suid"
#define MNTOPT_NOSUID   "nosuid"
#define MNTOPT_NOAUTO   "noauto"

struct mntent {
    char *mnt_fsname;   /* device or server */
    char *mnt_dir;      /* mount point */
    char *mnt_type;     /* filesystem type */
    char *mnt_opts;     /* mount options */
    int   mnt_freq;     /* dump frequency (days) */
    int   mnt_passno;   /* fsck pass number */
};

FILE          *setmntent(const char *filename, const char *type);
struct mntent *getmntent(FILE *stream);
struct mntent *getmntent_r(FILE *stream, struct mntent *result,
                            char *buf, int buflen);
int            addmntent(FILE *stream, const struct mntent *mnt);
int            endmntent(FILE *stream);
char          *hasmntopt(const struct mntent *mnt, const char *opt);

#endif /* _MNTENT_H */
