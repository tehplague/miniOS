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

/**
 * @file enter_ring3.h
 * @defgroup ring3 Ring-3 Entry and Userspace Helpers
 * @brief Functions to transfer execution to ring-3 and set up userspace argument arrays.
 * @{
 */

#ifndef _MINIOS_USERSPACE_ENTER_RING3_H_
#define _MINIOS_USERSPACE_ENTER_RING3_H_

#include <miniOS/types.h>

struct thread;

/**
 * enter_ring3() - Switch from ring 0 to ring 3 via IRETQ.
 * @param entry_rip Ring-3 instruction pointer to jump to (e.g. 0x400000 for ELF entry).
 * @param user_rsp  Ring-3 stack pointer (e.g. USER_STACK_TOP - 8).
 *
 * @brief Builds an IRETQ stack frame (SS, RSP, RFLAGS=0x202, CS=user_cs, RIP) and
 * executes IRETQ, transferring control to @entry_rip at CPL=3. Enables
 * interrupts (RFLAGS.IF=1) as part of the frame. Does not return — execution
 * continues in ring 3 at @entry_rip.
 */
void enter_ring3(uint64_t entry_rip, uint64_t user_rsp);

/**
 * copy_string_array_to_user() - Copy a NULL-terminated string array into a thread's user stack.
 * @param t        Destination thread whose user stack will receive the strings.
 * @param arr      NULL-terminated array of C strings (e.g. argv[] or envp[]).
 * @param user_ptr Output; set to the user-space address of the copied pointer array.
 *
 * @brief Copies each string to the user stack, builds a pointer array below the
 * strings, and writes the address of that array to *@user_ptr. Used by
 * SYS_execve to pass argv/envp to the new process image.
 *
 * @return 0 on success, -1 if the user stack has insufficient space.
 */
int copy_string_array_to_user(struct thread *t, char *const *arr, uint64_t *user_ptr);

/** @} */

#endif
