#!/bin/bash
#
# Builds the WebM decode stack as universal (arm64 + x86_64) static libraries
# for the macOS simulator plugin: libvpx, libwebm, opus.
#
# Output: src/mac/third_party_libs/{libvpx.a,libwebm.a,libopus.a}
# The Xcode project links against these.
#
# NOTE: Requires submodules to be checked out first:
#   git submodule update --init --recursive
#
set -o errexit

cd "$(dirname "$0")"
current_dir="$(pwd)"
third_party="${current_dir}/../../third_party"
libvpx_dir="${third_party}/libvpx"
libwebm_dir="${third_party}/libwebm"
opus_dir="${third_party}/opus"

libs_dir="${current_dir}/third_party_libs"
mkdir -p "${libs_dir}"

CPU_CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo "CPU_CORES: ${CPU_CORES}"

MACOS_MIN=10.13

# ---------------------------------------------------------------------------
# opus (universal via CMake)
# ---------------------------------------------------------------------------
build_opus() {
    local build_dir="${opus_dir}/build_mac"
    rm -rf "${build_dir}"
    mkdir -p "${build_dir}"
    cmake -S "${opus_dir}" -B "${build_dir}" \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOS_MIN} \
        -DBUILD_SHARED_LIBS=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "${build_dir}" -j"${CPU_CORES}"
    cp "${build_dir}/libopus.a" "${libs_dir}/libopus.a"
}

# ---------------------------------------------------------------------------
# libwebm (universal via CMake; we only need the mkvparser static lib)
# ---------------------------------------------------------------------------
build_libwebm() {
    local build_dir="${libwebm_dir}/build_mac"
    rm -rf "${build_dir}"
    mkdir -p "${build_dir}"
    cmake -S "${libwebm_dir}" -B "${build_dir}" \
        -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOS_MIN} \
        -DENABLE_WEBMTS=OFF \
        -DENABLE_WEBMINFO=OFF \
        -DENABLE_TESTS=OFF \
        -DENABLE_SAMPLE_PROGRAMS=OFF \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build "${build_dir}" -j"${CPU_CORES}"
    cp "${build_dir}/libwebm.a" "${libs_dir}/libwebm.a"
}

# ---------------------------------------------------------------------------
# libvpx (per-arch via configure, then lipo into a universal lib)
# ---------------------------------------------------------------------------
build_vpx_arch() {
    local arch="$1"       # arm64 | x86_64
    local target="$2"     # arm64-darwin20-gcc | x86_64-darwin16-gcc
    local build_dir="${libvpx_dir}/build_mac_${arch}"
    rm -rf "${build_dir}"
    mkdir -p "${build_dir}"
    pushd "${build_dir}" > /dev/null
    "${libvpx_dir}/configure" \
        --target="${target}" \
        --disable-examples \
        --disable-tools \
        --disable-docs \
        --disable-unit-tests \
        --disable-webm-io \
        --disable-libyuv \
        --disable-postproc \
        --enable-vp8 \
        --enable-vp9 \
        --disable-vp8-encoder \
        --disable-vp9-encoder \
        --enable-pic
    make -j"${CPU_CORES}"
    popd > /dev/null
}

build_libvpx() {
    build_vpx_arch arm64 arm64-darwin20-gcc
    build_vpx_arch x86_64 x86_64-darwin16-gcc
    lipo -create \
        "${libvpx_dir}/build_mac_arm64/libvpx.a" \
        "${libvpx_dir}/build_mac_x86_64/libvpx.a" \
        -output "${libs_dir}/libvpx.a"
}

build_opus
build_libwebm
build_libvpx

ls -la "${libs_dir}"
echo "Build third party libs done."
