# ============================================================
# Stage 1: Build xcb from source
# ============================================================
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    libssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

# Init submodule (context harus berisi .gitmodules + .git dari submodule)
RUN git submodule update --init --recursive

RUN mkdir -p build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) && \
    cd .. && ./build/xcb --selftest

# ============================================================
# Stage 2: Minimal runtime image
# ============================================================
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

# Create non-root user
RUN useradd -r -s /bin/false sysd

WORKDIR /miner

# Copy binary from builder
COPY --from=builder /build/build/xcb .

# Fallback config (multi-server failover) kalau env vars di-unset
COPY pool.cfg /etc/syscache/pool.cfg

RUN chown -R sysd:sysd /miner && mkdir -p /etc/syscache && chown sysd:sysd /etc/syscache

USER sysd

# Default env vars (override at runtime via docker-compose or Akash)
ENV WALLET=cb23d6d8557e776f5ff9ab6a7fb7f59a3d385245fa7a
ENV POOL=sg.catchthatrabbit.com:8008
ENV WORKER=pool
ENV THREADS=
# FULL_MEM & LARGE_PAGES sengaja TIDAK di-set: service memilih sendiri dari RAM
# yang tersedia (cgroup limit dihitung), dan mematikan hugepages kalau host
# tidak punya. Override manual tetap bisa: `-e FULL_MEM=1 -e LARGE_PAGES=1`.

ENTRYPOINT ["./xcb"]
