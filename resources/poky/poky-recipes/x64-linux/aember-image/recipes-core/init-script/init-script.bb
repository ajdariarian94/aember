DESCRIPTION = "Custom init script for PID1"
LICENSE = "CLOSED"

SRC_URI = "file://init"

S = "${UNPACKDIR}"

do_install() {
    install -m 0755 ${WORKDIR}/sources/init ${D}/init
}

FILES:${PN} += "/init"
