#!/usr/bin/env bash
# docker-build.sh — Build & jalankan Docker image
#
#   ./docker-build.sh              Build image xcb:miner
#   ./docker-build.sh --run        Build + run container
#   ./docker-build.sh --push       Build + push ke Docker Hub (ylxai/xcb)
#   ./docker-build.sh --no-cache   Build tanpa cache
set -euo pipefail
cd "$(dirname "$0")"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[docker]${NC} $*"; }
ok()    { echo -e "${GREEN}[  ok]${NC} $*"; }
warn()  { echo -e "${YELLOW}[warn]${NC} $*"; }
err()   { echo -e "${RED}[err]${NC} $*"; exit 1; }

IMAGE="xcb:miner"
PUSH_IMAGE="ylxai/xcb:v2"
RUN=0
PUSH=0
NO_CACHE=""
CONTAINER_NAME="xcb-miner"

for arg in "$@"; do
  case "$arg" in
    --run)      RUN=1 ;;
    --push)     PUSH=1 ;;
    --no-cache) NO_CACHE="--no-cache" ;;
    -h|--help)
      echo "Usage: $0 [--run] [--push] [--no-cache]"
      echo "  --run       Build + run container"
      echo "  --push      Build + push to ylxai/xcb:v2"
      echo "  --no-cache  Build without Docker cache"
      exit 0 ;;
    *) warn "Unknown flag: $arg" ;;
  esac
done

# Pre-flight
command -v docker &>/dev/null || err "Docker not installed."

# Submodule check (Dockerfile COPY butuh file yang benar)
if [ ! -f "external/RandomY/CMakeLists.txt" ] || [ ! -f "external/FTXUI/CMakeLists.txt" ]; then
  warn "Submodules not initialized. Running git submodule update..."
  git submodule update --init --recursive
fi

# Build
info "Building Docker image: $IMAGE"
docker build $NO_CACHE -t "$IMAGE" .
ok "Image built: $IMAGE"

# Size info
SIZE=$(docker image inspect "$IMAGE" --format '{{.Size}}' 2>/dev/null | awk '{printf "%.1fMB", $1/1024/1024}')
info "Image size: $SIZE"

# Run
if [ "$RUN" -eq 1 ]; then
  # Stop old container if exists
  docker rm -f "$CONTAINER_NAME" 2>/dev/null || true

  info "Starting container: $CONTAINER_NAME"
  # FULL_MEM/LARGE_PAGES tidak di-set: miner memilih sendiri dari RAM & hugepages
  # yang tersedia di container. Override manual: FULL_MEM=1 ./docker-build.sh --run
  docker run --name "$CONTAINER_NAME" --rm \
    -e WALLET="${WALLET:-cb23d6d8557e776f5ff9ab6a7fb7f59a3d385245fa7a}" \
    -e POOL="${POOL:-sg.catchthatrabbit.com:8008}" \
    -e WORKER="${WORKER:-docker}" \
    -e THREADS="${THREADS:-}" \
    -e FULL_MEM="${FULL_MEM:-}" \
    -e LARGE_PAGES="${LARGE_PAGES:-}" \
    "$IMAGE"
fi

# Push
if [ "$PUSH" -eq 1 ]; then
  info "Tagging as $PUSH_IMAGE..."
  docker tag "$IMAGE" "$PUSH_IMAGE"

  info "Pushing to Docker Hub..."
  docker push "$PUSH_IMAGE"
  ok "Pushed: $PUSH_IMAGE"
fi

echo
echo -e "${GREEN}Done!${NC}"
echo "  docker run --rm $IMAGE                           # quick run (default wallet)"
echo "  docker run --rm -e WALLET=your_wallet $IMAGE     # your wallet"
echo "  docker run --rm -e FULL_MEM=1 -e THREADS=8 $IMAGE  # custom config"
