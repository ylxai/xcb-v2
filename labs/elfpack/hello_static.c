/* hello_static.c — payload test static (glibc -static, ET_EXEC)
 * Verifikasi: output + exit code harus identik antara binary asli & packed.
 */
#include <stdio.h>

int main(int argc, char **argv, char **envp) {
    int e = 0;
    while (envp[e]) e++;
    printf("ELFPACK-STATIC-OK argc=%d argv0=%s envs=%d\n", argc, argv[0], e);
    return 42;
}
