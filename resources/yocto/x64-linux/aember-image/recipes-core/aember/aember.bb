SUMMARY = "Aember init system binary"
LICENSE = "CLOSED"

SRC_URI = "file://aember"

S = "${WORKDIR}"

do_install() {
    install -d ${D}/
    install -m 0755 ${WORKDIR}/aember ${D}/aember
}

FILES:${PN} = "/aember"