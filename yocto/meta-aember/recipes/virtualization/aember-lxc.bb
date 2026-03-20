SUMMARY = "Aember LXC container runtime"
LICENSE = "LGPL-2.1-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=4b6551da9cb7d5b3017d1c0a3e31469b"

DEPENDS = "glib-2.0 util-linux libcap"

SRC_URI = "https://linuxcontainers.org/downloads/lxc/lxc-6.0.2.tar.gz"
SRC_URI[sha256sum] = "1930aa10d892db8531d1353d15f7ebf5913e74a19e134423e4d074c07f2d6e8b"

SRC_URI += " \
    file://base.conf \
    file://x64-linux.conf \
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
    install -m 0644 ${WORKDIR}/sources/x64-linux.conf ${D}${sysconfdir}/lxc/x64-linux.conf

    install -d ${D}${localstatedir}/lib/lxc
}

PACKAGES =+ "${PN}-templates"

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

CUSTOM_OUTDIR = "${TOPDIR}/../build/${MACHINE}/virtualization/"

do_deploy() {
    install -d ${CUSTOM_OUTDIR}

    # Copy lxc binaries
    for bin in ${D}${bindir}/lxc-*; do
        [ -f "$bin" ] && install -m 0755 "$bin" ${CUSTOM_OUTDIR}/
    done

    # Copy liblxc
    for lib in ${D}${libdir}/liblxc.so*; do
        [ -f "$lib" ] && install -m 0755 "$lib" ${CUSTOM_OUTDIR}/
    done

    # Copy configs
    cp -r ${D}${sysconfdir}/lxc ${CUSTOM_OUTDIR}/etc-lxc
}

# Skip buildpaths QA — same as kernel recipe
INSANE_SKIP:${PN} += "buildpaths file-rdeps"
INSANE_SKIP:${PN}-templates += "file-rdeps"

COMPATIBLE_MACHINE = "x64-linux|arm64-linux|arm-linux"