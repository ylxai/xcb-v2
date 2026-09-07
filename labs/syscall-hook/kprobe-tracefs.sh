#!/bin/sh
# kprobe-tracefs.sh — Kernel-level syscall hooking TANPA modul kernel.
#
# Mengapa ada: LKM (kmod/) butuh kernel headers + gcc + make + /lib/modules
# yang cocok dengan kernel BERJALAN. Di Namespace devbox/machine (kernel host
# custom, container Wolfi minimal) itu tidak tersedia, jadi kompilasi modul
# mustahil. Pengganti yang setara dan justru lebih modern: kprobe via tracefs
# — mekanisme kernel resmi yang sama dipakai eBPF/LTTng/perf untuk mencegat
# fungsi kernel. Kita hook do_sys_openat2 (jalur inti semua open/openat modern)
# dan membaca argumennya (dfd, filename, flags, mode) langsung dari register
# CPU saat syscall dieksekusi.
#
# Prasyarat: CAP_SYS_ADMIN (root container), tracefs ter-mount di
# /sys/kernel/tracing, kernel dengan CONFIG_KPROBE_EVENTS (hampir semua distro).
#
# Pakai:  ./kprobe-tracefs.sh "ls /etc"        # hook satu command
#         ./kprobe-tracefs.sh                  # hook sampai Ctrl-C
#
# Keluar: trace ditulis ke stdout. Event kprobe dihapus otomatis (no trace).

set -e
TR=/sys/kernel/tracing
EVENT=hook_open

cleanup() {
    echo 0 > "$TR/events/kprobes/enable" 2>/dev/null || true
    echo > "$TR/kprobe_events" 2>/dev/null || true
    echo 0 > "$TR/tracing_on" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# Cek prasyarat
[ -w "$TR/kprobe_events" ] || { echo "ERROR: $TR/kprobe_events tidak writable (butuh root + tracefs)" >&2; exit 1; }

cleanup

# Daftarkan kprobe: hook do_sys_openat2, tangkap 4 argumen pertama.
# %di=dfd(int), %si=filename(char* user), %dx=flags(int), %cx=mode(umode_t)
echo "p:kprobes/$EVENT do_sys_openat2 dfd=%di filename=+0(%si):string flags=%dx mode=%cx" > "$TR/kprobe_events"
echo "kprobe terdaftar: do_sys_openat2 -> events/kprobes/$EVENT"

echo 1 > "$TR/events/kprobes/$EVENT/enable"
echo 1 > "$TR/tracing_on"
echo ">>> HOOK AKTIF — setiap open/openat di kernel tercatat <<<"

if [ $# -gt 0 ]; then
    echo ">>> menjalankan: $* <<<"
    "$@"
    sleep 0.2
    echo 0 > "$TR/tracing_on"
    echo ">>> TRACE (beberapa baris pertama) <<<"
    grep -v '^#' "$TR/trace" | grep "$EVENT" | head -40 || echo "(tidak ada event tertangkap)"
else
    echo ">>> tekan Ctrl-C untuk berhenti <<<"
    while :; do sleep 1; done
fi
