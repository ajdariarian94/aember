SUMMARY = "Aember minimal kernel"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

DEPENDS += "elfutils-native"

SRC_URI = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.68.tar.xz"
SRC_URI[sha256sum] = "283ff410e3f352ceed161ae30c0020301326059db03e86efcb384d46ac5840e2"

# Base config for all architectures
SRC_URI += "file://base.cfg"

# Architecture-specific configs
SRC_URI:append:x86-64 = " file://x64-linux.cfg"
SRC_URI:append:aarch64 = " file://arm64-linux.cfg"
SRC_URI:append:arm = " file://arm-linux.cfg"

KERNEL_DANGLING_FEATURES_WARN_ONLY = "1"

# Force older C standard to avoid GCC 15 C23 issues
KERNEL_CC:append = " -std=gnu11"

inherit kernel

S = "${UNPACKDIR}/linux-6.6.68"
STAGING_KERNEL_DIR = "${S}"

FILESEXTRAPATHS:prepend := "${THISDIR}/linux-aember:"

# Architecture-specific default configs
KBUILD_DEFCONFIG:x86-64 = "x86_64_defconfig"
KBUILD_DEFCONFIG:aarch64 = "defconfig"
KBUILD_DEFCONFIG:arm = "multi_v7_defconfig"

CUSTOM_OUTDIR = "${TOPDIR}/../build/${MACHINE}/kernel/"

# Force disable certificates (all architectures)
do_configure:append() {
    sed -i 's/CONFIG_SYSTEM_TRUSTED_KEYS=.*/CONFIG_SYSTEM_TRUSTED_KEYS=""/' ${B}/.config
    sed -i 's/CONFIG_SYSTEM_REVOCATION_KEYS=.*/CONFIG_SYSTEM_REVOCATION_KEYS=""/' ${B}/.config

    # Generate build manifest for certification
    cat > ${B}/kernel-build-manifest.txt << EOF
Aember Kernel Build Manifest
============================
Version: ${PV}
Architecture: ${TARGET_ARCH}
Machine: ${MACHINE}
Build Date: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Compiler: $(${KERNEL_CC} --version 2>/dev/null | head -n1 || echo "Unknown")
Defconfig: ${KBUILD_DEFCONFIG}
Kernel Image Type: ${KERNEL_IMAGETYPE}
EOF
}

# Deploy manifest and kernel artifacts to custom output location
do_deploy:append() {
    install -m 0644 ${B}/kernel-build-manifest.txt ${DEPLOYDIR}/

    # Copy to custom output location
    install -d ${CUSTOM_OUTDIR}
    install -m 0644 ${B}/kernel-build-manifest.txt ${CUSTOM_OUTDIR}/

    # Copy kernel image (bzImage for x86_64, Image for aarch64, zImage for arm)
    for img in ${DEPLOYDIR}/${KERNEL_IMAGETYPE}*; do
        [ -f "$img" ] && install -m 0644 "$img" ${CUSTOM_OUTDIR}/
    done

    # Copy DTBs if present (aarch64 and arm)
    if [ -d ${DEPLOYDIR}/dtb ]; then
        cp -r ${DEPLOYDIR}/dtb ${CUSTOM_OUTDIR}/
    fi

    # Copy Module.symvers and System.map for out-of-tree module builds
    [ -f ${B}/Module.symvers ] && install -m 0644 ${B}/Module.symvers ${CUSTOM_OUTDIR}/
    [ -f ${B}/System.map ] && install -m 0644 ${B}/System.map ${CUSTOM_OUTDIR}/
}

# Skip buildpaths QA check - common for kernel packages
INSANE_SKIP:${PN} += "buildpaths"
INSANE_SKIP:${PN}-src += "buildpaths"
INSANE_SKIP:kernel-vmlinux += "buildpaths"

# Compatible with all architectures
COMPATIBLE_MACHINE = "x64-linux|arm64-linux|arm-linux"