#!/usr/bin/env bash
# instabox-keepalive.sh — auto-on & anti-idle untuk sandbox instabox.
#
#   - running    -> kirim exec kecil (reset timer idle 30 menit)
#   - suspended  -> bangunkan (exec otomatis me-resume yang pause karena idle)
#   - stopped    -> start lagi (setelah batas max-run 8 jam / stop manual;
#                   syscached enabled sehingga miner hidup sendiri saat boot)
#
# Cron (5 menit < idle 30 menit):
#   */5 * * * * $HOME/.instabox/instabox-keepalive.sh >> $HOME/.instabox/keepalive.log 2>&1
set -u
source "$HOME/.instabox/env" 2>/dev/null || { echo "$(date '+%F %T') env instabox tidak ada"; exit 1; }
IB="$HOME/.local/bin/instabox"
MACHINES="${MACHINES:-analisa1 xcb2 xcb3 xcb4 xcb5}"
ts() { date "+%F %T"; }

for m in $MACHINES; do
  state=$("$IB" get "$m" --json 2>/dev/null | jq -r '.state // ""')
  case "$state" in
    running)
      if "$IB" exec "$m" -- true >/dev/null 2>&1; then
        echo "$(ts) $m: running, idle-reset OK"
      else
        echo "$(ts) $m: running tapi exec gagal (guest hang? coba restart manual)"
      fi
      ;;
    suspended|suspending)
      if "$IB" exec "$m" -- true >/dev/null 2>&1; then
        echo "$(ts) $m: suspended -> resumed (via exec)"
      elif "$IB" resume "$m" >/dev/null 2>&1; then
        echo "$(ts) $m: suspended -> resumed"
      else
        echo "$(ts) $m: resume gagal"
      fi
      ;;
    stopped|error|"")
      if "$IB" start "$m" >/dev/null 2>&1; then
        echo "$(ts) $m: stopped -> started"
      else
        echo "$(ts) $m: start gagal (rate-limit 3 start/menit? coba siklus berikutnya)"
      fi
      ;;
    *)
      echo "$(ts) $m: state=$state (tunggu)"
      ;;
  esac
done
