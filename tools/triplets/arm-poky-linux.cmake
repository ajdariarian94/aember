# vcpkg overlay triplet for 32-bit ARM (ARMv7-A hard-float, Poky SDK).
# Lives here, not in the vcpkg submodule, so the submodule pointer stays put;
# ConfigureStep passes -DVCPKG_OVERLAY_TRIPLETS=tools/triplets.
set(VCPKG_TARGET_ARCHITECTURE arm)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Build every port with the Poky toolchain
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/arm-poky-linux.cmake")
