#!/usr/bin/env bash
# Builds mbedTLS and cspot (with bell) as static archives for the PS5 title.
# Output: build/ps5-deps/prefix (mbedTLS) and build/ps5-deps/cspot-build
# (libcspot.a, bell/libbell.a, generated protobuf headers).
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
toolchain="$root/tooling/cmake/ps5-title.cmake"
deps="$root/build/ps5-deps"
prefix="$deps/prefix"
cspot_src="$root/third_party/cspot/cspot"

mbedtls_version=3.6.5
mbedtls_sha256=4a11f1777bb95bf4ad96721cac945a26e04bf19f57d905f241fe77ebeddf46d8
mbedtls_archive="$root/.deps/src/mbedtls-$mbedtls_version.tar.bz2"
mbedtls_src="$root/.deps/src/mbedtls-$mbedtls_version"

# nanopb's generator needs Python with protobuf<5 and grpcio-tools; Python
# 3.14 is too new for it. PYTHON_VENV points at a suitable virtualenv.
python_venv=${PYTHON_VENV:-$HOME/pyenv}
[[ -x $python_venv/bin/python3 ]] || {
    echo "nanopb needs a Python 3.12 venv with protobuf<5 (set PYTHON_VENV)" >&2
    exit 2
}
export PATH="$python_venv/bin:$PATH"

[[ -f $cspot_src/CMakeLists.txt ]] || {
    echo "cspot submodule missing: git submodule update --init --recursive" >&2
    exit 2
}

if [[ ! -f $mbedtls_src/CMakeLists.txt ]]; then
    mkdir -p "$root/.deps/src"
    if [[ ! -f $mbedtls_archive ]]; then
        echo "==> [cspot] Downloading mbedTLS $mbedtls_version" >&2
        curl -fsSL --retry 3 -o "$mbedtls_archive.tmp" \
            "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$mbedtls_version/mbedtls-$mbedtls_version.tar.bz2"
        mv -- "$mbedtls_archive.tmp" "$mbedtls_archive"
    fi
    sha256sum --check --status <<<"$mbedtls_sha256  $mbedtls_archive" || {
        echo "mbedTLS archive checksum mismatch" >&2
        exit 2
    }
    tar -xjf "$mbedtls_archive" -C "$root/.deps/src"
fi

if [[ ! -f $prefix/lib/libmbedtls.a || ! -f $prefix/lib/cmake/MbedTLS/MbedTLSConfig.cmake ]]; then
    echo "==> [cspot] Building mbedTLS $mbedtls_version" >&2
    cmake -S "$mbedtls_src" -B "$deps/mbedtls-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
        -DMBEDTLS_FATAL_WARNINGS=OFF \
        >/dev/null
    cmake --build "$deps/mbedtls-build" >/dev/null
    cmake --install "$deps/mbedtls-build" >/dev/null
fi

if [[ ! -f $deps/cspot-build/build.ninja ]]; then
    echo "==> [cspot] Configuring cspot" >&2
    # Spotify streams Ogg Vorbis, which cspot decodes with tremor directly, so
    # bell's codec wrappers, sinks, MQTT and Avahi are left out.
    cmake -S "$cspot_src" -B "$deps/cspot-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DBELL_DISABLE_CODECS=ON -DBELL_CODEC_AAC=OFF -DBELL_CODEC_MP3=OFF \
        -DBELL_CODEC_OPUS=OFF -DBELL_CODEC_ALAC=OFF -DBELL_CODEC_VORBIS=ON \
        -DBELL_DISABLE_MQTT=ON -DBELL_DISABLE_SINKS=ON -DBELL_DISABLE_AVAHI=ON \
        -DBELL_EXTERNAL_MBEDTLS="$prefix/lib/cmake/MbedTLS" -DMBEDTLS_RELEASE=RELEASE \
        -DCMAKE_FIND_ROOT_PATH="$root/.deps/native/ps5-payload-sdk/target;$prefix" \
        >/dev/null 2>&1
fi
echo "==> [cspot] Building cspot" >&2
cmake --build "$deps/cspot-build" 2>&1 | grep -v -E 'pkg_resources|warning:|^ ' || true
[[ -f $deps/cspot-build/libcspot.a && -f $deps/cspot-build/bell/libbell.a ]] || {
    echo "cspot build failed" >&2
    exit 1
}
