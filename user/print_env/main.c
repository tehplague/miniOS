#include <stdio.h>

int main(int argc, char *argv[], char *envp[]) {
    printf("--- print_env arguments ---\n");
    for (int i = 0; i < argc; i++) {
        printf("argv[%d]: %s\n", i, argv[i]);
    }
    
    printf("--- print_env environment ---\n");
    if (envp) {
        for (int i = 0; envp[i] != NULL; i++) {
            printf("envp[%d]: %s\n", i, envp[i]);
        }
    } else {
        printf("envp is NULL\n");
    }
    printf("--- end print_env ---\n");

    return 0;
}
