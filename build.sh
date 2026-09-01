#!/usr/bin/env bash
# build.sh — Build cepat tanpa install dependencies
#
#   ./build.sh              Build Release
#   ./build.sh --debug      Build Debug
#   ./build.sh --clean      Clean lalu build fresh
#   ./build.sh --no-test    Build saja tanpa selftest
#   ./build.sh -j8          Override jumlah thread make
set -euo pipefail
cd "$(dirname "$0")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[build]${NC} $*"; }
ok()    { echo -e "${GREEN}[  ok]${NC} $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC} $*"; }
err()   { echo -e "${RED}[err]${NC} $*"; exit 1; }

BUILD_TYPE="Release"
CLEAN=0
RUN_TEST=1
JOBS=$(nproc)

for arg in "$@"; do
  case "$arg" in
    --debug)    BUILD_TYPE="Debug" ;;
    --clean)    CLEAN=1 ;;
    --no-test)  RUN_TEST=0 ;;
    -j*)        JOBS="${arg#-j}" ;;
    -h|--help)
      echo "Usage: $0 [--debug] [--clean] [--no-test] [-jN]"
      exit 0 ;;
    *) warn "Unknown flag: $arg" ;;
  esac
done

# Pre-flight checks
command -v cmake &>/dev/null || err "cmake not found. Run ./setup.sh first."
command -v make  &>/dev/null || err "make not found. Run ./setup.sh first."

# Submodule check
if [ ! -f "external/RandomY/CMakeLists.txt" ] || [ ! -f "external/FTXUI/CMakeLists.txt" ]; then
  warn "Submodules not initialized. Running git submodule update..."
  git submodule update --init --recursive
fi

# Clean
if [ "$CLEAN" -eq 1 ]; then
  info "Cleaning build/..."
  rm -rf build/
fi

# Build
mkdir -p build
cd build

info "Configuring ($BUILD_TYPE)..."
cmake .. -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON 2>&1

info "Building with $JOBS threads..."
make -j"$JOBS" 2>&1

cd ..
ok "Build complete: build/miner-saya"

# Selftest
if [ "$RUN_TEST" -eq 1 ]; then
  info "Running selftest..."
  ./build/miner-saya --selftest
  ok "Selftest passed ✅"
fi
