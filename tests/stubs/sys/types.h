/* stubs/sys/types.h — prevents sys/types.h from redefining time_t that
 * stub_types.h already defined as uint64_t for the host test build. */
#ifndef _STUB_SYS_TYPES_H_
#define _STUB_SYS_TYPES_H_
/* Pull in the real sys/types.h but wrap time_t to avoid conflict */
#define time_t __sys_time_t_unused
#include_next <sys/types.h>
#undef time_t
/* Restore stub_types.h definition: time_t = uint64_t */
typedef uint64_t time_t;
#endif /* _STUB_SYS_TYPES_H_ */
