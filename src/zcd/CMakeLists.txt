# ============================================================
# Miner Zycord (ZCD) — target terpisah dari xcb.
#
# Biner `zcd-miner` hanya terdiri dari src/zcd/*, src/Hashing/RandomXV2.cpp,
# src/Log.cpp dan external/randomx-v2. Tidak ada backend CONN (src/ConnMiner.*,
# src/ConnPool.*, src/ConnRpc.*) maupun Core Coin (src/StratumClient.*,
# src/Miner.*, external/RandomY) yang ikut di-link atau di-include.
# ============================================================

# Path absolut terhadap root proyek: file ini berada di subdirektori, jadi
# "src/..." akan diresolusi terhadap src/zcd/ dan tidak ditemukan.
set(ZCD_ROOT ${CMAKE_SOURCE_DIR})
set(ZCD_SOURCES
    ${ZCD_ROOT}/src/zcd/ZcdMain.cpp
    ${ZCD_ROOT}/src/zcd/ZcdMiner.cpp
    ${ZCD_ROOT}/src/zcd/ZcdPool.cpp
    ${ZCD_ROOT}/src/zcd/ZcdJob.cpp
    ${ZCD_ROOT}/src/zcd/ZcdCommitment.cpp
    ${ZCD_ROOT}/src/zcd/ZcdJson.cpp
    ${ZCD_ROOT}/src/Hashing/RandomXV2.cpp
    ${ZCD_ROOT}/src/Log.cpp
)

set(ZCD_HEADERS
    ${ZCD_ROOT}/src/zcd/ZcdMiner.hpp
    ${ZCD_ROOT}/src/zcd/ZcdPool.hpp
    ${ZCD_ROOT}/src/zcd/ZcdJob.hpp
    ${ZCD_ROOT}/src/zcd/ZcdCommitment.hpp
    ${ZCD_ROOT}/src/zcd/ZcdJson.hpp
    ${ZCD_ROOT}/src/Hashing/RandomXV2.hpp
    ${ZCD_ROOT}/src/Log.hpp
)

add_executable(zcd-miner ${ZCD_SOURCES} ${ZCD_HEADERS})
target_include_directories(zcd-miner PRIVATE ${ZCD_ROOT}/src)

target_link_libraries(zcd-miner
    PRIVATE
        randomx_v2            # RandomX V2 — simbol rxv2_ (randomx_calculate_commitment)
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    if(PORTABLE)
        target_compile_options(zcd-miner PRIVATE -march=x86-64-v2)
    else()
        target_compile_options(zcd-miner PRIVATE -march=x86-64-v3)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_options(zcd-miner PRIVATE -march=armv8-a+crypto)
endif()

target_compile_options(zcd-miner PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
target_link_options(zcd-miner PRIVATE $<$<NOT:$<CONFIG:Debug>>:-s>)

# Runtime butuh librandomx_v2.so di samping biner (RPATH $ORIGIN).
# Tanpa INSTALL_RPATH, `cmake --install` menghapus RUNPATH build dan binary
# hasil install tidak menemukan library-nya (ldd: not found). xcb memakai
# setelan yang sama.
set_target_properties(zcd-miner PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)
add_custom_command(TARGET zcd-miner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:randomx_v2> $<TARGET_FILE_DIR:zcd-miner>
    COMMENT "Copy librandomx_v2.so ke samping zcd-miner"
)

install(TARGETS zcd-miner DESTINATION bin)
# Library ikut di-install ke direktori yang sama supaya RPATH $ORIGIN berlaku.
install(TARGETS randomx_v2 LIBRARY DESTINATION bin)

if(BUILD_TESTING)
    add_test(NAME zcd-selftest COMMAND zcd-miner --selftest)

    # Aturan share: commitment blake2b(blob||digest), limb 24..31, batas <.
    add_executable(zcd-commitment-test
        ${ZCD_ROOT}/tests/zcd_commitment_test.cpp
        ${ZCD_ROOT}/src/zcd/ZcdCommitment.cpp)
    target_include_directories(zcd-commitment-test PRIVATE ${ZCD_ROOT}/src)
    target_link_libraries(zcd-commitment-test PRIVATE randomx_v2)
    add_test(NAME zcd-commitment COMMAND zcd-commitment-test)

    # Parser job: blob 43 byte, target 8 byte LE, seed_hash 64 hex.
    add_executable(zcd-job-test
        ${ZCD_ROOT}/tests/zcd_job_test.cpp
        ${ZCD_ROOT}/src/zcd/ZcdJob.cpp
        ${ZCD_ROOT}/src/zcd/ZcdJson.cpp)
    target_include_directories(zcd-job-test PRIVATE ${ZCD_ROOT}/src)
    add_test(NAME zcd-job COMMAND zcd-job-test)

    # Regresi temuan review PR #29: parser host/port, validasi algo & height.
    add_executable(zcd-review-test
        ${ZCD_ROOT}/tests/zcd_review_test.cpp
        ${ZCD_ROOT}/src/zcd/ZcdJob.cpp
        ${ZCD_ROOT}/src/zcd/ZcdJson.cpp)
    target_include_directories(zcd-review-test PRIVATE ${ZCD_ROOT}/src)
    add_test(NAME zcd-review COMMAND zcd-review-test)
endif()
