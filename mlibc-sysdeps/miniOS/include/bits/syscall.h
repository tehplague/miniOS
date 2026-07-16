#ifndef _MLIBC_BITS_SYSCALL_H
#define _MLIBC_BITS_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

#define __scc(x) ((long)(x))

static __inline__ long __do_syscall0(long n) {
	long ret;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
	return ret;
}
static __inline__ long __do_syscall1(long n, long a1) {
	long ret;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
	return ret;
}
static __inline__ long __do_syscall2(long n, long a1, long a2) {
	long ret;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
	return ret;
}
static __inline__ long __do_syscall3(long n, long a1, long a2, long a3) {
	long ret;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3)
		: "rcx", "r11", "memory");
	return ret;
}
static __inline__ long __do_syscall4(long n, long a1, long a2, long a3, long a4) {
	long ret;
	register long _r10 __asm__("r10") = a4;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(_r10)
		: "rcx", "r11", "memory");
	return ret;
}
static __inline__ long __do_syscall5(long n, long a1, long a2, long a3, long a4, long a5) {
	long ret;
	register long _r10 __asm__("r10") = a4;
	register long _r8  __asm__("r8")  = a5;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(_r10), "r"(_r8)
		: "rcx", "r11", "memory");
	return ret;
}
static __inline__ long __do_syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
	long ret;
	register long _r10 __asm__("r10") = a4;
	register long _r8  __asm__("r8")  = a5;
	register long _r9  __asm__("r9")  = a6;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(_r10), "r"(_r8), "r"(_r9)
		: "rcx", "r11", "memory");
	return ret;
}

#define __syscall0(n)             __do_syscall0(__scc(n))
#define __syscall1(n,a)           __do_syscall1(__scc(n),__scc(a))
#define __syscall2(n,a,b)         __do_syscall2(__scc(n),__scc(a),__scc(b))
#define __syscall3(n,a,b,c)       __do_syscall3(__scc(n),__scc(a),__scc(b),__scc(c))
#define __syscall4(n,a,b,c,d)     __do_syscall4(__scc(n),__scc(a),__scc(b),__scc(c),__scc(d))
#define __syscall5(n,a,b,c,d,e)   __do_syscall5(__scc(n),__scc(a),__scc(b),__scc(c),__scc(d),__scc(e))
#define __syscall6(n,a,b,c,d,e,f) __do_syscall6(__scc(n),__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),__scc(f))

#ifdef __cplusplus
}
#endif
#endif /* _MLIBC_BITS_SYSCALL_H */
