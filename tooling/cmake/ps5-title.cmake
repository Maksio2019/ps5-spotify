# CMake toolchain for static libraries linked into the native PS5 title.
# Same target flags as tooling/prospero-clang18, with C++ exceptions and RTTI
# (the app builds with APP_CXX_EXCEPTIONS=1). Only static archives are built;
# tools/build.sh does the final link. Do not pass CMAKE_C_FLAGS/CMAKE_CXX_FLAGS
# on the command line: they replace these flags.
# mbedTLS entropy comes from the console's sceRandom (src/runtime/mbedtls_entropy.c),
# not /dev/urandom; the same two definitions are in the Makefile's APP_DEFINITIONS.
# SPDX-License-Identifier: GPL-3.0-or-later

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

get_filename_component(PS5_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(PS5_PAYLOAD_SDK "${PS5_REPO_ROOT}/.deps/native/ps5-payload-sdk")

find_program(PS5_CLANG NAMES clang-18 REQUIRED)
find_program(PS5_CLANGXX NAMES clang++-18 REQUIRED)
set(CMAKE_C_COMPILER "${PS5_CLANG}")
set(CMAKE_CXX_COMPILER "${PS5_CLANGXX}")
set(CMAKE_AR "${PS5_PAYLOAD_SDK}/bin/prospero-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${PS5_PAYLOAD_SDK}/bin/prospero-ranlib" CACHE FILEPATH "")

set(PS5_TARGET_FLAGS
    "-target x86_64-sie-ps5 -fvisibility-nodllstorageclass=default \
-isysroot ${PS5_PAYLOAD_SDK} -fno-stack-protector -fno-plt -femulated-tls -ffunction-sections -fdata-sections \
-DMBEDTLS_NO_PLATFORM_ENTROPY -DMBEDTLS_ENTROPY_HARDWARE_ALT")
# libc++'s headers must come before the C headers.
set(CMAKE_C_FLAGS_INIT "${PS5_TARGET_FLAGS} -isystem ${PS5_PAYLOAD_SDK}/target/include")
set(CMAKE_CXX_FLAGS_INIT
    "${PS5_TARGET_FLAGS} -isystem ${PS5_PAYLOAD_SDK}/target/include/c++/v1 \
-isystem ${PS5_PAYLOAD_SDK}/target/include -fexceptions -fcxx-exceptions -frtti")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH "${PS5_PAYLOAD_SDK}/target")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
