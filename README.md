tcluvc
----

This is a mirror of the tcluvc module extracted from the official
[Androwish monorepository](http://www.androwish.org). Please
log any bug reports, RFE's there. This repository is
unmonitored.

Tcl interface to UVC cameras using `libusb`. It may run on Android devices,
however, many devices with ARM processors seem to have problems with
isochronous transfers in their USB host implementations. The effect is
usually that no image data can be read from the UVC camera, ever.

Support on other platforms varies, too. It is known to work on x86(\_64)
Linuxen, MacOSX, and maybe FreeBSD and Haiku, but these are untested.

On Linux, an udev rule needs to be added allowing for detaching the
kernel driver from the device. Users must be in the `plugdev` group
in order to be able to open the camera's USB device.

An example `99-libuvc.rules` file is

    # libuvc: enable users in group "plugdev" to detach the uvcvideo kernel driver
    ACTION=="add", SUBSYSTEM=="usb", ENV{ID_USB_INTERFACES}=="*:0e02??:*", GROUP="plugdev"

