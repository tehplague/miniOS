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
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 1024

static void write_str(const char *s) {
    write(1, s, strlen(s));
}

int main(int argc, char *argv[], char *envp[]) {
    char line[BUF_SIZE];

    (void)argc;
    (void)argv;

    // Simple env var check for TERM
    for (char **env = envp; env && *env; env++) {
        if (strncmp(*env, "TERM=", 5) == 0) {
            // We have a terminal!
            break;
        }
    }

    for (;;) {
        ssize_t nread;

        write_str("$ ");

        nread = read(0, line, BUF_SIZE - 1);
        if (nread <= 0) {
            continue;
        }

        /* Strip all trailing CR/LF characters */
        while (nread > 0 && (line[nread - 1] == '\n' || line[nread - 1] == '\r'))
            line[--nread] = '\0';

        if (line[0] == '\0') continue;

        /* Built-in: exit */
        if (line[0]=='e' && line[1]=='x' && line[2]=='i' && line[3]=='t' && line[4]=='\0') {
            write_str("Goodbye\n");
            _exit(0);
        }

        /* Tokenize line into space-separated words.
         * argv[0] = full path (/bin/cmd), argv[1..] = arguments.
         * Phase 17 scope: max 8 tokens total (command + 7 args). */
        #define SH_MAX_ARGS 8
        char *tokens[SH_MAX_ARGS + 1];  /* +1 for NULL sentinel */
        int token_count = 0;

        /* Split line on spaces into tokens[] pointing into line[] */
        char *p = line;
        while (*p && token_count < SH_MAX_ARGS) {
            /* Skip leading spaces */
            while (*p == ' ') p++;
            if (*p == '\0') break;
            /* Token starts here */
            tokens[token_count++] = p;
            /* Advance to next delimiter; treat \n/\r same as end-of-input */
            while (*p && *p != ' ' && *p != '\n' && *p != '\r') p++;
            if (*p == ' ') { *p = '\0'; p++; }
            else if (*p) { *p = '\0'; break; } /* \n or \r: end tokenizing */
        }

        if (token_count == 0) continue;

        /* Built-in: cd — must execute in parent (no fork) so cwd change persists.
         * CRITICAL: This check must be BEFORE the fork/exec path. */
        if (token_count >= 1 &&
            tokens[0][0] == 'c' && tokens[0][1] == 'd' && tokens[0][2] == '\0') {
            const char *target = (token_count >= 2) ? tokens[1] : "/";
            if (chdir(target) < 0) {
                write_str("cd: cannot change directory\n");
            }
            continue;
        }

        /* Scan tokens for '|' pipe operator */
        int pipe_idx = -1;
        for (int i = 0; i < token_count; i++) {
            if (tokens[i][0] == '|' && tokens[i][1] == '\0') {
                pipe_idx = i;
                break;
            }
        }

        if (pipe_idx == -1) {
            /* --- Single-command path (no pipe) --- */

            /* Scan tokens for '>' redirect operator.
            * If found, extract the target path and remove '>' + path from tokens. */
            const char *redirect_out = NULL;
            int new_count = 0;
            char *final_tokens[SH_MAX_ARGS + 1];
            for (int i = 0; i < token_count; i++) {
                if (tokens[i][0] == '>' && tokens[i][1] == '\0') {
                    /* Next token is the redirect target */
                    if (i + 1 < token_count) {
                        redirect_out = tokens[i + 1];
                        i++;  /* skip path token too */
                    }
                } else {
                    final_tokens[new_count++] = tokens[i];
                }
            }
            final_tokens[new_count] = NULL;

            if (new_count == 0) continue;

            /* Build /bin/<cmd> path from final_tokens[0] (the command word).
            * If the command already starts with '/', use it as-is. */
            char path[BUF_SIZE + 6];
            if (final_tokens[0][0] == '/') {
                int i = 0;
                while ((path[i] = final_tokens[0][i]) != '\0') i++;
            } else {
                int pi = 0;
                path[pi++] = '/'; path[pi++] = 'b'; path[pi++] = 'i';
                path[pi++] = 'n'; path[pi++] = '/';
                for (int i = 0; final_tokens[0][i] != '\0'; i++) path[pi++] = final_tokens[0][i];
                path[pi] = '\0';
            }

            /* Replace final_tokens[0] with the full path so argv[0] is the program name */
            final_tokens[0] = path;

            /* Build argv for execve */
            char *argv_new[SH_MAX_ARGS + 1];
            for (int i = 0; i <= new_count; i++) argv_new[i] = final_tokens[i];

            pid_t child = fork();
            if (child == 0) {
                /* Child: set up redirect if requested, then exec */
                if (redirect_out != NULL) {
                    int rfd = open(redirect_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (rfd < 0) {
                        write_str("sh: cannot open redirect target\n");
                        _exit(1);
                    }
                    dup2(rfd, 1);   /* redirect stdout to file */
                    close(rfd);
                }
                execve(path, (char *const *)argv_new, (char *const *)envp);
                /* execve returns only on failure */
                write_str("sh: command not found\n");
                _exit(1);
            } else {
                /* Parent: wait for child to finish. */
                int status = 0;
                waitpid(child, &status, 0);
            }
        } else {
            /* --- Pipe execution path: cmd1 | cmd2 ---
             * Left side: tokens[0..pipe_idx-1]
             * Right side: tokens[pipe_idx+1..token_count-1] (may include '>' redirect) */

            /* Create anonymous pipe: fds[0]=read end, fds[1]=write end */
            int fds[2];
            if (pipe(fds) != 0) {
                write_str("sh: pipe() failed\n");
                continue;
            }

            /* Build left command argv (tokens before '|') */
            char *left_argv[SH_MAX_ARGS + 1];
            int left_argc = 0;
            for (int i = 0; i < pipe_idx && left_argc < SH_MAX_ARGS; i++) {
                left_argv[left_argc++] = tokens[i];
            }
            left_argv[left_argc] = NULL;

            if (left_argc == 0) {
                close(fds[0]); close(fds[1]);
                continue;
            }

            /* Build left command path (/bin/<cmd> or absolute) */
            char left_path[BUF_SIZE + 6];
            if (left_argv[0][0] == '/') {
                int i = 0;
                while ((left_path[i] = left_argv[0][i]) != '\0') i++;
            } else {
                int pi = 0;
                left_path[pi++] = '/'; left_path[pi++] = 'b'; left_path[pi++] = 'i';
                left_path[pi++] = 'n'; left_path[pi++] = '/';
                for (int i = 0; left_argv[0][i] != '\0'; i++) left_path[pi++] = left_argv[0][i];
                left_path[pi] = '\0';
            }
            left_argv[0] = left_path;

            /* Build right command argv (tokens after '|', excluding '>' and filename) */
            char *right_argv[SH_MAX_ARGS + 1];
            int right_argc = 0;
            const char *right_redirect = NULL;
            int right_start = pipe_idx + 1;
            for (int i = right_start; i < token_count && right_argc < SH_MAX_ARGS; i++) {
                if (tokens[i][0] == '>' && tokens[i][1] == '\0') {
                    if (i + 1 < token_count) {
                        right_redirect = tokens[i + 1];
                        i++;  /* skip filename */
                    }
                } else {
                    right_argv[right_argc++] = tokens[i];
                }
            }
            right_argv[right_argc] = NULL;

            if (right_argc == 0) {
                close(fds[0]); close(fds[1]);
                continue;
            }

            /* Build right command path */
            char right_path[BUF_SIZE + 6];
            if (right_argv[0][0] == '/') {
                int i = 0;
                while ((right_path[i] = right_argv[0][i]) != '\0') i++;
            } else {
                int pi = 0;
                right_path[pi++] = '/'; right_path[pi++] = 'b'; right_path[pi++] = 'i';
                right_path[pi++] = 'n'; right_path[pi++] = '/';
                for (int i = 0; right_argv[0][i] != '\0'; i++) right_path[pi++] = right_argv[0][i];
                right_path[pi] = '\0';
            }
            right_argv[0] = right_path;

            /* Fork left child: stdout -> pipe write-end */
            pid_t left_pid = fork();
            if (left_pid == 0) {
                /* Left child: close read end, dup2 write end to stdout */
                close(fds[0]);
                dup2(fds[1], 1);
                close(fds[1]);
                execve(left_path, (char *const *)left_argv, (char *const *)envp);
                write_str("sh: command not found\n");
                _exit(1);
            }

            /* Fork right child: stdin -> pipe read-end */
            pid_t right_pid = fork();
            if (right_pid == 0) {
                /* Right child: close write end, dup2 read end to stdin */
                close(fds[1]);
                dup2(fds[0], 0);
                close(fds[0]);

                /* Apply output redirection if present (e.g., cmd1 | cmd2 > file) */
                if (right_redirect != NULL) {
                    int rfd = open(right_redirect, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (rfd < 0) {
                        write_str("sh: cannot open redirect target\n");
                        _exit(1);
                    }
                    dup2(rfd, 1);
                    close(rfd);
                }

                execve(right_path, (char *const *)right_argv, (char *const *)envp);
                write_str("sh: command not found\n");
                _exit(1);
            }

            /* Parent: close both pipe ends so children see EOF when they exit */
            close(fds[0]);
            close(fds[1]);

            /* Wait for both children */
            waitpid(left_pid, NULL, 0);
            waitpid(right_pid, NULL, 0);
        }
    }
    return 0;
}
