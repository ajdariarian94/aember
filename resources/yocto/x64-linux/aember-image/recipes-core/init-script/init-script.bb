SUMMARY = "Custom init that launches aember"
LICENSE = "CLOSED"

SRC_URI = "file://init"

do_install() {
    install -d ${D}
    install -m 0755 ${WORKDIR}/init ${D}/init
}

FILES:${PN} = "/init"
