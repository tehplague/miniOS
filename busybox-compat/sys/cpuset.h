/* sys/cpuset.h — CPU affinity types for miniOS/mlibc cross-build.
 * mlibc's bits/cpu_set.h provides cpu_set_t but not the CPU_ZERO/CPU_SET/
 * CPU_ISSET/CPU_COUNT convenience macros; this header adds them.
 * miniOS does not implement affinity; the syscall stub returns ENOSYS.
 */
#ifndef _SYS_CPUSET_H_
#define _SYS_CPUSET_H_

#include <stddef.h>

#define CPU_SETSIZE 1024

typedef struct {
    unsigned long __bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpu_set_t;

#define CPU_ZERO(s) \
    do { size_t __i; for (__i = 0; __i < sizeof(cpu_set_t) / sizeof(unsigned long); __i++) \
        (s)->__bits[__i] = 0UL; } while (0)

#define CPU_SET(cpu, s) \
    ((s)->__bits[(cpu) / (8 * sizeof(unsigned long))] |= \
     1UL << ((cpu) % (8 * sizeof(unsigned long))))

#define CPU_CLR(cpu, s) \
    ((s)->__bits[(cpu) / (8 * sizeof(unsigned long))] &= \
     ~(1UL << ((cpu) % (8 * sizeof(unsigned long)))))

#define CPU_ISSET(cpu, s) \
    (!!((s)->__bits[(cpu) / (8 * sizeof(unsigned long))] & \
        (1UL << ((cpu) % (8 * sizeof(unsigned long))))))

#define CPU_COUNT(s) __cpu_count(s)
static inline int __cpu_count(const cpu_set_t *s) {
    int n = 0;
    size_t i;
    for (i = 0; i < sizeof(cpu_set_t) / sizeof(unsigned long); i++)
        n += __builtin_popcountl(s->__bits[i]);
    return n;
}

#endif /* _SYS_CPUSET_H_ */
