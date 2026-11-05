// victim.c — Program "korban" yang dipakai demo hooking.
// Mencoba operasi yang akan dicegat supervisor:
//   1. buka /tmp/ok.txt (harus lolos)
//   2. buka /etc/passwd dengan mode tulis (kebijakan seccomp: tolak EACCES)
//   3. buka /tmp/rahasia.txt (ptrace-hook --block-path rahasia: tolak)
//   4. kill diri sendiri dengan SIGKILL (anti-kill: harus ditolak)
//   5. execve /bin/echo di child (harus lolos)
//   6. execve /bin/sh di child (kebijakan seccomp: tolak EACCES)
// Setiap hasil dicetak ke stdout dengan errno-nya; test.sh yang menilai.
//
// Catatan: execve MENGGANTI image proses, jadi setiap uji exec dijalankan di
// child (fork + waitpid). Versi lama mengandalkan eksekusi setelah execve
// sukses — baris itu tidak pernah tercapai sehingga uji /bin/sh tidak pernah
// berjalan.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void show(const char *what, int rc) {
    if (rc == -1)
        printf("  %-45s -> GAGAL errno=%d (%s)\n", what, errno, strerror(errno));
    else
        printf("  %-45s -> OK (rc=%d)\n", what, rc);
}

// Jalankan execve di child; kembalikan exit status child (127 = exec gagal).
static int exec_in_child(char *const argv[]) {
    const pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) return -1;
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
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

    fd = open("/tmp/rahasia.txt", O_RDONLY);
    show("open /tmp/rahasia.txt (baca)", fd == -1 ? -1 : 0);
    if (fd >= 0) close(fd);

    const int rc = kill(getpid(), SIGKILL);
    show("kill(self, SIGKILL)", rc);
    printf("  [victim] masih hidup setelah SIGKILL!\n");

    char *argv_e[] = {"/bin/echo", "halo-dari-execve", NULL};
    printf("  %-45s -> status=%d\n", "execve /bin/echo (child)", exec_in_child(argv_e));

    char *argv_sh[] = {"/bin/sh", "-c", "echo shell-berjalan", NULL};
    printf("  %-45s -> status=%d\n", "execve /bin/sh (child)", exec_in_child(argv_sh));

    printf("[victim] selesai normal\n");
    return 0;
}
