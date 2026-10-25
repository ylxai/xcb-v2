# yespower upstream (openwall/yespower, tag YESPOWER_1_0_0).
# yespower-opt.c sudah #include yespower-platform.c — jadi platform tidak
# dikompilasi terpisah (begitu pula Makefile upstream: yespower-opt.o
# bergantung pada yespower-platform.c).
add_library(yespower STATIC
    yespower-opt.c
    sha256.c
)

target_include_directories(yespower PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Flag arsip yang sama dengan target xcb supaya jalur AVX/SSE2 di
# yespower-opt.c aktif sesuai baseline CPU (x86-64-v3 default, v2 dengan
# PORTABLE=ON, armv8+crypto di aarch64).
get_property(YP_LANG GLOBAL PROPERTY ENABLED_LANGUAGES)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    if(PORTABLE)
        target_compile_options(yespower PRIVATE -march=x86-64-v2)
    else()
        target_compile_options(yespower PRIVATE -march=x86-64-v3)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    target_compile_options(yespower PRIVATE -march=armv8-a+crypto)
endif()
