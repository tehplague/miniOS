/* procfs.c — Minimal read-only virtual filesystem for /proc.
 *
 * Hardcoded inode constants — no heap allocation, no shared state with sysfs.
 *
 * procfs_read generates content into a stack buffer on every call and slices
 * [off, off+len) from it, so fgets-style multi-read access works correctly
 * (unlike sysfs callbacks which ignore the offset argument).
 *
 * Currently serves:
 *   /proc/meminfo          — RAM totals from PMM, zeros for swap/cache/buffers
 *   /proc/mounts           — mount table
 *   /proc/stat             — single aggregate "cpu" jiffy line (BusyBox top/ps)
 *   /proc/<pid>/status     — name, state, pid/ppid/tid
 *   /proc/<pid>/stat       — space-separated fields (partial Linux layout, enough
 *                            for BusyBox ps/top: pid, comm, state, ppid, pgrp,
 *                            session, utime, stime)
 *   /proc/<pid>/cmdline    — NUL-separated argv as execve() received it
 *   /proc/<pid>/cwd        — current working directory (as plain text; miniOS
 *                            has no symlinks, so this isn't a real symlink target)
 *   /proc/<pid>/maps       — heap + mmap region ranges
 *
 * PID enumeration walks thread_pool[] directly (up to SCHED_MAX_THREADS slots,
 * small enough that no caching is needed — regenerated on every lookup/readdir).
 */

#include <miniOS/fs/procfs.h>
#include <miniOS/fs/vfs.h>
#include <miniOS/mm/pmm.h>
#include <miniOS/mm/vmm.h>
#include <miniOS/sched/sched.h>
#include <miniOS/io.h>
#include <string.h>

#define PROCFS_INO_ROOT    1
#define PROCFS_INO_MEMINFO 2
#define PROCFS_INO_MOUNTS  3
#define PROCFS_INO_STAT    4

/* PID-keyed inode range: ino = PROCFS_PID_BASE + (pid << PROCFS_PID_SHIFT) + subtype.
 * 8 subtype slots per pid (only 6 used) keeps decode a shift+mask, no division. */
#define PROCFS_PID_BASE    0x1000u
#define PROCFS_PID_SHIFT   3u
#define PROCFS_SUB_MASK    ((1u << PROCFS_PID_SHIFT) - 1u)

#define PROCFS_SUB_DIR      0u
#define PROCFS_SUB_STATUS   1u
#define PROCFS_SUB_STAT     2u
#define PROCFS_SUB_CMDLINE  3u
#define PROCFS_SUB_CWD      4u
#define PROCFS_SUB_MAPS     5u

static uint32_t procfs_pid_ino(uint32_t pid, uint32_t sub)
{
    return PROCFS_PID_BASE + (pid << PROCFS_PID_SHIFT) + sub;
}

static int procfs_ino_is_pid(uint32_t ino)
{
    return ino >= PROCFS_PID_BASE;
}

static uint32_t procfs_ino_pid(uint32_t ino)
{
    return (ino - PROCFS_PID_BASE) >> PROCFS_PID_SHIFT;
}

static uint32_t procfs_ino_sub(uint32_t ino)
{
    return (ino - PROCFS_PID_BASE) & PROCFS_SUB_MASK;
}

/* pid 0 is never a real process (idle threads use tid but not a POSIX pid
 * exposed here); a zombie thread keeps its pid until reaped by waitpid, so
 * this also surfaces zombies, matching Linux /proc behaviour. */
