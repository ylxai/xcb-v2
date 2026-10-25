#!/usr/bin/env bash
# sandbox-status.sh — status semua sandbox instabox (padanan fleet-status.sh)
#
#   ./sandbox-status.sh              pakai daftar default
#   ./sandbox-status.sh xcb2 xcb5    cek sandbox tertentu
#
# Yang dicek per sandbox:
#   state    : state mesin di instabox (running/suspended/stopped/...)
#   svc      : unit miner (syscached) active?
#   alive    : proses miner hidup? (dari systemd MainPID)
#   hr       : hashrate terakhir dari journal
#   sh       : share diterima/ditolak (A/R/W)
#   worker   : nama worker yang dipakai pool
#   run_left : sisa waktu sebelum batas max-run (8 jam) tercapai
set -u
source "$HOME/.instabox/env" 2>/dev/null || { echo "env instabox tidak ada (~/.instabox/env)"; exit 1; }
IB="$HOME/.local/bin/instabox"
MACHINES="${*:-analisa1 xcb2 xcb3 xcb4 xcb5}"
UNIT="${UNIT:-syscached}"

probe='
svc=$(systemctl is-active '"$UNIT"' 2>&1)
pid=$(systemctl show -p MainPID --value '"$UNIT"' 2>/dev/null)
alive=no
[ -n "${pid:-}" ] && [ "$pid" != 0 ] && [ -d "/proc/$pid" ] && alive=yes
j=$(sudo journalctl -u '"$UNIT"' -n 40 --no-pager 2>/dev/null)
hr=$(echo "$j" | grep -o "HR [0-9.]* kH/s" | tail -1 | tr " " "_")
sh=$(echo "$j" | grep -o "A [0-9]* R [0-9]* W [0-9]*" | tail -1 | tr " " "_")
auth=$(sudo journalctl -u '"$UNIT"' -n 2000 --no-pager 2>/dev/null | grep -o "Authorized as .*" | tail -1 | sed "s/.*\.//")
printf "svc=%s alive=%s hr=%s sh=%s auth=%s\n" "$svc" "$alive" "${hr:-n/a}" "${sh:-n/a}" "${auth:-n/a}"
'
pick() { sed -n "s/.*$1=\([^ ]*\).*/\1/p"; }

printf "%-9s %-10s %-8s %-6s %-13s %-24s %-8s %s\n" \
  NAME STATE SVC ALIVE HASH SHARES WORKER RUN_LEFT
printf "%s\n" "---------------------------------------------------------------------------------------------------"

fails=0
for m in $MACHINES; do
  info=$("$IB" get "$m" --json 2>/dev/null)
  if [ -z "$info" ]; then
    printf "%-9s %-10s %s\n" "$m" "?" "TIDAK BISA DIHUBUNGI"; fails=$((fails+1)); continue
  fi
  state=$(jq -r '.state // "?"' <<<"$info")
  runat=$(jq -r '.runStartedAt // ""' <<<"$info")
  maxrun=$(jq -r '.maxRunMinutes // 0' <<<"$info")

  left="n/a"
  if [ "$state" = "running" ] && [ -n "$runat" ] && [ "$maxrun" -gt 0 ] 2>/dev/null; then
    start=$(date -d "$runat" +%s 2>/dev/null || echo 0)
    rem=$(( maxrun * 60 - $(date +%s) + start ))
    [ "$rem" -lt 0 ] && rem=0
    left="$rem"
  fi

  out=""
  if [ "$state" = "running" ]; then
    out=$("$IB" exec "$m" -- bash -c "$probe" 2>/dev/null)
  fi
  if [ -z "$out" ]; then
    printf "%-9s %-10s %s\n" "$m" "$state" "EXEC GAGAL/UNREACHABLE"; fails=$((fails+1)); continue
  fi

  svc=$(echo "$out" | pick svc)
  alive=$(echo "$out" | pick alive)
  hr=$(echo "$out" | pick hr)
  sh=$(echo "$out" | pick sh)
  auth=$(echo "$out" | pick auth)

  if [ "$svc" = "active" ] && [ "$alive" = "yes" ]; then st="OK"; else st="PROBLEM"; fails=$((fails+1)); fi

  printf "%-9s %-10s %-8s %-6s %-13s %-24s %-8s %ss\n" \
    "$m" "$state" "$st" "$alive" "${hr:-n/a}" "${sh:-n/a}" "${auth:-n/a}" "$left"
done

echo
[ "$fails" -eq 0 ] && echo "SEMUA SANDBOX SEHAT" || echo "$fails sandbox bermasalah"
exit 0
