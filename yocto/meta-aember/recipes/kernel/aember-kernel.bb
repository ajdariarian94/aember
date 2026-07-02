SUMMARY = "Aember minimal kernel"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

DEPENDS += "elfutils-native"

SRC_URI = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.68.tar.xz"
SRC_URI[sha256sum] = "283ff410e3f352ceed161ae30c0020301326059db03e86efcb384d46ac5840e2"

# Base config for all architectures
SRC_URI += "file://base.cfg"

# Architecture-specific configs
SRC_URI:append:x86-64  = " file://x64-linux.cfg"
SRC_URI:append:aarch64 = " file://arm64-linux.cfg"
SRC_URI:append:arm     = " file://arm-linux.cfg"

KERNEL_DANGLING_FEATURES_WARN_ONLY = "1"

# Force older C standard to avoid GCC 15 C23 issues
KERNEL_CC:append = " -std=gnu11"

inherit kernel

S = "${UNPACKDIR}/linux-6.6.68"
STAGING_KERNEL_DIR = "${S}"

FILESEXTRAPATHS:prepend := "${THISDIR}/linux-aember:"

# Architecture-specific default configs
KBUILD_DEFCONFIG:x86-64  = "x86_64_defconfig"
KBUILD_DEFCONFIG:aarch64 = "defconfig"
KBUILD_DEFCONFIG:arm     = "multi_v7_defconfig"

# ---------------------------------------------------------------------------
# Map Yocto MACHINE → build system directory names
# Mirrors the same mapping used in lxc.bb
# ---------------------------------------------------------------------------
DEPLOY_ARCH:x86-64  = "x64-poky-linux"
DEPLOY_ARCH:aarch64 = "aarch64-poky-linux"
DEPLOY_ARCH:arm     = "arm-poky-linux"

CUSTOM_OUTDIR = "${TOPDIR}/../build/${DEPLOY_ARCH}/kernel/"

# ---------------------------------------------------------------------------
# Force disable certificates (all architectures)
# ---------------------------------------------------------------------------
do_configure:append() {
    sed -i 's/CONFIG_SYSTEM_TRUSTED_KEYS=.*/CONFIG_SYSTEM_TRUSTED_KEYS=""/' ${B}/.config
    sed -i 's/CONFIG_SYSTEM_REVOCATION_KEYS=.*/CONFIG_SYSTEM_REVOCATION_KEYS=""/' ${B}/.config

    cat > ${B}/kernel-build-manifest.txt << MANIFEST
Aember Kernel Build Manifest
============================
Version: ${PV}
Architecture: ${TARGET_ARCH}
Machine: ${MACHINE}
Build Date: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Compiler: $(${KERNEL_CC} --version 2>/dev/null | head -n1 || echo "Unknown")
Defconfig: ${KBUILD_DEFCONFIG}
Kernel Image Type: ${KERNEL_IMAGETYPE}
MANIFEST
}

KERNEL_IMAGE = "${WORKDIR}/image"

# ---------------------------------------------------------------------------
# Deploy kernel artifacts + modules to build system output directory
# ---------------------------------------------------------------------------
do_deploy:append() {
    CUSTOM_OUTDIR=$(realpath -m ${TOPDIR}/../build/${DEPLOY_ARCH}/kernel)
    mkdir -p "${CUSTOM_OUTDIR}"

    # Kernel build manifest
    [ -f "${B}/kernel-build-manifest.txt" ] && \
        cp -v "${B}/kernel-build-manifest.txt" "${CUSTOM_OUTDIR}/"

    # Kernel images
    shopt -s nullglob || true
    for img in ${DEPLOYDIR}/${KERNEL_IMAGETYPE}*; do
        [ -f "$img" ] && cp -v "$img" "${CUSTOM_OUTDIR}/"
    done

    # DTBs
    [ -d "${DEPLOYDIR}/dtb" ] && cp -rv "${DEPLOYDIR}/dtb" "${CUSTOM_OUTDIR}/"

    # Symbols and module versioning
    [ -f "${B}/Module.symvers" ] && cp -v "${B}/Module.symvers" "${CUSTOM_OUTDIR}/"
    [ -f "${B}/System.map"     ] && cp -v "${B}/System.map"     "${CUSTOM_OUTDIR}/"

    # Kernel modules — needed for initramfs module loading
    if [ -d "${KERNEL_IMAGE}/lib/modules" ]; then
        mkdir -p "${CUSTOM_OUTDIR}/lib"
        cp -rv "${KERNEL_IMAGE}/lib/modules" "${CUSTOM_OUTDIR}/lib/"
    fi
}

# Skip buildpaths QA check — common for kernel packages
INSANE_SKIP:${PN}          += "buildpaths"
INSANE_SKIP:${PN}-src      += "buildpaths"
INSANE_SKIP:kernel-vmlinux += "buildpaths"

COMPATIBLE_MACHINE = "x64-linux|arm64-linux|arm-linux"