SUMMARY = "HDMI Device Kernel Module"
DESCRIPTION = "Kernel module to drive ammrat13's HDMI Peripheral"
LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://LICENSE.md;md5=b234ee4d69f5fce4486a80fdaf4a4263"

inherit module

SRC_URI = "\
    file://LICENSE.md \
    file://Makefile \
    file://ammrat13-hdmi-dev.conf \
    file://ammrat13-hdmi-dev.c \
"

# Handle loading this module automatically on boot
FILES:${PN} += "\
    /etc/ \
    /etc/modules-load.d/ \
    /etc/modules-load.d/ammrat13-hdmi-dev.conf \
"
do_install:append() {
    install -m 0755 -d ${D}/etc/modules-load.d/
    install -m 0644 -t ${D}/etc/modules-load.d/ ${S}/ammrat13-hdmi-dev.conf
}

S = "${WORKDIR}"
