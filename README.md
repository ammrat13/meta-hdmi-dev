# HDMI Peripheral: Kernel Driver

This is a kernel module that drives the HDMI Peripheral for the Zynq 7000
defined by [hdmi-cmd-gen][1] and [hdmi-cmd-enc][2]. It exposes the peripheral as
a framebuffer device, with additional `ioctl`s for vertical syncing.

See also the [hdmi-dev-video-player][3], which served as a precursor to this
kernel module.

## Building

This repository is a Yocto layer. It was tested with Petalinux 2023.2, which is
based on Yocto Langdale, and uses Linux-Xilinx 6.1.30.

This layer exposes the `ammrat13-hdmi-dev-mod` package, which builds the kernel
module and configures it to be loaded at boot via `/etc/modules-load.d/`.

## Usage

This kernel module takes no command-line parameters, and only supports
`640x480@60Hz` since that's the only configuration supported by the hardware.
Other than that, it exposes the HDMI Peripheral as a framebuffer device:
`/dev/fb*`.

Additionally, this driver supports the `ioctl`s for `FBIOGET_VBLANK` and
`FBIO_WAITFORVSYNC`.

### `FBIOGET_VBLANK`
This `ioctl` returns the current position of the scan dot, along with whether
the screen is currently in: vertical blanking, horizontal blanking, and vertical
sync. This never fails unless the address supplied as the first argument is
invalid.

### `FBIO_WAITFORVSYNC`
This `ioctl` waits until the scan dot is in *vertical blanking*. This does not
necesarily wait for the next frame's vertical blanking interval. It returns `0`
on success, or `EINTR`. It should never return `ETIMEDOUT` --- something's gone
wrong if it does.

[1]: https://github.com/ammrat13/hdmi-cmd-gen "ammrat13/hdmi-cmd-gen"
[2]: https://github.com/ammrat13/hdmi-cmd-enc "ammrat13/hdmi-cmd-enc"
[3]: https://github.com/ammrat13/hdmi-dev-video-player.git "ammrat13/hdmi-dev-video-player"
