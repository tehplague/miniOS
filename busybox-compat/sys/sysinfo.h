#ifndef _SYS_SYSINFO_H
#define _SYS_SYSINFO_H

#ifdef __cplusplus
extern "C" {
#endif

struct sysinfo {
    long           uptime;      /* seconds since boot */
    unsigned long  loads[3];    /* 1/5/15-min load averages (scaled by 2^16) */
    unsigned long  totalram;    /* total usable RAM in mem_unit bytes */
    unsigned long  freeram;     /* available RAM */
    unsigned long  sharedram;   /* shared memory */
    unsigned long  bufferram;   /* buffer memory */
    unsigned long  totalswap;   /* total swap space */
    unsigned long  freeswap;    /* available swap */
    unsigned short procs;       /* current process count */
    char           _pad[6];     /* alignment to next unsigned long */
    unsigned long  totalhigh;   /* total high memory */
    unsigned long  freehigh;    /* available high memory */
    unsigned int   mem_unit;    /* unit for all sizes (bytes) */
    char           _f[0];
};

int sysinfo(struct sysinfo *info);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_SYSINFO_H */
