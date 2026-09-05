cmake_minimum_required(VERSION 3.10)
option(NO_LTO "Disable LTO (musl alpine workaround)" OFF)
option(ENABLE_SELF_DEFENSE "Aktifkan proteksi diri: anti-debugging (deteksi ptrace, dumpable=0, ptrace lock) + title spoof" OFF)

project(xcb VERSION 1.0.0 LANGUAGES CXX C ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release hanya sebagai default — jangan menimpa -DCMAKE_BUILD_TYPE dari CLI,
# kalau ditimpa maka ./setup.sh --debug dan ./build.sh --debug tidak berefek.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ============================================================
# LTO: satu keputusan, dipakai konsisten compile + link
# ============================================================
# musl (Alpine) gagal build dengan -flto + _FORTIFY_SOURCE bawaan:
#   "inlining failed in call to 'always_inline' 'vsnprintf'" -> lto-wrapper fatal.
# Deteksi di sini, bukan di setup.sh, supaya build via Dockerfile atau cmake
# manual ikut terlindungi.
execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
    OUTPUT_VARIABLE MINER_TARGET_TRIPLE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(MINER_TARGET_TRIPLE MATCHES "musl")
    message(STATUS "Target musl (${MINER_TARGET_TRIPLE}): LTO dimatikan")
    set(NO_LTO ON)
endif()

if(NO_LTO)
    set(MINER_LTO_FLAGS "")
else()
    set(MINER_LTO_FLAGS -flto)
endif()

# ============================================================
# RandomY library (dari submodule)
# ============================================================
add_subdirectory(external/RandomY EXCLUDE_FROM_ALL)

# Tambahkan include path RandomY
include_directories(external/RandomY/src)

# ============================================================
# FTXUI - terminal TUI library (dashboard, dari submodule)
# ============================================================
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_MODULES OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
add_subdirectory(external/FTXUI)

# ============================================================
# Cari dependency
# ============================================================
# Prefer -pthread over -lpthread: pada glibc >= 2.34 pthread sudah di libc dan
# CMAKE_THREAD_LIBS_INIT kosong, tapi musl/toolchain lain masih butuh flagnya.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# ============================================================
# Source files xcb
# ============================================================
set(MINER_SOURCES
    src/main.cpp
    src/StratumClient.cpp
    src/Miner.cpp
    src/Config.cpp
    src/Log.cpp
    src/Stats.cpp
    src/Submitter.cpp
    src/Sha3_512.cpp
    src/Dashboard.cpp
)

set(MINER_HEADERS
    src/StratumClient.hpp
    src/Miner.hpp
    src/Config.hpp
    src/Log.hpp
    src/Stats.hpp
    src/Submitter.hpp
    src/Sha3_512.hpp
    src/Dashboard.hpp
)

# ============================================================
# Self-defense (opsional, OFF default): anti-debugging + title spoof
# ============================================================
if(ENABLE_SELF_DEFENSE)
    list(APPEND MINER_SOURCES src/SelfDefense.cpp)
    list(APPEND MINER_HEADERS src/SelfDefense.hpp)
    message(STATUS "ENABLE_SELF_DEFENSE=ON: anti-debugging aktif")
endif()

# ============================================================
# Executable
# ============================================================
add_executable(xcb ${MINER_SOURCES} ${MINER_HEADERS})

target_link_libraries(xcb
    PRIVATE
        randomx               # RandomY library
        ftxui::ftxui          # FTXUI dashboard (TUI)
        Threads::Threads      # find_package(Threads) di atas — bukan -lpthread literal
        OpenSSL::SSL
        OpenSSL::Crypto
)

# Optimasi portabel per arsitektur CPU
# PORTABLE=ON menurunkan baseline x86 ke x86-64-v2 (tanpa AVX2) untuk image yang
# didistribusikan ke host CPU tak diketahui (runner CI, node Akash pre-Haswell).
option(PORTABLE "Turunkan baseline x86 ke x86-64-v2 (tanpa AVX2)" OFF)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    if(PORTABLE)
        set(MINER_ARCH_FLAGS -march=x86-64-v2)
    else()
        set(MINER_ARCH_FLAGS -march=x86-64-v3)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(MINER_ARCH_FLAGS -march=armv8-a+crypto)
else()
    set(MINER_ARCH_FLAGS "")
endif()

# -O3 dan LTO hanya untuk build non-Debug — kalau tidak, --debug tetap
# menghasilkan binary teroptimasi penuh yang sulit di-debug.
target_compile_options(xcb PRIVATE
    ${MINER_ARCH_FLAGS}
    $<$<NOT:$<CONFIG:Debug>>:-O3>
    $<$<NOT:$<CONFIG:Debug>>:${MINER_LTO_FLAGS}>
)
# -s (strip) juga hanya non-Debug, supaya simbol debug tidak dibuang.
target_link_options(xcb PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:${MINER_LTO_FLAGS}>
    $<$<NOT:$<CONFIG:Debug>>:-s>
)

install(TARGETS xcb DESTINATION bin)

if(ENABLE_SELF_DEFENSE)
    target_compile_definitions(xcb PRIVATE XCB_SELF_DEFENSE)
endif()
