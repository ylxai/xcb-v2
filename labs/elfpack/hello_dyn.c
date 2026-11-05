/* hello_dyn.c — payload test dynamic (PIE, linked libc) */
#include <stdio.h>

int main(int argc, char **argv, char **envp) {
    int e = 0;
    while (envp[e]) e++;
    printf("ELFPACK-DYNAMIC-OK argc=%d argv0=%s envs=%d\n", argc, argv[0], e);
    return 7;
}
