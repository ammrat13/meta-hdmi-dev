SUMMARY = "HDMI Device Kernel Module"
DESCRIPTION = "Kernel module to drive ammrat13's HDMI Peripheral"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE.md;md5=1a4de40bb78539f9fcfa73935eea026e"

inherit module

SRC_URI = "\
    file://LICENSE.md \
    file://Makefile \
    file://ammrat13-hdmi-dev.c \
"

S = "${WORKDIR}"
