#!/bin/bash
#
# Builds the plugin.webm Corona static library for iOS (device + simulator) and
# packages it into plugins/2025.3720/{iphone,iphone-sim}.
#
# A static library only archives this target's own object files, so after each
# SDK build we merge the compiled plugin objects with the matching per-SDK
# third-party archives (libvpx/libwebm/libopus) via `libtool` into one
# self-contained libplugin_webm.a. Run build_thrid_party_libs.sh first.
#
set -o errexit

# A static archive stores each member's mtime plus a timestamp in its __.SYMDEF
# table of contents, so libtool — both ours below and xcodebuild's own static-lib
# step — emits a byte-different .a every run even when the compiled objects are
# identical. That's why the committed libplugin_webm.a shows a git diff after every
# build. ZERO_AR_DATE=1 makes Apple's libtool/ar zero those timestamps, producing a
# reproducible archive (verified: two builds of unchanged sources are now identical).
export ZERO_AR_DATE=1

path=$(dirname "$0")
pushd "$path" > /dev/null; path=$(pwd); popd > /dev/null

TARGET_NAME=plugin_webm
CONFIG=Release
lib_version=2025.3720
lib_name=lib${TARGET_NAME}.a   # libplugin_webm.a

if [ ! -d "$path/third_party_libs/device" ] || [ ! -d "$path/third_party_libs/sim" ]; then
    echo "ERROR: third_party_libs/{device,sim} missing. Run ./build_thrid_party_libs.sh first." >&2
    exit 1
fi

# Clean + build both SDKs.
xcodebuild -project "$path/Plugin.xcodeproj" -configuration $CONFIG clean
xcodebuild -project "$path/Plugin.xcodeproj" -configuration $CONFIG -sdk iphoneos
xcodebuild -project "$path/Plugin.xcodeproj" -configuration $CONFIG -sdk iphonesimulator

# $1 = plugin platform dir (iphone|iphone-sim)
# $2 = xcodebuild SDK build-dir suffix (iphoneos|iphonesimulator)
# $3 = third-party slice dir (device|sim)
package() {
    local plat="$1" sdkdir="$2" slice="$3"
    local built="$path/build/$CONFIG-$sdkdir/${lib_name}"
    local merged="$path/build/${plat}-${lib_name}"

    # Merge plugin objects + third-party static libs into one self-contained lib.
    libtool -static -o "$merged" \
        "$built" \
        "$path/third_party_libs/${slice}/libvpx.a" \
        "$path/third_party_libs/${slice}/libwebm.a" \
        "$path/third_party_libs/${slice}/libopus.a"

    local dst_dir="$path/../../plugins/${lib_version}/${plat}"
    mkdir -p "$dst_dir"
    cp "$path/metadata.lua" "$dst_dir/metadata.lua"
    cp "$merged" "$dst_dir/${lib_name}"

    echo "Packing ${plat}..."
    cd "$path"
    tar -czf "${lib_version}-${plat}.tgz" -C "$dst_dir" "${lib_name}" metadata.lua
    echo "  -> $dst_dir/${lib_name}"
    echo "  -> $path/${lib_version}-${plat}.tgz"
}

package iphone     iphoneos        device
package iphone-sim iphonesimulator sim

echo Done.
