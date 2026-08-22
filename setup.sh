#!/usr/bin/env bash
# setup.sh — Setup lengkap dari fresh clone: deps → submodule → build → selftest
#
#   ./setup.sh              Setup + build Release
#   ./setup.sh --debug      Setup + build Debug
#   ./setup.sh --clean      Bersihkan build lama, lalu setup fresh
#   ./setup.sh --docker     Build Docker image setelah build selesai
#
# Berjalan di: Ubuntu/Debian, Fedora/RHEL, Arch, macOS (Homebrew)
set -euo pipefail
cd "$(dirname "$0")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[setup]${NC} $*"; }
ok()    { echo -e "${GREEN}[  ok]${NC} $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC} $*"; }
err()   { echo -e "${RED}[err]${NC} $*"; }

BUILD_TYPE="Release"
DOCKER_BUILD=0
CLEAN=0

for arg in "$@"; do
  case "$arg" in
    --debug)   BUILD_TYPE="Debug" ;;
    --clean)   CLEAN=1 ;;
    --docker)  DOCKER_BUILD=1 ;;
    -h|--help)
      echo "Usage: $0 [--debug] [--clean] [--docker]"
      echo "  --debug    Build type Debug (default: Release)"
      echo "  --clean    Hapus build/ lama lalu setup fresh"
      echo "  --docker   Build Docker image setelah build selesai"
      exit 0 ;;
    *) warn "Unknown flag: $arg" ;;
  esac
done

# ============================================================
# 1. Detect OS & Package Manager
# ============================================================
detect_os() {
  if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "$ID"
  elif [ "$(uname)" = "Darwin" ]; then
    echo "macos"
  else
    echo "unknown"
  fi
}

OS=$(detect_os)
info "Detected OS: $OS"

# ============================================================
# 2. Check & Install Dependencies
# ============================================================
MISSING=()

check_cmd() { command -v "$1" &>/dev/null; }

# CMake
if ! check_cmd cmake; then
  MISSING+=(cmake)
fi

# C++ compiler
if ! check_cmd g++ && ! check_cmd clang++; then
  MISSING+=(compiler)
fi

# pkg-config (needed by some deps)
if ! check_cmd pkg-config; then
  MISSING+=(pkg-config)
fi

# OpenSSL dev headers
if ! pkg-config --exists openssl 2>/dev/null; then
  if ! [ -f /usr/include/openssl/ssl.h ]; then
    MISSING+=(openssl-dev)
  fi
fi

# git (wajib untuk submodule)
if ! check_cmd git; then
  MISSING+=(git)
fi

if [ ${#MISSING[@]} -gt 0 ]; then
  info "Missing dependencies: ${MISSING[*]}"
  install_deps
fi

install_deps() {
  case "$OS" in
    ubuntu|debian|pop)
      info "Installing via apt..."
      sudo apt-get update -qq
      sudo apt-get install -y --no-install-recommends \
        build-essential cmake git libssl-dev pkg-config ca-certificates
      ;;
    fedora|rhel|centos|rocky|almalinux)
      info "Installing via dnf/yum..."
      if check_cmd dnf; then
        sudo dnf install -y gcc-c++ cmake git openssl-devel pkg-config ca-certificates
      else
        sudo yum install -y gcc-c++ cmake git openssl-devel pkg-config ca-certificates
      fi
      ;;
    arch|manjaro)
      info "Installing via pacman..."
      sudo pacman -S --needed --noconfirm base-devel cmake git openssl pkgconf
      ;;
    macos)
      info "Installing via Homebrew..."
      if ! check_cmd brew; then
        err "Homebrew not installed. Visit https://brew.sh"
        exit 1
      fi
      brew install cmake openssl pkg-config
      ;;
    *)
      err "Unsupported OS: $OS"
      err "Install manually: cmake, g++/clang++, git, openssl-dev, pkg-config"
      exit 1
      ;;
  esac
  ok "Dependencies installed"
}

if [ ${#MISSING[@]} -gt 0 ]; then
  install_deps
fi

# CMake version check
CMAKE_VER=$(cmake --version | head -1 | grep -oP '\d+\.\d+')
info "CMake version: $CMAKE_VER"

# ============================================================
# 3. Init Git Submodules
# ============================================================
info "Checking git submodules..."

if [ -d ".git" ]; then
  # Cek apakah submodule sudah ter-init (ada file di dalamnya)
  RANDY_READY=0
  FTXUI_READY=0

  if [ -f "external/RandomY/CMakeLists.txt" ] && [ -d "external/RandomY/src" ]; then
    RANDY_READY=1
  fi
  if [ -f "external/FTXUI/CMakeLists.txt" ] && [ -d "external/FTXUI/include" ]; then
    FTXUI_READY=1
  fi

  if [ "$RANDY_READY" -eq 0 ] || [ "$FTXUI_READY" -eq 0 ]; then
    info "Submodules not initialized. Running git submodule update --init --recursive..."
    git submodule update --init --recursive
    ok "Submodules initialized"
  else
    ok "Submodules already initialized"
  fi
else
  err "Not a git repository. Run 'git clone <url>' first."
  exit 1
fi

# ============================================================
# 4. Clean (optional)
# ============================================================
if [ "$CLEAN" -eq 1 ]; then
  info "Cleaning build/ directory..."
  rm -rf build/
  ok "Build directory cleaned"
fi

# ============================================================
# 5. Build
# ============================================================
info "Building miner-saya ($BUILD_TYPE)..."

mkdir -p build
cd build

cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
make -j"$(nproc)"

cd ..
ok "Build successful: build/miner-saya"

# ============================================================
# 6. Selftest
# ============================================================
info "Running selftest..."
LARGE_PAGES=0 ./build/miner-saya --selftest
ok "Selftest passed ✅"

# ============================================================
# 7. Benchmark (optional quick smoke test)
# ============================================================
echo
info "Quick benchmark (5000 hashes)..."
timeout 10 bash -c 'LARGE_PAGES=0 ./build/miner-saya --benchmark 5000' 2>/dev/null || true
ok "Benchmark done"

# ============================================================
# 8. Docker build (optional)
# ============================================================
if [ "$DOCKER_BUILD" -eq 1 ]; then
  info "Building Docker image..."
  docker build -t xcb:miner .
  ok "Docker image built: xcb:miner"
  info "Run with: docker run --rm xcb:miner"
fi

# ============================================================
# Done
# ============================================================
echo
echo -e "${GREEN}═══════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  ✅ Setup complete!${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════════${NC}"
echo
echo "  Quick start:"
echo "    ./mine.sh                  # auto-detect CPU/RAM, set hugepages"
echo "    ./mine.sh --light          # hemat RAM (~256MB)"
echo "    ./mine.sh --benchmark 10000 # smoke test tanpa pool"
echo
echo "  Direct binary:"
echo "    ./build/miner-saya -t 4"
echo "    ./build/miner-saya --selftest"
echo
