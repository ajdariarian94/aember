DESCRIPTION = "Aember prebuilt binary"
LICENSE = "CLOSED"
LIC_FILES_CHKSUM = ""

SRC_URI = "file://aember \
           file://AEMBER.LICENSE"

S = "${UNPACKDIR}"

do_install() {
    # Install binary
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/sources/aember ${D}${bindir}/aember

    # Install license
    # install -d ${D}${datadir}/licenses/aember
    # install -m 0644 ${WORKDIR}/AEMBER.LICENSE ${D}${datadir}/licenses/aember/
}

# Tell Yocto to include the license folder in the package
# FILES_${PN} += "${datadir}/licenses/aember"
