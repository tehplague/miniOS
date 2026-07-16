#include <stdio.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    char *const print_env_argv[] = {"/test/bin/print_env", "from_test_env", NULL};
    char *const print_env_envp[] = {"GREETING=HELLO", "SUBJECT=WORLD", NULL};

    printf("--- test_env starting ---\n");
    execve("/test/bin/print_env", print_env_argv, print_env_envp);

    // If execve returns, it must have failed
    perror("execve failed");
    return 1;
}
