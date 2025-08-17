SUMMARY = "Aember init system binary"
LICENSE = "CLOSED"

SRC_URI = "file://aember"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/aember ${D}${bindir}/aember
}

FILES:${PN} = "${bindir}/aember"
