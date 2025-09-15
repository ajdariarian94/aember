DESCRIPTION = "Aember initramfs image"
LICENSE = "CLOSED"

IMAGE_INSTALL += "busybox aember init-script"
IMAGE_LINGUAS = " "

inherit core-image

# Minimal image types for initramfs
IMAGE_FSTYPES = "cpio.gz"

ROOTFS_POSTPROCESS_COMMAND += "aember_busybox_symlinks;"

aember_busybox_symlinks() {
    mkdir -p ${IMAGE_ROOTFS}/bin

    # Copy BusyBox binary (check both common install paths)
    if [ -x ${IMAGE_ROOTFS}/usr/bin/busybox ]; then
        cp ${IMAGE_ROOTFS}/usr/bin/busybox ${IMAGE_ROOTFS}/bin/
    fi

    # Generate all symlinks in /bin
    # ${IMAGE_ROOTFS}/bin/busybox --install -s ${IMAGE_ROOTFS}/bin

    # Make sure /bin/sh always exists
    # ln -sf busybox ${IMAGE_ROOTFS}/bin/sh
}


