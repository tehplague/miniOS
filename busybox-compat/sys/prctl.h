#ifndef _SYS_PRCTL_H
#define _SYS_PRCTL_H

#define PR_SET_NAME 15
#define PR_GET_NAME 16

#ifdef __cplusplus
extern "C" {
#endif

int prctl(int option, unsigned long arg2, unsigned long arg3,
          unsigned long arg4, unsigned long arg5);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_PRCTL_H */
