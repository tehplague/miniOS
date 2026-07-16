// MIT License
//
// Copyright (c) 2026 Christian Spoo
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>

#ifndef MINI_OS_INIT_DEBUG
#define MINI_OS_INIT_DEBUG 0
#endif

/* Raw syscall wrapper for mount (nr=165) — not in Newlib */
static long sys_mount(const char *source, const char *target,
                       const char *fstype, unsigned long flags,
                       const void *data)
{
    long ret;
    register long r10 __asm__("r10") = flags;
    register long r8  __asm__("r8")  = (long)data;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"((long)165), "D"(source), "S"(target), "d"(fstype),
          "r"(r10), "r"(r8)
        : "rcx", "r11", "memory"
    );
    return ret;
}

#if MINI_OS_INIT_DEBUG
static void iprint(const char *s) { int l=0; while(s[l])l++; write(1,s,l); }
#define INIT_DEBUG_PRINT(msg) iprint(msg)
#else
#define INIT_DEBUG_PRINT(msg) do { (void)(msg); } while (0)
#endif

int main(int argc, char *argv[], char *envp[]) {
    (void)argc;
    (void)argv;
    (void)envp;

    INIT_DEBUG_PRINT("init: main entered\n");

    /* Mount /sys — /sys is pre-created in the disk image */
    sys_mount("none", "/sys", "sysfs", 0, NULL);
    INIT_DEBUG_PRINT("init: /sys mounted\n");

    /* Mount /data — /data is pre-created in the disk image */
    sys_mount("/dev/sda2", "/data", "fat32", 0, NULL);
    INIT_DEBUG_PRINT("init: /data mounted\n");

    /* /tmp and /dev are pre-mounted by the kernel with /dev/null and
     * /dev/tty already created; no userspace setup needed here. */

    // Set up environment variables
    char *shell_envp[] = {
        "TERM=xterm-256color",
        "PATH=/bin:/test/bin",
        NULL
    };

    INIT_DEBUG_PRINT("init: trying autorun\n");
    if (access("/test/.autorun", 0) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            /* child: try ELF first, then sh */
            char *autorun_argv[] = {"/test/.autorun", NULL};
            execve("/test/.autorun", autorun_argv, shell_envp);
            char *sh_autorun_argv[] = {"/bin/sh", "/test/.autorun", NULL};
            execve("/bin/sh", sh_autorun_argv, shell_envp);
            _exit(1);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        }
    }
    INIT_DEBUG_PRINT("init: autorun done, starting shell\n");

    // Launch the shell
    char *shell_argv[] = {"/bin/sh", NULL};
    execve("/bin/sh", shell_argv, shell_envp);

    // If we get here, execve failed
    return 1;
}
