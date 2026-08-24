#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()  { echo -e "${CYAN}[setup]${NC} $*"; }
ok()    { echo -e "${GREEN}[  ok]${NC} $*"; }
err()   { echo -e "${RED}[err]${NC} $*"; }

BUILD_TYPE=Release; CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --debug) BUILD_TYPE=Debug ;;
    --clean) CLEAN=1 ;;
    -h|--help) echo "Usage: $0 [--debug] [--clean]"; exit 0 ;;
  esac
done

detect_os() {
  . /etc/os-release 2>/dev/null && echo "$ID" || (uname | grep -qi darwin && echo macos || echo unknown)
}
OS=$(detect_os)
info "Detected OS: $OS"

install_deps() {
  case $OS in
    alpine) apk add --no-cache build-base cmake git openssl-dev pkgconf ;;
    ubuntu|debian|pop)
      sudo apt-get update -qq && sudo apt-get install -y --no-install-recommends \
        build-essential cmake git libssl-dev pkg-config ca-certificates ;;
    fedora|rhel|centos|rocky|almalinux)
      dnf install -y gcc-c++ cmake git openssl-devel pkg-config ca-certificates \
        || yum install -y gcc-c++ cmake git openssl-devel pkg-config ca-certificates ;;
    arch|manjaro)
      sudo pacman -S --needed --noconfirm base-devel cmake git openssl pkgconf ;;
    macos)
      command -v brew >/dev/null 2>&1 || { err "Homebrew required"; exit 1; }
      brew install cmake openssl pkg-config ;;
    *) err "Unsupported OS: $OS"; exit 1 ;;
  esac
  ok "Dependencies installed"
}
install_deps

CMAKE_VER=$(cmake --version | head -1 | sed 's/[^0-9.]//g' | cut -d. -f1-2)
info "CMake: $CMAKE_VER"

# Detect musl — alpine musl can't build with flto + fortify
NO_LTO=0
ldd --version 2>&1 | grep -qi musl && NO_LTO=1 && info "musl libc detected: building without LTO"

[ -d .git ] || { err "Not a git repo"; exit 1; }
if [ ! -f external/RandomY/CMakeLists.txt ] || [ ! -f external/FTXUI/CMakeLists.txt ]; then
  info "Initializing submodules..."
  git submodule update --init --recursive
fi

[ "$CLEAN" = 1 ] && rm -rf build

info "Building miner-saya ($BUILD_TYPE)..."
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
         -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
         -DNO_LTO="$NO_LTO"
make -j"$(nproc)"
cd ..
ok "Build: build/miner-saya"

info "Selftest..."
LARGE_PAGES=0 ./build/miner-saya --selftest
ok "Selftest passed"

echo && info "Benchmark 5000 hashes..."
timeout 10 bash -c 'LARGE_PAGES=0 ./build/miner-saya --benchmark 5000' 2>/dev/null || true
