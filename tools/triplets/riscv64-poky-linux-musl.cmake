# vcpkg overlay triplet for riscv64-poky-linux with musl (Poky SDK, opt-in variant).
set(VCPKG_TARGET_ARCHITECTURE riscv64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Build every port with the Poky toolchain
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/../toolchains/riscv64-poky-linux-musl.cmake")
