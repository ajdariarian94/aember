SUMMARY = "Aember LXC container runtime"
LICENSE = "LGPL-2.1-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=4b6551da9cb7d5b3017d1c0a3e31469b"

DEPENDS = "glib-2.0 util-linux libcap"

SRC_URI = "https://linuxcontainers.org/downloads/lxc/lxc-6.0.2.tar.gz"
SRC_URI[sha256sum] = "1930aa10d892db8531d1353d15f7ebf5913e74a19e134423e4d074c07f2d6e8b"

SRC_URI += " \
    file://base.conf \
    file://x64-linux.conf \
    file://arm64-linux.conf \
    file://0001-lxccontainer-prctl-fallback.patch \
"

inherit meson pkgconfig deploy

addtask deploy after do_install before do_build

S = "${UNPACKDIR}/lxc-6.0.2"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Minimal build — no apparmor, selinux, seccomp, no init scripts
EXTRA_OEMESON = " \
    -Dinit-script=[] \
    -Dinstall-state-dirs=false \
    -Dapparmor=false \
    -Dselinux=false \
    -Dseccomp=false \
    -Dcapabilities=false \
    -Dexamples=false \
    -Dman=false \
    -Dlog-path=/var/log/lxc \
    -Ddata-path=/var/lib/lxc \
    -Ddbus=false \
"

do_install:append() {
    install -d ${D}${sysconfdir}/lxc
    install -m 0644 ${WORKDIR}/sources/base.conf ${D}${sysconfdir}/lxc/default.conf

    # Install architecture-specific LXC config
    if [ "${TARGET_ARCH}" = "x86_64" ]; then
        install -m 0644 ${WORKDIR}/sources/x64-linux.conf ${D}${sysconfdir}/lxc/arch.conf
    elif [ "${TARGET_ARCH}" = "aarch64" ]; then
        install -m 0644 ${WORKDIR}/sources/arm64-linux.conf ${D}${sysconfdir}/lxc/arch.conf
    fi

    install -d ${D}${localstatedir}/lib/lxc
}

# Ensure -dev package is produced alongside the main package
PACKAGES =+ "${PN}-templates"
PACKAGES =+ "${PN}-dev"

FILES:${PN} = " \
    ${bindir}/lxc-* \
    ${sbindir}/init.lxc \
    ${libdir}/liblxc.so* \
    ${libdir}/lxc/ \
    ${libexecdir}/lxc/ \
    ${sysconfdir}/lxc/ \
    ${sysconfdir}/default/lxc \
    ${localstatedir}/lib/lxc \
    ${datadir}/lxc/ \
    ${datadir}/bash-completion/ \
"

FILES:${PN}-templates = " \
    ${datadir}/lxc/templates/ \
"

# Dev package: headers, pkgconfig, and the unversioned .so linker symlink
FILES:${PN}-dev = " \
    ${includedir}/lxc/ \
    ${libdir}/pkgconfig/lxc.pc \
    ${libdir}/liblxc.so \
"

# Make the dev package depend on the runtime so consumers get everything
RDEPENDS:${PN}-dev = "${PN}"

# Allow the dev package to be built into the SDK / sysroot
ALLOW_EMPTY:${PN}-dev = "1"

# Map Yocto TARGET_SYS → build system directory names
DEPLOY_ARCH:x86-64  = "x64-poky-linux"
DEPLOY_ARCH:aarch64 = "aarch64-poky-linux"
DEPLOY_ARCH:arm     = "arm-poky-linux"

CUSTOM_OUTDIR = "${TOPDIR}/../build/${DEPLOY_ARCH}/virtualization/"

do_deploy() {
    CUSTOM_OUTDIR=$(realpath -m ${TOPDIR}/../build/${DEPLOY_ARCH}/virtualization)
    install -d ${CUSTOM_OUTDIR}

    # ── Runtime binaries ──────────────────────────────────────────────────────
    for bin in ${D}${bindir}/lxc-*; do
        [ -f "$bin" ] && install -m 0755 "$bin" ${CUSTOM_OUTDIR}/
    done

    # ── Shared libraries → under libs/ ───────────────────────────────────────
    install -d ${CUSTOM_OUTDIR}/libs
    for lib in ${D}${libdir}/liblxc.so*; do
        if [ -L "$lib" ]; then
            cp -P "$lib" ${CUSTOM_OUTDIR}/libs/
        elif [ -f "$lib" ]; then
            install -m 0755 "$lib" ${CUSTOM_OUTDIR}/libs/
        fi
    done

    # ── Headers ───────────────────────────────────────────────────────────────
    if [ -d "${D}${includedir}/lxc" ]; then
        install -d ${CUSTOM_OUTDIR}/include/lxc
        cp -r ${D}${includedir}/lxc/. ${CUSTOM_OUTDIR}/include/lxc/
    fi

    # ── pkg-config — rewrite paths to match deployed layout ──────────────────
    if [ -f "${D}${libdir}/pkgconfig/lxc.pc" ]; then
        install -d ${CUSTOM_OUTDIR}/pkgconfig
        sed \
            -e "s|^prefix=.*|prefix=${CUSTOM_OUTDIR}|" \
            -e "s|^libdir=.*|libdir=${CUSTOM_OUTDIR}/libs|" \
            -e "s|^includedir=.*|includedir=${CUSTOM_OUTDIR}/include|" \
            ${D}${libdir}/pkgconfig/lxc.pc > ${CUSTOM_OUTDIR}/pkgconfig/lxc.pc
    fi

    # ── Configs ───────────────────────────────────────────────────────────────
    cp -r ${D}${sysconfdir}/lxc ${CUSTOM_OUTDIR}/etc-lxc
}

# Skip buildpaths QA — same as kernel recipe
INSANE_SKIP:${PN} += "buildpaths file-rdeps"
INSANE_SKIP:${PN}-templates += "file-rdeps"
INSANE_SKIP:${PN}-dev += "buildpaths"

COMPATIBLE_MACHINE = "x64-linux|arm64-linux|arm-linux"