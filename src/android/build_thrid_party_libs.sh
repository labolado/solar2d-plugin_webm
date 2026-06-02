#!/bin/bash
#
# Cross-compiles the WebM decode stack (libvpx, libwebm, opus) as static libs
# for every Android ABI into src/android/jni/<abi>/{libvpx,libwebm,libopus}.a
# (Android.mk picks them up as PREBUILT_STATIC_LIBRARY per TARGET_ARCH_ABI).
#
# opus/libwebm use the NDK CMake toolchain; libvpx uses its own configure with
# the NDK per-arch clang wrappers (the *-android-gcc targets assume a standalone
# NDK toolchain — the clang wrapper auto-applies the target triple + sysroot).
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
jni_dir="${current_dir}/jni"

# Use the newest installed NDK. (Also: r27+ ships universal arm64 llvm tools; the
# older r21 tools are x86_64-only and crash under Rosetta on recent macOS with a
# libc++ -ftyped-cxx-new-delete abort, so newer is required here anyway.)
ANDROID_SDK_HOME="${ANDROID_SDK_HOME:-/opt/homebrew/share/android-commandlinetools}"
if [ -z "${ANDROID_NDK}" ]; then
    ANDROID_NDK="${ANDROID_SDK_HOME}/ndk/$(ls -1 "${ANDROID_SDK_HOME}/ndk" | sort -V | tail -1)"
fi
API=21
CPU_CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

HOST_TAG=darwin-x86_64
TOOLBIN="${ANDROID_NDK}/toolchains/llvm/prebuilt/${HOST_TAG}/bin"
echo "NDK: ${ANDROID_NDK}"

# libvpx's x86 configure probes nasm section alignment via a bare `readelf`,
# which the NDK ships only as `llvm-readelf`. Expose a shim so the probe passes.
SHIM="${current_dir}/.toolshim"
mkdir -p "${SHIM}"
ln -sf "${TOOLBIN}/llvm-readelf" "${SHIM}/readelf"
export PATH="${SHIM}:${PATH}"

# opus + libwebm via the NDK CMake toolchain.
# $1 proj  $2 lib  $3 abi  $4.. extra cmake args
build_cmake_lib() {
    local proj="$1" lib="$2" abi="$3"; shift 3
    local bdir="${proj}/build_android_${abi}"
    rm -rf "${bdir}"; mkdir -p "${bdir}"
    cmake -S "${proj}" -B "${bdir}" -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK}/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="${abi}" \
        -DANDROID_PLATFORM="android-${API}" \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        "$@"
    cmake --build "${bdir}" -j"${CPU_CORES}"
    cp "${bdir}/${lib}" "${jni_dir}/${abi}/${lib}"
}

# libvpx via its configure, driven by the NDK clang wrappers.
# $1 abi  $2 vpx-target  $3 clang-triple
build_vpx() {
    local abi="$1" target="$2" triple="$3"
    local cc="${TOOLBIN}/${triple}${API}-clang"
    # ARM .asm assembles via clang's integrated assembler (NDK r24+ dropped GAS);
    # x86 SIMD .asm needs a real x86 assembler (nasm) — clang can't take -f elf32.
    local as_tool="${cc}"
    case "${abi}" in
        x86|x86_64) as_tool="$(command -v nasm)" ;;
    esac
    local bdir="${libvpx_dir}/build_android_${abi}"
    rm -rf "${bdir}"; mkdir -p "${bdir}"
    pushd "${bdir}" > /dev/null
    local extra_flags=""
    case "${abi}" in
        x86_64) extra_flags="--disable-avx512 --disable-avx2 --disable-avx --disable-sse4_2 --disable-sse4_1 --disable-ssse3 --disable-sse3 --disable-sse2 --disable-mmx" ;;
    esac
    CC="${cc}" \
    CXX="${TOOLBIN}/${triple}${API}-clang++" \
    AS="${as_tool}" \
    LD="${cc}" \
    AR="${TOOLBIN}/llvm-ar" \
    NM="${TOOLBIN}/llvm-nm" \
    STRIP="${TOOLBIN}/llvm-strip" \
    RANLIB="${TOOLBIN}/llvm-ranlib" \
    "${libvpx_dir}/configure" \
        --target="${target}" \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests \
        --disable-webm-io --disable-libyuv --disable-postproc \
        --enable-vp8 --enable-vp9 \
        --disable-vp8-encoder --disable-vp9-encoder \
        --enable-static --disable-shared --enable-pic \
        --disable-runtime-cpu-detect \
        ${extra_flags}
    make -j"${CPU_CORES}"
    popd > /dev/null
    cp "${bdir}/libvpx.a" "${jni_dir}/${abi}/libvpx.a"
}

# abi | libvpx target | NDK clang triple (here-doc so set -e aborts on failure)
while read -r abi vpxtarget triple; do
    [ -z "${abi}" ] && continue
    echo "==================== ${abi} ===================="
    mkdir -p "${jni_dir}/${abi}"
    build_cmake_lib "${opus_dir}"   libopus.a "${abi}" -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_DISABLE_INTRINSICS=ON
    build_cmake_lib "${libwebm_dir}" libwebm.a "${abi}" \
        -DENABLE_WEBMTS=OFF -DENABLE_WEBMINFO=OFF -DENABLE_TESTS=OFF -DENABLE_SAMPLE_PROGRAMS=OFF
    build_vpx "${abi}" "${vpxtarget}" "${triple}"
done <<'EOF'
armeabi-v7a armv7-android-gcc    armv7a-linux-androideabi
arm64-v8a   arm64-android-gcc    aarch64-linux-android
x86_64      x86_64-android-gcc   x86_64-linux-android
EOF

echo "=== results ==="
for abi in armeabi-v7a arm64-v8a x86_64; do
    echo "${abi}:"; ls -la "${jni_dir}/${abi}"/*.a 2>&1
done
echo "Build third party libs done."
