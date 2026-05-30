#!/bin/bash
#
# Cross-compiles the WebM decode stack (libvpx, libwebm, opus) as static libs
# for tvOS, into per-SDK folders (one .a can't hold both arm64-device and
# arm64-simulator):
#
#   src/tvos/third_party_libs/device/{libvpx,libwebm,libopus}.a   (arm64,        appletvos)
#   src/tvos/third_party_libs/sim/{libvpx,libwebm,libopus}.a      (arm64+x86_64, appletvsimulator)
#
# opus/libwebm use CMake's built-in tvOS support; libvpx has no tvOS target, so
# we drive its generic darwin targets with explicit clang -target triples +
# the matching SDK sysroot.
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

libs_dir="${current_dir}/third_party_libs"
device_dir="${libs_dir}/device"
sim_dir="${libs_dir}/sim"
rm -rf "${libs_dir}"
mkdir -p "${device_dir}" "${sim_dir}"

CPU_CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
TVOS_MIN=12.0
echo "CPU_CORES: ${CPU_CORES}"

DEV_SDK=$(xcrun --sdk appletvos --show-sdk-path)
SIM_SDK=$(xcrun --sdk appletvsimulator --show-sdk-path)

# ---------------------------------------------------------------------------
# opus / libwebm via CMake (built-in tvOS cross-compile). Simulator is built
# fat (arm64+x86_64) in one shot via CMAKE_OSX_ARCHITECTURES.
# $1 proj  $2 lib  $3.. extra cmake flags
# ---------------------------------------------------------------------------
build_cmake_lib() {
    local proj="$1"; shift
    local lib="$1"; shift
    _one() {
        local tag="$1" sysroot="$2" archs="$3"
        local bdir="${proj}/build_tvos_${tag}"
        rm -rf "${bdir}"; mkdir -p "${bdir}"
        cmake -S "${proj}" -B "${bdir}" -G "Unix Makefiles" \
            -DCMAKE_SYSTEM_NAME=tvOS \
            -DCMAKE_OSX_SYSROOT="${sysroot}" \
            -DCMAKE_OSX_ARCHITECTURES="${archs}" \
            -DCMAKE_OSX_DEPLOYMENT_TARGET="${TVOS_MIN}" \
            -DBUILD_SHARED_LIBS=OFF \
            -DCMAKE_BUILD_TYPE=Release \
            "$@"
        cmake --build "${bdir}" -j"${CPU_CORES}"
    }
    _one device appletvos "arm64"
    cp "${proj}/build_tvos_device/${lib}" "${device_dir}/${lib}"
    _one sim appletvsimulator "arm64;x86_64"
    cp "${proj}/build_tvos_sim/${lib}" "${sim_dir}/${lib}"
}

build_opus() {
    build_cmake_lib "${opus_dir}" libopus.a -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_DISABLE_INTRINSICS=ON
}
build_libwebm() {
    build_cmake_lib "${libwebm_dir}" libwebm.a \
        -DENABLE_WEBMTS=OFF -DENABLE_WEBMINFO=OFF -DENABLE_TESTS=OFF -DENABLE_SAMPLE_PROGRAMS=OFF
}

# ---------------------------------------------------------------------------
# libvpx: no tvOS target. Use a numbered macOS target (arm64-darwin23) so the
# arm64/x86 ISA + SIMD are selected (the *-darwin-gcc target would auto-inject
# iphoneos flags). libvpx applies --extra-cflags only to compile, not link, so
# a -target/-isysroot there causes a compile/link platform mismatch. Instead we
# wrap the compiler to APPEND the tvOS -target/-isysroot after every arg list
# (winning over libvpx's auto macOS sysroot) for both compile and link.
# $1 vpx-target  $2 clang -target triple  $3 sysroot  $4 asmode(clang|nasm)  $5 out .a
# ---------------------------------------------------------------------------
build_vpx() {
    local target="$1" triple="$2" sysroot="$3" asmode="$4" out="$5"
    local bdir="${libvpx_dir}/build_tvos_$(basename "${out%.a}")_${triple}"
    rm -rf "${bdir}"; mkdir -p "${bdir}"
    local clang clangxx; clang="$(xcrun -f clang)"; clangxx="$(xcrun -f clang++)"
    local wrap="${bdir}/wrap"; mkdir -p "${wrap}"
    cat > "${wrap}/cc" <<EOF
#!/bin/sh
exec "${clang}" "\$@" -target ${triple} -isysroot ${sysroot}
EOF
    cat > "${wrap}/cxx" <<EOF
#!/bin/sh
exec "${clangxx}" "\$@" -target ${triple} -isysroot ${sysroot}
EOF
    chmod +x "${wrap}/cc" "${wrap}/cxx"
    local as_tool="${wrap}/cc"
    [ "${asmode}" = "nasm" ] && as_tool="$(command -v nasm)"
    pushd "${bdir}" > /dev/null
    CC="${wrap}/cc" CXX="${wrap}/cxx" LD="${wrap}/cc" AS="${as_tool}" \
    "${libvpx_dir}/configure" \
        --target="${target}" \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests \
        --disable-webm-io --disable-libyuv --disable-postproc \
        --enable-vp8 --enable-vp9 \
        --disable-vp8-encoder --disable-vp9-encoder \
        --enable-static --disable-shared --enable-pic
    make -j"${CPU_CORES}"
    popd > /dev/null
    cp "${bdir}/libvpx.a" "${out}"
}

build_libvpx() {
    build_vpx arm64-darwin23-gcc  "arm64-apple-tvos${TVOS_MIN}"            "${DEV_SDK}" clang "${device_dir}/libvpx.a"
    build_vpx arm64-darwin23-gcc  "arm64-apple-tvos${TVOS_MIN}-simulator"  "${SIM_SDK}" clang /tmp/libvpx_tvsim_arm64.a
    build_vpx x86_64-darwin23-gcc "x86_64-apple-tvos${TVOS_MIN}-simulator" "${SIM_SDK}" nasm  /tmp/libvpx_tvsim_x86_64.a
    lipo -create /tmp/libvpx_tvsim_arm64.a /tmp/libvpx_tvsim_x86_64.a -output "${sim_dir}/libvpx.a"
    rm -f /tmp/libvpx_tvsim_arm64.a /tmp/libvpx_tvsim_x86_64.a
}

build_opus
build_libwebm
build_libvpx

echo "=== device (expect arm64) ==="; lipo -info "${device_dir}"/*.a
echo "=== sim (expect x86_64 arm64) ==="; lipo -info "${sim_dir}"/*.a
echo "Build third party libs done."
