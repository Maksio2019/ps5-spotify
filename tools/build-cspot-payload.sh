#!/usr/bin/env bash
# Builds mbedTLS and cspot (with bell) as static archives for the Spotify payload
# (payload SDK toolchain, not the title one). Same sources and options as
# tools/build-cspot.sh. mbedTLS uses its normal platform entropy (/dev/urandom):
# payloads are not sandboxed.
# Output: build/payload-deps/prefix (mbedTLS) and build/payload-deps/cspot-build
# (libcspot.a, bell/libbell.a, generated protobuf headers).
# With this toolchain bell's config-mode lookup does not put the mbedTLS include
# directory on its targets, so it is added as a standard include directory.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"
toolchain="$sdk/toolchain/prospero.cmake"
deps="$root/build/payload-deps"
prefix="$deps/prefix"
cspot_src="$root/third_party/cspot/cspot"
mbedtls_version=3.6.5
mbedtls_src="$root/.deps/src/mbedtls-$mbedtls_version"

python_venv=${PYTHON_VENV:-$HOME/pyenv}
[[ -x $python_venv/bin/python3 ]] || {
    echo "nanopb needs a Python 3.12 venv with protobuf<5 (set PYTHON_VENV)" >&2
    exit 2
}
export PATH="$python_venv/bin:$PATH"

# tools/build-cspot.sh downloads and checks the mbedTLS source.
[[ -f $mbedtls_src/CMakeLists.txt ]] || bash "$root/tools/build-cspot.sh"

if [[ ! -f $prefix/lib/libmbedtls.a || ! -f $prefix/lib/cmake/MbedTLS/MbedTLSConfig.cmake ]]; then
    echo "==> [cspot-payload] Building mbedTLS $mbedtls_version" >&2
    cmake -S "$mbedtls_src" -B "$deps/mbedtls-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
        -DMBEDTLS_FATAL_WARNINGS=OFF \
        >/dev/null
    cmake --build "$deps/mbedtls-build" >/dev/null
    DESTDIR= cmake --install "$deps/mbedtls-build" >/dev/null
fi

if [[ ! -f $deps/cspot-build/build.ninja ]]; then
    echo "==> [cspot-payload] Configuring cspot" >&2
    cmake -S "$cspot_src" -B "$deps/cspot-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_VERBOSE_MAKEFILE=OFF \
        -DBELL_DISABLE_CODECS=ON -DBELL_CODEC_AAC=OFF -DBELL_CODEC_MP3=OFF \
        -DBELL_CODEC_OPUS=OFF -DBELL_CODEC_ALAC=OFF -DBELL_CODEC_VORBIS=ON \
        -DBELL_DISABLE_MQTT=ON -DBELL_DISABLE_SINKS=ON -DBELL_DISABLE_AVAHI=ON \
        -DBELL_EXTERNAL_MBEDTLS="$prefix/lib/cmake/MbedTLS" -DMBEDTLS_RELEASE=RELEASE \
        -DCMAKE_FIND_ROOT_PATH="$sdk/target;$prefix" \
        -DCMAKE_C_STANDARD_INCLUDE_DIRECTORIES="$prefix/include" \
        -DCMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES="$prefix/include" \
        >"$deps/cspot-configure.log" 2>&1
fi
echo "==> [cspot-payload] Building cspot" >&2
cmake --build "$deps/cspot-build" >"$deps/cspot-build.log" 2>&1 || {
    grep -E "error|Error" "$deps/cspot-build.log" | head -30 >&2
    echo "cspot payload build failed (see $deps/cspot-build.log)" >&2
    exit 1
}
[[ -f $deps/cspot-build/libcspot.a && -f $deps/cspot-build/bell/libbell.a ]] || {
    echo "cspot payload build produced no archives" >&2
    exit 1
}
echo "==> [cspot-payload] OK: $deps/cspot-build/libcspot.a" >&2
