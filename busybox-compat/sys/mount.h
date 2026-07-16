/* sys/mount.h — mlibc doesn't install this header. mount()/umount() are
 * implemented as miniOS-specific syscall wrappers in user/libc/miniOS_compat.c;
 * this just supplies the declarations and the standard Linux MS_ and MNT_
 * flag values BusyBox's mount/umount applets reference (many are already
 * #ifndef-guarded in mount.c for flags the running kernel doesn't enforce —
 * only the base set BusyBox assumes always exists needs to be defined here). */
#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#ifdef __cplusplus
extern "C" {
#endif

#define MS_RDONLY      1
#define MS_NOSUID      2
#define MS_NODEV       4
#define MS_NOEXEC      8
#define MS_SYNCHRONOUS 16
#define MS_REMOUNT     32
#define MS_MANDLOCK    64
#define MS_NOATIME     1024
#define MS_NODIRATIME  2048
#define MS_MGC_VAL     0xC0ED0000

#define MNT_FORCE  1
#define MNT_DETACH 2
#define MNT_EXPIRE 4
#define UMOUNT_NOFOLLOW 8

int mount(const char *source, const char *target, const char *fstype,
          unsigned long flags, const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_MOUNT_H */
