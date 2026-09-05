// victim.c — Program "korban" yang dipakai demo hooking.
// Mencoba operasi yang akan dicegat supervisor:
//   1. buka /tmp/ok.txt (harus lolos)
//   2. buka /etc/passwd dengan mode tulis (harus ditolak EACCES)
//   3. bunuh diri dengan SIGKILL (harus ditolak -> proses tetap hidup)
//   4. execve /bin/echo halo (harus lolos)
//   5. execve /bin/sh -c ... (harus ditolak oleh kebijakan)
// Setiap hasil dicetak ke stdout dengan errno-nya.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static void show(const char *what, int rc) {
    if (rc == -1)
        printf("  %-45s -> GAGAL errno=%d (%s)\n", what, errno, strerror(errno));
    else
        printf("  %-45s -> OK (rc=%d)\n", what, rc);
}

int main(void) {
    setbuf(stdout, NULL); // jangan buffer: output tetap terlihat sebelum execve
    printf("[victim] pid=%d mulai\n", getpid());

    int fd = open("/tmp/ok.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    show("open /tmp/ok.txt (tulis)", fd == -1 ? -1 : 0);
    if (fd >= 0) { write(fd, "halo\n", 5); close(fd); }

    fd = open("/etc/passwd", O_WRONLY);
    show("open /etc/passwd (tulis)", fd == -1 ? -1 : 0);
    if (fd >= 0) close(fd);

    int rc = kill(getpid(), SIGKILL);
    show("kill(self, SIGKILL)", rc);
    printf("  [victim] masih hidup setelah SIGKILL!\n");

    char *argv_e[] = {"/bin/echo", "halo-dari-execve", NULL};
    rc = execve("/bin/echo", argv_e, environ);
    show("execve /bin/echo", rc);

    char *argv_sh[] = {"/bin/sh", "-c", "echo shell-berjalan", NULL};
    rc = execve("/bin/sh", argv_sh, environ);
    show("execve /bin/sh", rc);

    printf("[victim] selesai normal\n");
    return 0;
}
