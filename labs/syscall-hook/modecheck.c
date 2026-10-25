// modecheck.c — pembantu uji K1: mode O_CREAT harus diteruskan utuh.
// Diuji lewat LD_PRELOAD=preload-hide.so ./modecheck; file harus lahir 0700.
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    const int fd = open("/tmp/create_mode_test", O_WRONLY | O_CREAT | O_TRUNC, 0700);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    close(fd);
    return 0;
}
