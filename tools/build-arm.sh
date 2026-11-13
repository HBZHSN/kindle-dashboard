#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${KINDLE_BUILD_DIR:-/var/tmp/kindle-dashboard-arm}
export ARM_TOOLCHAIN="$WORK/arm-linux-musleabi-cross"
export ARM_DEPS="$WORK/deps"
export PATH="$ARM_TOOLCHAIN/bin:$PATH"
export PKG_CONFIG_LIBDIR="$ARM_DEPS/lib/pkgconfig"
export CC=arm-linux-musleabi-gcc CXX=arm-linux-musleabi-g++
export CFLAGS='-Os -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp -ffunction-sections -fdata-sections'
export CXXFLAGS="$CFLAGS"
mkdir -p "$WORK/src" "$ARM_DEPS"
if [ ! -x "$ARM_TOOLCHAIN/bin/$CC" ]; then
    tar -xzf "$ROOT/.build/arm-linux-musleabi-cross.tgz" -C "$WORK"
fi
extract() {
    if [ ! -d "$WORK/src/$1" ]; then
        mkdir -p "$WORK/src/$1"
        tar -xf "$ROOT/.build/$2" -C "$WORK/src/$1" --strip-components=1
    fi
}
build() {
    local name=$1; shift
    cmake -S "$WORK/src/$name" -B "$WORK/build-$name" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/kindle-musl.cmake" \
        -DCMAKE_INSTALL_PREFIX="$ARM_DEPS" -DCMAKE_BUILD_TYPE=MinSizeRel \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF "$@"
    cmake --build "$WORK/build-$name" -j 6
    cmake --install "$WORK/build-$name"
}
extract zlib zlib-1.3.1.tar.gz
build zlib -DZLIB_BUILD_EXAMPLES=OFF
extract png libpng-headers.tar.gz
build png -DPNG_SHARED=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_ARM_NEON=off
extract freetype freetype-headers.tar.gz
build freetype -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON
extract harfbuzz harfbuzz-headers.tar.gz
build harfbuzz -DHB_HAVE_FREETYPE=ON -DHB_HAVE_GLIB=OFF -DHB_HAVE_ICU=OFF -DHB_HAVE_GOBJECT=OFF \
    -DHB_BUILD_UTILS=OFF -DHB_BUILD_SUBSET=OFF
extract yaml libyaml-headers.tar.gz
build yaml -DYAML_BUILD_SHARED_LIBS=OFF -DYAML_BUILD_TESTS=OFF
extract openssl openssl-3.5.8.tar.gz
if [ ! -f "$ARM_DEPS/lib/libssl.a" ]; then
    cd "$WORK/src/openssl"
    ./Configure linux-armv4 --prefix="$ARM_DEPS" --libdir=lib no-shared no-tests no-module no-dso \
        no-legacy no-comp no-quic no-ssl3 no-weak-ssl-ciphers
    make -j 6 build_libs
    make install_dev
fi
extract curl curl-8.22.0.tar.xz
build curl -DBUILD_CURL_EXE=OFF -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF \
    -DCURL_USE_OPENSSL=ON -DOPENSSL_USE_STATIC_LIBS=ON -DOPENSSL_ROOT_DIR="$ARM_DEPS" \
    -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
    -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
    -DHTTP_ONLY=ON -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON \
    -DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt
mkdir -p "$ARM_DEPS/include/nlohmann"
cp "$ROOT/.build/include/nlohmann/json.hpp" "$ARM_DEPS/include/nlohmann/"
cmake -S "$ROOT" -B "$ROOT/.build/kindle-static" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/kindle-musl.cmake" -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_PREFIX_PATH="$ARM_DEPS" -DOPENSSL_ROOT_DIR="$ARM_DEPS" -DOPENSSL_USE_STATIC_LIBS=ON \
    -DZLIB_LIBRARY_RELEASE="$ARM_DEPS/lib/libz.a" \
    -DDASHBOARD_STATIC=ON -DDASHBOARD_WINDOW=OFF -DBUILD_TESTING=OFF
cmake --build "$ROOT/.build/kindle-static" -j 4
arm-linux-musleabi-strip "$ROOT/.build/kindle-static/kindle-dashboard"
file "$ROOT/.build/kindle-static/kindle-dashboard"
