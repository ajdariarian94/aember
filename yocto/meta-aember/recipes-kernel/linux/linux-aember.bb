SUMMARY = "Aember minimal kernel"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

DEPENDS += "elfutils-native"

SRC_URI = "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.68.tar.xz"
SRC_URI[sha256sum] = "283ff410e3f352ceed161ae30c0020301326059db03e86efcb384d46ac5840e2"

SRC_URI += "file://base.cfg"
SRC_URI:append:x86-64 = " file://x64-linux.cfg"

KERNEL_DANGLING_FEATURES_WARN_ONLY = "1"

# Force older C standard to avoid GCC 15 C23 issues
KERNEL_CC:append = " -std=gnu11"

inherit kernel

S = "${UNPACKDIR}/linux-6.6.68"
STAGING_KERNEL_DIR = "${S}"

FILESEXTRAPATHS:prepend := "${THISDIR}/linux-aember:"

KBUILD_DEFCONFIG:x86-64 = "x86_64_defconfig"

# Force disable certificates
do_configure:append() {
    sed -i 's/CONFIG_SYSTEM_TRUSTED_KEYS=.*/CONFIG_SYSTEM_TRUSTED_KEYS=""/' ${B}/.config
    sed -i 's/CONFIG_SYSTEM_REVOCATION_KEYS=.*/CONFIG_SYSTEM_REVOCATION_KEYS=""/' ${B}/.config
}

# Skip buildpaths QA check - common for kernel packages
INSANE_SKIP:${PN} += "buildpaths"
INSANE_SKIP:${PN}-src += "buildpaths"
INSANE_SKIP:kernel-vmlinux += "buildpaths"