static struct thread *procfs_find_thread(uint32_t pid)
{
    if (pid == 0)
        return NULL;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        if (thread_pool[i].pid == pid)
            return &thread_pool[i];
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Content generators
 * --------------------------------------------------------------------- */

static int procfs_generate_mounts(char *buf, uint32_t bufsiz)
{
    int pos = 0;
    int n = vfs_mount_count();
    for (int i = 0; i < n; i++) {
        const vfs_mount_t *m = vfs_get_mount(i);
        if (!m) break;
        const char *dev = (m->device[0]) ? m->device : m->fstype;
        const char *fst = (m->fstype[0]) ? m->fstype : "none";
        /* kernel snprintf returns count including the null terminator;
         * subtract 1 so next entry overwrites the null rather than skipping it */
        int r = snprintf(buf + pos, bufsiz - (uint32_t)pos,
                         "%s %s %s rw 0 0\n", dev, m->mount_point, fst);
        if (r <= 1 || (uint32_t)(pos + r - 1) >= bufsiz - 1) break;
        pos += r - 1;
    }
    return pos;
}

static int procfs_generate_meminfo(char *buf, uint32_t bufsiz)
{
    uint64_t total_kb = pmm_total_count() * (PAGE_SIZE / 1024);
    uint64_t free_kb  = pmm_free_count()  * (PAGE_SIZE / 1024);

    int r = snprintf(buf, bufsiz,
        "MemTotal:        %8llu kB\n"
        "MemFree:         %8llu kB\n"
        "MemAvailable:    %8llu kB\n"
        "Buffers:                0 kB\n"
        "Cached:                 0 kB\n"
        "SwapTotal:              0 kB\n"
        "SwapFree:               0 kB\n"
        "SReclaimable:           0 kB\n",
        (unsigned long long)total_kb,
        (unsigned long long)free_kb,
        (unsigned long long)free_kb);
    /* kernel snprintf counts include the null terminator; return without it */
    return (r > 0) ? r - 1 : 0;
}

static int procfs_generate_stat_global(char *buf, uint32_t bufsiz)
{
    /* BusyBox top/ps's read_cpu_jiffy() require a "cpu ..." line with 8
     * jiffy-like fields (user, nice, system, idle, iowait, irq, softirq,
     * steal) to compute CPU%; miniOS doesn't track kernel/idle time
     * separately, so this charges every tick to "user" — enough for top to
     * run without crashing, not enough for an accurate percentage. */
    int r = snprintf(buf, bufsiz, "cpu  %llu 0 0 0 0 0 0 0\n",
                      (unsigned long long)sched_tick_count);
    return (r > 0) ? r - 1 : 0;
}

static char procfs_state_char(thread_state_t s)
{
    switch (s) {
    case THREAD_RUNNABLE:   return 'R';
    case THREAD_RUNNING:    return 'R';
    case THREAD_DEAD:       return 'Z';
    case THREAD_WAITING:    return 'S';
    case THREAD_BLOCKED:    return 'D';
    case THREAD_FUTEX_WAIT: return 'S';
    case THREAD_STOPPED:    return 'T';
    default:                return '?';
    }
}

static const char *procfs_state_str(thread_state_t s)
{
    switch (s) {
    case THREAD_RUNNABLE:   return "R (runnable)";
    case THREAD_RUNNING:    return "R (running)";
    case THREAD_DEAD:       return "Z (zombie)";
    case THREAD_WAITING:    return "S (waiting)";
    case THREAD_BLOCKED:    return "D (blocked)";
    case THREAD_FUTEX_WAIT: return "S (futex)";
    case THREAD_STOPPED:    return "T (stopped)";
    default:                return "? (unknown)";
    }
}

static int procfs_generate_status(struct thread *t, char *buf, uint32_t bufsiz)
{
    int r = snprintf(buf, bufsiz,
        "Name:\t%s\n"
        "State:\t%s\n"
        "Tid:\t%u\n"
        "Pid:\t%u\n"
        "PPid:\t%u\n"
        "Pgid:\t%u\n"
        "Sid:\t%u\n",
        t->comm[0] ? t->comm : "?",
        procfs_state_str(t->state),
        t->tid, t->pid, t->ppid, t->pgid, t->sid);
    return (r > 0) ? r - 1 : 0;
}

static int procfs_generate_stat(struct thread *t, char *buf, uint32_t bufsiz)
{
    /* Partial /proc/<pid>/stat: pid, comm, state, ppid, pgrp, session, then
     * placeholder tty/tpgid/flags/minflt/cminflt/majflt/cmajflt fields (miniOS
     * doesn't track them), then utime/stime (real, from sched_tick's per-thread
     * counter — no kernel/user split, so stime is always 0), then remaining
     * fields BusyBox ps/top may skip past (cutime/cstime/priority/nice/
     * num_threads/itrealvalue/starttime/vsize/rss) as zero. */
    int r = snprintf(buf, bufsiz,
        "%u (%s) %c %u %u %u 0 0 0 0 0 0 0 %llu 0 0 0 0 0 0 0 0 0 0\n",
        t->pid, t->comm[0] ? t->comm : "?", procfs_state_char(t->state),
        t->ppid, t->pgid, t->sid,
        (unsigned long long)t->utime_ticks);
    return (r > 0) ? r - 1 : 0;
}

static int procfs_generate_cmdline(struct thread *t, char *buf, uint32_t bufsiz)
{
    uint32_t n = t->cmdline_len;
    if (n > bufsiz) n = bufsiz;
    memcpy(buf, t->cmdline, n);
    return (int)n;
}

static int procfs_generate_cwd(struct thread *t, char *buf, uint32_t bufsiz)
{
    int r = snprintf(buf, bufsiz, "%s\n", THREAD_CWD(t));
    return (r > 0) ? r - 1 : 0;
}

static int procfs_generate_maps(struct thread *t, char *buf, uint32_t bufsiz)
{
    int pos = 0;
    uint64_t brk_base = *THREAD_BRK_BASE_PTR(t);
    uint64_t brk      = *THREAD_BRK_PTR(t);

    if (brk > brk_base) {
        int r = snprintf(buf + pos, bufsiz - (uint32_t)pos,
            "%016llx-%016llx rw-p 00000000 00:00 0 [heap]\n",
            (unsigned long long)brk_base, (unsigned long long)brk);
        if (r > 1) pos += r - 1;
    }

    mmap_region_t *regions = THREAD_MMAPR(t);
    if (regions) {
        for (int i = 0; i < MMAP_MAX_REGIONS; i++) {
            if (regions[i].virt_addr == 0)
                continue;
            const char *perm = (regions[i].prot & 0x2) ? "rw-p" : "r--p";
            int r = snprintf(buf + pos, bufsiz - (uint32_t)pos,
                "%016llx-%016llx %s 00000000 00:00 0\n",
                (unsigned long long)regions[i].virt_addr,
                (unsigned long long)(regions[i].virt_addr + regions[i].len),
                perm);
            if (r <= 1 || (uint32_t)(pos + r - 1) >= bufsiz - 1) break;
            pos += r - 1;
        }
    }
    return pos;
}

/* -----------------------------------------------------------------------
 * VFS ops
 * --------------------------------------------------------------------- */

static int procfs_lookup(const char *path, vfs_inode_info_t *out)
{
    while (*path == '/') path++;

    if (*path == '\0') {
        out->inode     = PROCFS_INO_ROOT;
        out->file_type = VFS_FILE_TYPE_DIR;
        out->size      = 0;
        return 0;
    }
    if (strncmp(path, "meminfo", 8) == 0) {
        char tmp[2048];
        out->inode     = PROCFS_INO_MEMINFO;
        out->file_type = VFS_FILE_TYPE_REG;
        out->size      = (uint64_t)procfs_generate_meminfo(tmp, sizeof(tmp));
        return 0;
    }
    if (strncmp(path, "mounts", 7) == 0) {
        char tmp[2048];
        out->inode     = PROCFS_INO_MOUNTS;
        out->file_type = VFS_FILE_TYPE_REG;
        out->size      = (uint64_t)procfs_generate_mounts(tmp, sizeof(tmp));
        return 0;
    }
    if (strncmp(path, "stat", 5) == 0) {
        char tmp[2048];
        out->inode     = PROCFS_INO_STAT;
        out->file_type = VFS_FILE_TYPE_REG;
        out->size      = (uint64_t)procfs_generate_stat_global(tmp, sizeof(tmp));
        return 0;
    }

    if (path[0] >= '0' && path[0] <= '9') {
        const char *p = path;
        uint32_t pid = 0;
        while (*p >= '0' && *p <= '9') {
            pid = pid * 10 + (uint32_t)(*p - '0');
            p++;
        }
        if (!procfs_find_thread(pid))
            return -1; /* ENOENT */

        if (*p != '\0' && *p != '/')
            return -1; /* e.g. "123abc" is not a valid pid component */
        if (*p == '/')
            p++;
        if (*p == '\0') {
            /* Bare pid directory, with or without a trailing slash (BusyBox's
             * ps calls stat("/proc/<pid>/", ...) with the slash included). */
            out->inode     = procfs_pid_ino(pid, PROCFS_SUB_DIR);
            out->file_type = VFS_FILE_TYPE_DIR;
            out->size      = 0;
            return 0;
        }

        struct thread *t = procfs_find_thread(pid);
        static const struct { const char *name; uint32_t sub; } subs[] = {
            { "status",  PROCFS_SUB_STATUS  },
            { "stat",    PROCFS_SUB_STAT    },
            { "cmdline", PROCFS_SUB_CMDLINE },
            { "cwd",     PROCFS_SUB_CWD     },
            { "maps",    PROCFS_SUB_MAPS    },
        };
        for (unsigned i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
            size_t sublen = strlen(subs[i].name);
            if (strncmp(p, subs[i].name, sublen + 1) == 0) {
                char tmp[2048];
                int len = 0;
                switch (subs[i].sub) {
                case PROCFS_SUB_STATUS:  len = procfs_generate_status(t, tmp, sizeof(tmp)); break;
                case PROCFS_SUB_STAT:    len = procfs_generate_stat(t, tmp, sizeof(tmp)); break;
                case PROCFS_SUB_CMDLINE: len = procfs_generate_cmdline(t, tmp, sizeof(tmp)); break;
                case PROCFS_SUB_CWD:     len = procfs_generate_cwd(t, tmp, sizeof(tmp)); break;
                case PROCFS_SUB_MAPS:    len = procfs_generate_maps(t, tmp, sizeof(tmp)); break;
                }
                out->inode     = procfs_pid_ino(pid, subs[i].sub);
                out->file_type = VFS_FILE_TYPE_REG;
                out->size      = (uint64_t)len;
                return 0;
            }
        }
        return -1;
    }

    return -1; /* ENOENT */
}

static int procfs_read(uint32_t ino, uint64_t off, void *buf, uint32_t len)
{
    char tmp[2048];
    int total;

    if (ino == PROCFS_INO_MEMINFO) {
        total = procfs_generate_meminfo(tmp, sizeof(tmp));
    } else if (ino == PROCFS_INO_MOUNTS) {
        total = procfs_generate_mounts(tmp, sizeof(tmp));
    } else if (ino == PROCFS_INO_STAT) {
        total = procfs_generate_stat_global(tmp, sizeof(tmp));
    } else if (procfs_ino_is_pid(ino)) {
        uint32_t pid = procfs_ino_pid(ino);
        uint32_t sub = procfs_ino_sub(ino);
        struct thread *t = procfs_find_thread(pid);
        if (!t)
            return -1; /* process exited/reaped since lookup */
        switch (sub) {
        case PROCFS_SUB_STATUS:  total = procfs_generate_status(t, tmp, sizeof(tmp)); break;
        case PROCFS_SUB_STAT:    total = procfs_generate_stat(t, tmp, sizeof(tmp)); break;
        case PROCFS_SUB_CMDLINE: total = procfs_generate_cmdline(t, tmp, sizeof(tmp)); break;
        case PROCFS_SUB_CWD:     total = procfs_generate_cwd(t, tmp, sizeof(tmp)); break;
        case PROCFS_SUB_MAPS:    total = procfs_generate_maps(t, tmp, sizeof(tmp)); break;
        default: return -1;
        }
    } else {
        return -1;
    }

    if (total <= 0)
        return 0;
    if (off >= (uint64_t)total)
        return 0; /* EOF */

    uint32_t avail = (uint32_t)((uint64_t)total - off);
    if (len > avail)
        len = avail;
    memcpy(buf, tmp + (uint32_t)off, len);
    return (int)len;
}

static int procfs_readdir(uint32_t ino, uint64_t *offset,
                          vfs_dirent_cb_t cb, void *ud)
{
    if (ino == PROCFS_INO_ROOT) {
        if (*offset == 0) {
            int r = cb("meminfo", 7, PROCFS_INO_MEMINFO, VFS_FILE_TYPE_REG, ud);
            (*offset)++;
            if (r) return r;
        }
        if (*offset == 1) {
            int r = cb("mounts", 6, PROCFS_INO_MOUNTS, VFS_FILE_TYPE_REG, ud);
            (*offset)++;
            if (r) return r;
        }
        if (*offset == 2) {
            int r = cb("stat", 4, PROCFS_INO_STAT, VFS_FILE_TYPE_REG, ud);
            (*offset)++;
            if (r) return r;
        }
        /* PID directories start at offset 3; *offset - 3 is a thread_pool index
         * cursor so repeated calls resume where they left off. */
        uint64_t idx = *offset - 3;
        for (; idx < SCHED_MAX_THREADS; idx++) {
            struct thread *t = &thread_pool[idx];
            if (t->pid == 0)
                continue;
            char namebuf[16];
            int nlen = snprintf(namebuf, sizeof(namebuf), "%u", t->pid);
            nlen = (nlen > 0) ? nlen - 1 : 0;
            int r = cb(namebuf, nlen, procfs_pid_ino(t->pid, PROCFS_SUB_DIR),
                       VFS_FILE_TYPE_DIR, ud);
            *offset = idx + 1 + 3;
            if (r) return r;
        }
        *offset = SCHED_MAX_THREADS + 3;
        return 0;
    }

    if (procfs_ino_is_pid(ino) && procfs_ino_sub(ino) == PROCFS_SUB_DIR) {
        uint32_t pid = procfs_ino_pid(ino);
        static const struct { const char *name; uint32_t sub; } subs[] = {
            { "status",  PROCFS_SUB_STATUS  },
            { "stat",    PROCFS_SUB_STAT    },
            { "cmdline", PROCFS_SUB_CMDLINE },
            { "cwd",     PROCFS_SUB_CWD     },
            { "maps",    PROCFS_SUB_MAPS    },
        };
        for (uint64_t idx = *offset; idx < sizeof(subs) / sizeof(subs[0]); idx++) {
            int r = cb(subs[idx].name, (int)strlen(subs[idx].name),
                       procfs_pid_ino(pid, subs[idx].sub), VFS_FILE_TYPE_REG, ud);
            *offset = idx + 1;
            if (r) return r;
        }
        return 0;
    }

    return -1;
}

static int procfs_statfs(vfs_statfs_t *out)
{
    memset(out, 0, sizeof(*out));
    out->f_type    = 0x9fa0; /* PROC_SUPER_MAGIC */
    out->f_bsize   = 4096;
    out->f_namelen = 255;
    return 0;
}

static int procfs_unlink(uint32_t parent_ino, const char *name)
{
    (void)parent_ino; (void)name;
    return -30; /* EROFS */
}

static int procfs_rmdir(uint32_t parent_ino, const char *name)
{
    (void)parent_ino; (void)name;
    return -30; /* EROFS */
}

/* -----------------------------------------------------------------------
 * Mount callback and init
 * --------------------------------------------------------------------- */

static vfs_ops_t procfs_ops;

static int procfs_mount(const char *source, const char *target, const void *data)
{
    (void)source; (void)data;
    return vfs_register_mount(target, &procfs_ops, PROCFS_INO_ROOT);
}

void procfs_init(void)
{
    memset(&procfs_ops, 0, sizeof(procfs_ops));
    procfs_ops.lookup  = procfs_lookup;
    procfs_ops.read    = procfs_read;
    procfs_ops.readdir = procfs_readdir;
    procfs_ops.unlink  = procfs_unlink;
    procfs_ops.rmdir   = procfs_rmdir;
    procfs_ops.statfs  = procfs_statfs;

    register_filesystem("procfs", procfs_mount);
    printk("procfs: filesystem type registered\n");
}
