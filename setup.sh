#!/usr/bin/env bash
# setup.sh — Setup lengkap dari fresh clone: deps → submodule → build → selftest
#
#   ./setup.sh              Setup + build Release
#   ./setup.sh --debug      Setup + build Debug
#   ./setup.sh --clean      Bersihkan build lama, lalu setup fresh
#
# Berjalan di: Ubuntu/Debian, Fedora/RHEL, Arch (+turunan), Alpine, macOS
set -euo pipefail
cd "$(dirname "$0")"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[setup]${NC} $*"; }
ok()    { echo -e "${GREEN}[  ok]${NC} $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC} $*"; }
err()   { echo -e "${RED}[err]${NC} $*"; }

BUILD_TYPE=Release; CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --debug) BUILD_TYPE=Debug ;;
    --clean) CLEAN=1 ;;
    -h|--help)
      echo "Usage: $0 [--debug] [--clean]"
      echo "  --debug    Build type Debug (default: Release)"
      echo "  --clean    Hapus build/ lama lalu setup fresh"
      exit 0 ;;
    *) err "Unknown flag: $arg (lihat --help)"; exit 1 ;;
  esac
done

# ============================================================
# 1. Detect OS
# ============================================================
# ID bersifat opsional di /etc/os-release, dan ID_LIKE menangkap turunan
# (cachyos/endeavouros -> arch, pop -> debian) tanpa perlu daftar panjang.
detect_os() {
  if [ -r /etc/os-release ]; then
    local ID="" ID_LIKE=""
    # shellcheck disable=SC1091
    . /etc/os-release
    if [ -n "$ID" ]; then echo "$ID"; return; fi
    if [ -n "$ID_LIKE" ]; then echo "${ID_LIKE%% *}"; return; fi
  fi
  case "$(uname -s)" in
    Darwin) echo macos ;;
    *)      echo unknown ;;
  esac
}

detect_os_family() {
  if [ -r /etc/os-release ]; then
    local ID_LIKE=""
    # shellcheck disable=SC1091
    . /etc/os-release
    [ -n "$ID_LIKE" ] && echo "$ID_LIKE" && return
  fi
  echo ""
}

OS=$(detect_os)
OS_FAMILY=$(detect_os_family)
info "Detected OS: $OS${OS_FAMILY:+ (family: $OS_FAMILY)}"

# sudo hanya dipakai kalau bukan root dan sudo tersedia (container biasanya root
# tanpa sudo terpasang).
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  if command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
  else
    warn "Bukan root dan sudo tidak ada — install dependency bisa gagal"
  fi
fi

# ============================================================
# 2. Check dependencies — install HANYA kalau ada yang kurang
# ============================================================
check_cmd() { command -v "$1" >/dev/null 2>&1; }

install_deps() {
  # Cocokkan ID dulu, lalu ID_LIKE sebagai fallback untuk distro turunan.
  case "$OS $OS_FAMILY" in
    alpine*|*" alpine"*)
      $SUDO apk add --no-cache build-base cmake git openssl-dev pkgconf ;;
    ubuntu*|debian*|pop*|*debian*)
      $SUDO apt-get update -qq
      $SUDO apt-get install -y --no-install-recommends \
        build-essential cmake git libssl-dev pkg-config ca-certificates ;;
    fedora*|rhel*|centos*|rocky*|almalinux*|*rhel*|*fedora*)
      if check_cmd dnf; then
        $SUDO dnf install -y gcc-c++ cmake git openssl-devel pkg-config ca-certificates
      else
        $SUDO yum install -y gcc-c++ cmake git openssl-devel pkg-config ca-certificates
      fi ;;
    arch*|manjaro*|cachyos*|endeavouros*|*arch*)
      $SUDO pacman -S --needed --noconfirm base-devel cmake git openssl pkgconf ;;
    macos*)
      check_cmd brew || { err "Homebrew required — https://brew.sh"; exit 1; }
      brew install cmake openssl pkg-config ;;
    *)
      err "Unsupported OS: $OS"
      err "Install manual: cmake, g++/clang++, git, openssl-dev, pkg-config"
      exit 1 ;;
  esac
  ok "Dependencies installed"
}

MISSING=()
check_cmd cmake      || MISSING+=(cmake)
check_cmd git        || MISSING+=(git)
check_cmd pkg-config || MISSING+=(pkg-config)
{ check_cmd g++ || check_cmd clang++; } || MISSING+=(compiler)
if ! pkg-config --exists openssl 2>/dev/null && [ ! -f /usr/include/openssl/ssl.h ]; then
  MISSING+=(openssl-dev)
fi

if [ ${#MISSING[@]} -gt 0 ]; then
  info "Missing dependencies: ${MISSING[*]}"
  install_deps
else
  ok "Semua dependency sudah ada — skip install"
fi

info "CMake: $(cmake --version | head -1)"

# ============================================================
# 3. Submodules
# ============================================================
# git rev-parse, bukan [ -d .git ] — .git adalah FILE di worktree/submodule.
git rev-parse --git-dir >/dev/null 2>&1 || { err "Bukan git repository — clone dulu"; exit 1; }

# Verifikasi isi, bukan hanya CMakeLists.txt: checkout parsial lolos cek dangkal
# lalu gagal saat compile dengan error yang membingungkan.
if [ ! -d external/RandomY/src ] || [ ! -d external/FTXUI/include ]; then
  info "Initializing submodules..."
  git submodule update --init --recursive
  ok "Submodules initialized"
else
  ok "Submodules already initialized"
fi

# ============================================================
# 4. Build
# ============================================================
if [ "$CLEAN" -eq 1 ]; then
  info "Cleaning build/ ..."
  rm -rf build
fi

# LTO/musl ditangani di CMakeLists.txt (deteksi via -dumpmachine), tidak lagi
# lewat probe `ldd --version` yang exit non-zero di musl dan tertelan pipefail.
info "Building miner-saya ($BUILD_TYPE)..."
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
cd ..
ok "Build: build/miner-saya"

# ============================================================
# 5. Selftest
# ============================================================
info "Selftest..."
./build/miner-saya --selftest
ok "Selftest passed"

echo
echo -e "${GREEN}✅ Setup complete${NC}"
echo "  ./mine.sh                    # auto-detect CPU/RAM, set hugepages"
echo "  ./build/miner-saya -t 4      # jalankan langsung"
echo "  ./build/miner-saya --selftest"
echo "  ./docker-build.sh            # build image docker"
