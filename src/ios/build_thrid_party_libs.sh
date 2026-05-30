#!/bin/bash
#
# Cross-compiles the WebM decode stack (libvpx, libwebm, opus) as static libraries
# for iOS, into per-SDK folders (a single .a cannot hold both arm64-device and
# arm64-simulator, so device and simulator slices live separately):
#
#   src/ios/third_party_libs/device/{libvpx,libwebm,libopus}.a   (arm64,        iphoneos)
#   src/ios/third_party_libs/sim/{libvpx,libwebm,libopus}.a      (arm64+x86_64, iphonesimulator)
#
# The Xcode project links the matching folder per SDK via LIBRARY_SEARCH_PATHS.
#
# NOTE: requires submodules checked out:  git submodule update --init --recursive
#
set -o errexit

cd "$(dirname "$0")"
current_dir="$(pwd)"
third_party="${current_dir}/../../third_party"
libvpx_dir="${third_party}/libvpx"
libwebm_dir="${third_party}/libwebm"
opus_dir="${third_party}/opus"
toolchain="${current_dir}/ios.toolchain.cmake"

libs_dir="${current_dir}/third_party_libs"
device_dir="${libs_dir}/device"
sim_dir="${libs_dir}/sim"
rm -rf "${libs_dir}"
mkdir -p "${device_dir}" "${sim_dir}"

CPU_CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
IOS_MIN=12.0
echo "CPU_CORES: ${CPU_CORES}"

# ---------------------------------------------------------------------------
# CMake projects (opus, libwebm): build per IOS_PLATFORM, then lipo the two
# simulator arches into one fat lib.
# $1 = project dir, $2 = lib name (libopus.a / libwebm.a), $3.. = extra cmake args
# ---------------------------------------------------------------------------
# Builds one IOS_PLATFORM into ${proj}/build_ios_<platform>/ (lib lands at its root).
cmake_build_one() {
    local proj="$1" platform="$2"; shift 2
    local bdir="${proj}/build_ios_${platform}"
    rm -rf "${bdir}"; mkdir -p "${bdir}"
    cmake -S "${proj}" -B "${bdir}" -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
        -DIOS_PLATFORM="${platform}" \
        -DIOS_DEPLOYMENT_TARGET="${IOS_MIN}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        "$@"
    cmake --build "${bdir}" -j"${CPU_CORES}"
}

build_cmake_lib() {
    local proj="$1"; shift
    local lib="$1"; shift   # remaining args are extra cmake flags
    cmake_build_one "${proj}" OS64 "$@"
    cp "${proj}/build_ios_OS64/${lib}" "${device_dir}/${lib}"
    cmake_build_one "${proj}" SIMULATORARM64 "$@"
    cmake_build_one "${proj}" SIMULATOR64 "$@"
    lipo -create "${proj}/build_ios_SIMULATORARM64/${lib}" \
                 "${proj}/build_ios_SIMULATOR64/${lib}" \
                 -output "${sim_dir}/${lib}"
}

build_opus() {
    # Disable intrinsics so opus skips arch-specific runtime CPU detection,
    # which fails under cross-compilation (NEON detection on arm64).
    build_cmake_lib "${opus_dir}" libopus.a -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_DISABLE_INTRINSICS=ON
}

build_libwebm() {
    build_cmake_lib "${libwebm_dir}" libwebm.a \
        -DENABLE_WEBMTS=OFF -DENABLE_WEBMINFO=OFF \
        -DENABLE_TESTS=OFF -DENABLE_SAMPLE_PROGRAMS=OFF
}

# ---------------------------------------------------------------------------
# libvpx: each iOS target self-configures its sysroot inside libvpx's configure
# (arm*-darwin-* -> iphoneos SDK; *-iphonesimulator-* -> simulator SDK), so we
# must NOT pass our own -isysroot/-miphoneos flags (doing so breaks the link
# test). We only force -arch for the x86_64 simulator slice (host is arm64).
# $1 vpx-target  $2 output .a path  $3.. extra configure args
# ---------------------------------------------------------------------------
build_vpx() {
    local target="$1" out="$2"; shift 2
    local bdir="${libvpx_dir}/build_ios_${target}"
    rm -rf "${bdir}"; mkdir -p "${bdir}"
    pushd "${bdir}" > /dev/null
    "${libvpx_dir}/configure" \
        --target="${target}" \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests \
        --disable-webm-io --disable-libyuv --disable-postproc \
        --enable-vp8 --enable-vp9 \
        --disable-vp8-encoder --disable-vp9-encoder \
        --enable-static --disable-shared --enable-pic \
        "$@"
    make -j"${CPU_CORES}"
    popd > /dev/null
    cp "${bdir}/libvpx.a" "${out}"
}

build_libvpx() {
    # libvpx only ships x86*-iphonesimulator targets; register an arm64 one so
    # the existing *-iphonesimulator-* configure branch wires up the sim sysroot.
    if ! grep -q "arm64-iphonesimulator-gcc" "${libvpx_dir}/configure"; then
        sed -i '' 's#\(all_platforms="${all_platforms} x86_64-iphonesimulator-gcc"\)#\1\n    all_platforms="${all_platforms} arm64-iphonesimulator-gcc"#' "${libvpx_dir}/configure"
    fi
    build_vpx arm64-darwin-gcc           "${device_dir}/libvpx.a"
    build_vpx arm64-iphonesimulator-gcc  /tmp/libvpx_sim_arm64.a
    build_vpx x86_64-iphonesimulator-gcc /tmp/libvpx_sim_x86_64.a \
        --extra-cflags="-arch x86_64" --extra-cxxflags="-arch x86_64"
    lipo -create /tmp/libvpx_sim_arm64.a /tmp/libvpx_sim_x86_64.a -output "${sim_dir}/libvpx.a"
    rm -f /tmp/libvpx_sim_arm64.a /tmp/libvpx_sim_x86_64.a
}

build_opus
build_libwebm
build_libvpx

echo "=== device (expect arm64) ==="
lipo -info "${device_dir}"/*.a
echo "=== sim (expect x86_64 arm64) ==="
lipo -info "${sim_dir}"/*.a
echo "Build third party libs done."
