#!/bin/bash
# Build aember's programs with another optimisation level, for comparing
# (doc/performance.md). Run in the dev container after `aember app build
# --architecture ARCH` (it reuses that build's vcpkg packages, which keep
# their own flags).
#
#   tools/perf/variant.sh ARCH NAME "-Os" [syms]
#
# -> build/perf/ARCH-NAME/install/{bin,services}/ and, for
#    `aember systemtest --scenarios tools/perf/measure.py --input binaries=...`,
#    build/perf/ARCH-NAME/tree/usr/bin/{aember,aember-netd,aember-containerd}.
# syms: keep symbols and add -g1 (for tools/perf/size.py); the Release
# flags are otherwise the toolchain's (LTO, sections, gc-sections).
set -euo pipefail
ARCH=$1 NAME=$2 OPT=$3 SYMS=${4:-}
W=$(cd "$(dirname "$0")/../.." && pwd)
B=$W/build/perf/$ARCH-$NAME
VI=$W/build/configure/$ARCH/Release/vcpkg_installed
[ -d "$VI" ] || { echo "no $VI: run aember app build --architecture $ARCH first" >&2; exit 1; }
mkdir -p "$B"
EXTRA="" STRIP="-Wl,--strip-all" NOSTRIP=""
if [ "$SYMS" = syms ]; then EXTRA="-g1" STRIP="" NOSTRIP='unset(AEMBER_STRIP_COMMAND)'; fi
JOBS=${AEMBER_BUILD_JOBS:-4}
cat > "$B/toolchain.cmake" <<TC
include("$W/tools/toolchains/$ARCH.cmake")
set(CMAKE_C_FLAGS_RELEASE   "$OPT -flto=$JOBS -ffunction-sections -fdata-sections -fvisibility=hidden $EXTRA")
set(CMAKE_CXX_FLAGS_RELEASE "$OPT -flto=$JOBS -ffunction-sections -fdata-sections -fvisibility=hidden $EXTRA")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto=$JOBS -Wl,--gc-sections -Wl,--as-needed $STRIP")
$NOSTRIP
TC
cmake -S "$W" -B "$B/cfg" -DCMAKE_BUILD_TYPE=Release -DARCHITECTURE="$ARCH" \
  -DCMAKE_TOOLCHAIN_FILE="$W/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_CHAINLOAD_TOOLCHAIN_FILE="$B/toolchain.cmake" \
  -DVCPKG_TARGET_TRIPLET="$ARCH" -DVCPKG_HOST_TRIPLET=x64-linux \
  -DVCPKG_OVERLAY_TRIPLETS="$W/tools/triplets" -DVCPKG_INSTALLED_DIR="$VI" \
  -DVCPKG_MANIFEST_INSTALL=OFF -DBUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX="$B/install" > "$B/configure.log" 2>&1
AEMBER_BUILD_JOBS=$JOBS cmake --build "$B/cfg" -j"$JOBS" > "$B/build.log" 2>&1 \
  || { tail -20 "$B/build.log" >&2; exit 1; }
mkdir -p "$B/tree/usr/bin"
cp "$B/install/bin/aember" "$B/install/services/aember-netd" \
   "$B/install/services/aember-containerd" "$B/tree/usr/bin/"
echo "$B"
