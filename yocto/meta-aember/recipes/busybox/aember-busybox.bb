SUMMARY = "Aember minimal BusyBox"
DESCRIPTION = "Deterministic BusyBox build for Aember systems"
HOMEPAGE = "https://busybox.net/"
LICENSE = "GPL-2.0-only"

LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/GPL-2.0-only;md5=801f80980d171dd6425610833a22dbe6"

# ---- Release version ----
PV = "1.36.1"

# ---- Source tarball + checksum (deterministic) ----
SRC_URI = "https://busybox.net/downloads/busybox-${PV}.tar.bz2"
SRC_URI[sha256sum] = "b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314"

# ---- Config fragments ----
SRC_URI += "file://base.cfg"
SRC_URI:append:x86-64 = " file://x64-linux.cfg"
SRC_URI:append:aarch64 = " file://arm64-linux.cfg"
SRC_URI:append:arm = " file://arm-linux.cfg"

# ---- Important fix: sources must be relative to UNPACKDIR ----
S = "${UNPACKDIR}/busybox-${PV}"

FILESEXTRAPATHS:prepend := "${THISDIR}/aember-busybox/files:"

# ---- perl-native provides pod2text/pod2man/pod2html for docs ----
DEPENDS += "perl-native"

inherit pkgconfig deploy

CUSTOM_OUTDIR = "/aember_ws/aember/yocto/build/${MACHINE}/busybox/"

# ---- Merge a config fragment into .config, respecting disable lines ----
merge_cfg() {
    local fragment="$1"
    while IFS= read -r line; do
        # Handle lines like: # CONFIG_FOO is not set
        if echo "$line" | grep -qE '^# CONFIG_[A-Z0-9_]+ is not set$'; then
            key=$(echo "$line" | sed 's/^# \(CONFIG_[A-Z0-9_]*\) is not set$/\1/')
            sed -i "s/^${key}=.*$/# ${key} is not set/" .config
            grep -q "^# ${key} is not set" .config || echo "# ${key} is not set" >> .config
        # Handle lines like: CONFIG_FOO=y or CONFIG_FOO=value
        elif echo "$line" | grep -qE '^CONFIG_[A-Z0-9_]+='; then
            key=$(echo "$line" | cut -d= -f1)
            sed -i "s/^# ${key} is not set$/${line}/" .config
            sed -i "s/^${key}=.*$/${line}/" .config
            grep -q "^${key}=" .config || echo "$line" >> .config
        fi
    done < "$fragment"
}

# ---- Configure step ----
do_configure() {
    oe_runmake CROSS_COMPILE=${TARGET_PREFIX} distclean
    oe_runmake CROSS_COMPILE=${TARGET_PREFIX} defconfig

    merge_cfg ${UNPACKDIR}/base.cfg

    if [ "${TARGET_ARCH}" = "x86_64" ]; then
        merge_cfg ${UNPACKDIR}/x64-linux.cfg
    elif [ "${TARGET_ARCH}" = "aarch64" ]; then
        merge_cfg ${UNPACKDIR}/arm64-linux.cfg
    elif [ "${TARGET_ARCH}" = "arm" ]; then
        merge_cfg ${UNPACKDIR}/arm-linux.cfg
    fi

    if [ "${TARGET_ARCH}" = "arm" ]; then
        yes "" | oe_runmake \
            CROSS_COMPILE=${TARGET_PREFIX} \
            "EXTRA_CFLAGS=-std=gnu11 --sysroot=${RECIPE_SYSROOT} -mfloat-abi=hard -mfpu=neon" \
            "EXTRA_LDFLAGS=--sysroot=${RECIPE_SYSROOT}" \
            oldconfig
    else
        yes "" | oe_runmake \
            CROSS_COMPILE=${TARGET_PREFIX} \
            "EXTRA_CFLAGS=-std=gnu11 --sysroot=${RECIPE_SYSROOT}" \
            "EXTRA_LDFLAGS=--sysroot=${RECIPE_SYSROOT}" \
            oldconfig
    fi
}

# ---- Compile step ----
do_compile() {
    if [ "${TARGET_ARCH}" = "arm" ]; then
        oe_runmake -j${@oe.utils.cpu_count()} \
            CROSS_COMPILE=${TARGET_PREFIX} \
            "EXTRA_CFLAGS=-std=gnu11 --sysroot=${RECIPE_SYSROOT} -mfloat-abi=hard -mfpu=neon" \
            "EXTRA_LDFLAGS=--sysroot=${RECIPE_SYSROOT}"
    else
        oe_runmake -j${@oe.utils.cpu_count()} \
            CROSS_COMPILE=${TARGET_PREFIX} \
            "EXTRA_CFLAGS=-std=gnu11 --sysroot=${RECIPE_SYSROOT}" \
            "EXTRA_LDFLAGS=--sysroot=${RECIPE_SYSROOT}"
    fi

    cat > ${B}/busybox-build-manifest.txt << EOF
Aember BusyBox Build Manifest
=============================
BusyBox Version: ${PV}
Architecture: ${TARGET_ARCH}
Machine: ${MACHINE}
Build Date: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
Compiler: $(${CC} --version 2>/dev/null | head -n1 || echo "Unknown")
EOF
}

# ---- Install step ----
do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/busybox ${D}${bindir}/busybox
}

# ---- Deploy artifacts to custom output location ----
do_deploy() {
    install -d ${DEPLOYDIR}
    install -m 0755 ${B}/busybox ${DEPLOYDIR}/busybox
    install -m 0644 ${B}/busybox-build-manifest.txt ${DEPLOYDIR}/

    install -d ${CUSTOM_OUTDIR}
    install -m 0755 ${B}/busybox ${CUSTOM_OUTDIR}/busybox
    install -m 0644 ${B}/busybox-build-manifest.txt ${CUSTOM_OUTDIR}/
}

addtask deploy after do_compile before do_build

# ---- QA skips (common for kernel-like packages) ----
INSANE_SKIP:${PN} += "buildpaths already-stripped"

# ---- Compatible machines (mirrors kernel recipe) ----
COMPATIBLE_MACHINE = "x64-linux|arm64-linux|arm-linux"