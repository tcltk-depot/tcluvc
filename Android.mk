LOCAL_PATH := $(call my-dir)

###########################
#
# tcluvc shared library
#
###########################

include $(CLEAR_VARS)

LOCAL_MODULE := tcluvc

tcl_path := $(LOCAL_PATH)/../tcl

include $(tcl_path)/tcl-config.mk

LOCAL_ADDITIONAL_DEPENDENCIES += $(tcl_path)/tcl-config.mk

tk_path := $(LOCAL_PATH)/../sdl2tk

include $(tk_path)/tk-config.mk

LOCAL_ADDITIONAL_DEPENDENCIES += $(tk_path)/tk-config.mk

LOCAL_C_INCLUDES := $(tcl_includes) $(tk_includes) \
	$(LOCAL_PATH) $(LOCAL_PATH)/compat $(LOCAL_PATH)/libuvc/include \
	$(LOCAL_PATH)/../jpeg-turbo

LOCAL_EXPORT_C_INCLUDES := $(LOCAL_C_INCLUDES)

LOCAL_SRC_FILES := \
	tcluvc.c \
	libuvc/src/ctrl.c \
	libuvc/src/device.c \
	libuvc/src/diag.c \
	libuvc/src/frame.c \
	libuvc/src/frame-mjpeg.c \
	libuvc/src/init.c \
	libuvc/src/stream.c

LOCAL_CFLAGS := $(tcl_cflags) $(tk_cflags) \
	-DPACKAGE_NAME="\"tcluvc\"" \
	-DPACKAGE_VERSION="\"0.1\"" \
	-DLIBUVC_HAVE_JPEG=1 \
	-O2

LOCAL_SHARED_LIBRARIES := libtcl libtk jpeg_tkimg

LOCAL_LDLIBS := -llog

include $(BUILD_SHARED_LIBRARY)
