#ifndef _MINIOS_DIRENT_H_
#define _MINIOS_DIRENT_H_

#include <miniOS/types.h>

typedef struct {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
} __attribute__((packed)) linux_dirent64_t;

#endif /* _MINIOS_DIRENT_H_ */
