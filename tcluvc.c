/*
 * tcluvc.c --
 *
 *      This file contains the implementation of the "uvc" Tcl built-in
 *      command which allows to operate cameras using libuvc.
 *
 * Copyright (c) 2016-25 Christian Werner <chw at ch minus werner dot de>
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include <tk.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <sched.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <libusb-1.0/libusb.h>
#include <libusb-1.0/libusb_dl.h>
#include <libuvc/libuvc.h>
#include <libuvc/libuvc_internal.h>
#ifdef HAVE_LIBUDEV
#include <libudev.h>
#endif

/*
 * Provide Tcl_Size if needed (Tcl 8.6 and earlier).
 */

#ifndef TCL_SIZE_MAX
#include <limits.h>
#define TCL_SIZE_MAX INT_MAX
#ifndef Tcl_Size
typedef int Tcl_Size;
#endif
#endif

#if defined(ANDROID) && !defined(__TERMUX__)
#define LIBUSB_SO "libusb.so"
#endif

#if defined(linux) && !defined(ANDROID) && !defined(__TERMUX__)
#define LIBUSB_SO "libusb-1.0.so.0"
#endif

#ifdef __FreeBSD__
#define LIBUSB_SO "libusb.so.3"
#endif

#ifdef __OpenBSD__
#define LIBUSB_SO "libusb-1.0.so"
#endif

#ifdef __sun
#define LIBUSB_SO "libusb-1.0.so.0"
#endif

#ifdef __APPLE__
#define LIBUSB_SO "libusb-1.0.dylib"
#endif

#ifdef __HAIKU__
#define LIBUSB_SO "libusb-1.0.so.0"
#endif

#ifdef __TERMUX__
#define LIBUSB_SO "libusb-1.0.so"
#endif

#ifndef LIBUSB_SO
#error LIBUSB_SO unknown on this platform
#endif

#ifndef TCL_THREADS
#error "build requires TCL_THREADS"
#endif


/*
 * RIFF/AVI structures and constants.
 */

static void inline
PUT16LE(unsigned short *p, unsigned short v)
{
    unsigned char b[2];

    b[0] = v & 0xff;
    b[1] = (v >> 8) & 0xff;
    memcpy(p, b, 2);
}

static void inline
PUT32LE(unsigned int *p, unsigned int v)
{
    unsigned char b[4];

    b[0] = v & 0xff;
    b[1] = (v >> 8) & 0xff;
    b[2] = (v >> 16) & 0xff;
    b[3] = (v >> 24) & 0xff;
    memcpy(p, b, 4);
}

struct RIFF_avih {
    unsigned int uspf;    /* us per frame. */
    unsigned int bps;     /* Data rate. */
    unsigned int res0;
    unsigned int flags;
    unsigned int nframes; /* Number of frames. */
    unsigned int res1;
    unsigned int nstreams;
    unsigned int bufsize;
    unsigned int width;
    unsigned int height;
    unsigned int scale;
    unsigned int rate;
    unsigned int start;
    unsigned int length;
};

struct RIFF_strh {
    unsigned char type[4];
    unsigned char handler[4];
    unsigned int flags;
    unsigned int priority;
    unsigned int res0;
    unsigned int scale;
    unsigned int rate;
    unsigned int start;
    unsigned int length;
    unsigned int bufsize;
    unsigned int quality;
    unsigned int samplesize;
};

struct RIFF_strf_vids {
    unsigned int size;
    unsigned int width;
    unsigned int height;
    unsigned short planes;
    unsigned short bits;
    unsigned char compr[4];
    unsigned int image_size;
    unsigned int xpels_meter;
    unsigned int ypels_meter;
    unsigned int num_colors;
    unsigned int imp_colors;
};

struct AVI_HDR {
    unsigned char riff_id[4];
    unsigned int riff_size;
    unsigned char riff_type[4];
    unsigned char hdrl_list_id[4];
    unsigned int hdrl_size;
    unsigned char hdrl_type[4];
    unsigned char avih_id[4];
    unsigned int avih_size;
    struct RIFF_avih avih;
};

struct AVIX_HDR {
    unsigned char riff_id[4];
    unsigned int riff_size;
    unsigned char riff_type[4];
    unsigned char data_list_id[4];
    unsigned int data_size;
    unsigned char data_type[4];
};

struct AVI_HDR_VIDEO {
    unsigned char strl_list_id[4];
    unsigned int strl_size;
    unsigned char strl_type[4];
    unsigned char strh_id[4];
    unsigned int strh_size;
    struct RIFF_strh strh;
    unsigned char strf_id[4];
    unsigned int strf_size;
    struct RIFF_strf_vids strf;
};

struct AVI_HDR_ODML {
    unsigned char strl_list_id[4];
    unsigned int strl_size;
    unsigned char strl_type[4];
    unsigned char strh_id[4];
    unsigned int strh_size;
    unsigned int nframes;
};

struct AVI_DATA {
    unsigned char data_list_id[4];
    unsigned int data_size;
    unsigned char data_type[4];
};

struct CHUNK_HDR {
    unsigned char id[4];
    unsigned int size;
};

struct AVI_IDX {
    unsigned char id[4];
    unsigned int flags;
    unsigned int offset;
    unsigned int size;
};

/*
 * Structure for UVC control item.
 */

#define CTRL_HAS_MIN 0x0001
#define CTRL_HAS_MAX 0x0002
#define CTRL_HAS_RES 0x0004
#define CTRL_HAS_DEF 0x0008

typedef struct {
    int code;			/* Selector mask and selector, see below. */
    const char *name;		/* Name of control, lower case. */
    int type;			/* Item length in bytes. */
    int count;			/* Number of items. */
    int flags;			/* See CTRL_HAS_* masks above. */
    unsigned char cur[32];	/* Current value of control. */
    unsigned char min[32];	/* Minimum value, read on open. */
    unsigned char max[32];	/* Maximum value, read on open. */
    unsigned char res[32];	/* Resolution, read on open. */
    unsigned char def[16];	/* Default value, read on open. */
} UCTRL;

/*
 * Structure for UVC frame format item.
 */

typedef struct {
    int width, height;		/* Frame width, height in pixels. */
    int fps;			/* Default frame rate. */
    int iscomp;			/* Compressed format. */
    int bpp;			/* Bits per pixel for uncompressed formats. */
    char fourcc[4];		/* Four character code for format. */
    short fpsList[32];		/* List of supported frame rates. */
    Tcl_DString str;		/* Textual representation. */
} UFMT;

/*
 * Recording states.
 */
#define REC_STOP	0
#define REC_RECPRI	1
#define REC_RECORD	2
#define REC_PAUSEPRI	3
#define REC_PAUSE	4
#define REC_ERROR	5

/*
 * Control structure for libuvc capture.
 */

typedef struct {
    int running;		/* Greater than zero when acquiring. */
    uvc_context_t *ctx;		/* libuvc context. */
    uvc_device_t *dev;		/* UVC device. */
    uvc_device_handle_t *devh;	/* UVC device handle. */
    uvc_frame_t *frame;		/* Last captured frame or NULL. */
    Tcl_Interp *interp;		/* Interpreter for this object. */
    Tcl_ThreadId tid;		/* Thread identifier of interp. */
    Tcl_HashTable evts;		/* Events in flight. */
    int numev;			/* Number events queued. */
    int idle;			/* FrameReady() in do-when-idle. */
    int mirror;			/* Image mirror flags. */
    int rotate;			/* Image rotation in degrees. */
    int width;			/* Requested width. */
    int height;			/* Requested height. */
    int conv;			/* When true, convert early. */
    int fps;			/* Frames per second. */
    int usefmt;			/* Current UVC format index. */
    int iscomp;			/* Compressed format. */
    int greyshift;		/* For GRAY16 to GRAY8 conversion. */
    Tcl_HashTable ctrl;		/* UVC controls. */
    Tcl_HashTable fmts;		/* UVC formats. */
    char devId[32];		/* Device id. */
    Tcl_DString devName;	/* Device name. */
    int cbCmdLen;		/* Initial length of callback command. */
    Tcl_DString cbCmd;		/* Callback command prefix. */
    Tcl_WideInt counters[3];	/* Statistic counters. */

    /* Info for recording to channel (file or socket) follows. */

    int rstate;			/* Recording state. */
    int ruser;			/* True, when user writes frames. */
    Tcl_Channel rchan;		/* Recording channel or NULL. */
    Tcl_DString rbdStr;		/* Frame boundary string. */
    struct timeval rrate;	/* Recording frame rate. */
    struct timeval rtv;		/* Target time for next frame. */
    struct timeval ltv;		/* Time of last frame read. */
    Tcl_Mutex rmutex;		/* Recording mutex. */
    struct {
	Tcl_WideInt nframes;
	Tcl_WideInt nframes0;
	Tcl_WideInt totsize;
	Tcl_WideInt segsize;
	Tcl_WideInt segsize0;
	Tcl_WideInt segstart;
	int hdrsize;
	Tcl_WideInt pos0;
	struct timeval rate;
	struct AVI_HDR avi_hdr;
	struct AVI_HDR_VIDEO avi_hdrv;
	struct AVI_HDR_ODML avi_hdro;
	struct AVI_DATA avi_data;
	int idx_off;
	int curr_idx, num_idx;
	struct AVI_IDX *idx;
    } avi;			/* AVI file writer. */
} TUVC;

typedef struct {
    Tcl_Event hdr;		/* Generic event header. */
    TUVC *tuvc;			/* Pointer to control structure. */
    Tcl_HashEntry *hPtr;	/* For invalidating the event. */
} TUEVT;

/*
 * Per interpreter control structure.
 */

typedef struct {
    int idCount;
    uvc_context_t *ctx;			/* libuvc context. */
    int checkedTk;			/* Non-zero when Tk availability
					 * checked. */
    Tcl_HashTable tuvcc;		/* List of active TUVC instances. */
    Tcl_Encoding enc;			/* UTF-8 encoding. */
#ifdef HAVE_LIBUDEV
    Tcl_Interp *interp;			/* Interpreter for this object. */
    int devsNeedRefresh;		/* True when list needs refresh. */
    Tcl_HashTable devs;			/* List of devices (udev). */
    int cbCmdLen;			/* Init. length of callback command. */
    Tcl_DString cbCmd;			/* Callback command prefix. */
    struct udev *udev;			/* udev instance. */
    struct udev_monitor *udevMon;	/* udev monitor. */
#endif
} TUVCI;

/*
 * UVC controls.
 */

#define UVC_SELECTOR 0xFF0000
#define UVC_SELECTOR_CT 0x010000
#define UVC_SELECTOR_PU 0x020000
#define UVC_SELECTOR_SU 0x030000

static struct {
    int code;
    const char *name;
    int type;
    int count;
} const UvcCtrlInfo[] = {
    { UVC_CT_SCANNING_MODE_CONTROL | UVC_SELECTOR_CT,
      "scanning-mode", 1, 1 },
    { UVC_CT_AE_MODE_CONTROL | UVC_SELECTOR_CT,
      "ae-mode", 1, 1 },
    { UVC_CT_AE_PRIORITY_CONTROL | UVC_SELECTOR_CT,
      "ae-priority", 1, 1 },
    { UVC_CT_EXPOSURE_TIME_ABSOLUTE_CONTROL | UVC_SELECTOR_CT,
      "exposure-time-abs", 4, 1 },
    { UVC_CT_EXPOSURE_TIME_RELATIVE_CONTROL | UVC_SELECTOR_CT,
      "exposure-time-rel", 1, 1 },
    { UVC_CT_FOCUS_ABSOLUTE_CONTROL | UVC_SELECTOR_CT,
      "focus-abs", 2, 1 },
    { UVC_CT_FOCUS_RELATIVE_CONTROL | UVC_SELECTOR_CT,
      "focus-rel", 2, 2 },
    { UVC_CT_FOCUS_SIMPLE_CONTROL | UVC_SELECTOR_CT,
      "focus-simple", 1, 1 },
    { UVC_CT_FOCUS_AUTO_CONTROL | UVC_SELECTOR_CT,
      "focus-auto", 1, 1 },
    { UVC_CT_IRIS_ABSOLUTE_CONTROL | UVC_SELECTOR_CT,
      "iris-abs", 2, 1},
    { UVC_CT_IRIS_RELATIVE_CONTROL | UVC_SELECTOR_CT,
      "iris-rel", 1, 1 },
    { UVC_CT_ZOOM_ABSOLUTE_CONTROL | UVC_SELECTOR_CT,
      "zoom-abs", 2, 1},
    { UVC_CT_ZOOM_RELATIVE_CONTROL | UVC_SELECTOR_CT,
      "zoom-rel", 1, 3 },
    { UVC_CT_PANTILT_ABSOLUTE_CONTROL | UVC_SELECTOR_CT,
      "pantilt-abs", 4, 2 },
    { UVC_CT_PANTILT_RELATIVE_CONTROL | UVC_SELECTOR_CT,
      "pantilt-rel", 1, 4 },
    { UVC_CT_ROLL_ABSOLUTE_CONTROL | UVC_SELECTOR_CT,
      "roll-abs", 2, 1 },
    { UVC_CT_ROLL_RELATIVE_CONTROL | UVC_SELECTOR_CT,
      "roll-rel", 1, 2 },
    { UVC_CT_PRIVACY_CONTROL | UVC_SELECTOR_CT,
      "privacy", 1, 1 },
    { UVC_CT_DIGITAL_WINDOW_CONTROL | UVC_SELECTOR_CT,
      "digital-window", 2, 6 },
    { UVC_CT_REGION_OF_INTEREST_CONTROL | UVC_SELECTOR_CT,
      "roi", 2, 5 },
    { UVC_PU_BACKLIGHT_COMPENSATION_CONTROL | UVC_SELECTOR_PU,
      "backlight-compensation", 2, 1 },
    { UVC_PU_BRIGHTNESS_CONTROL | UVC_SELECTOR_PU,
      "brightness", 2, 1 },
    { UVC_PU_CONTRAST_CONTROL | UVC_SELECTOR_PU,
      "contrast", 2, 1 },
    { UVC_PU_CONTRAST_AUTO_CONTROL | UVC_SELECTOR_PU,
      "contrast-auto", 1, 1 },
    { UVC_PU_GAIN_CONTROL | UVC_SELECTOR_PU,
      "gain", 2, 1 },
    { UVC_PU_POWER_LINE_FREQUENCY_CONTROL | UVC_SELECTOR_PU,
      "power-line-frequency", 1, 1 },
    { UVC_PU_HUE_CONTROL | UVC_SELECTOR_PU,
      "hue", 2, 1 },
    { UVC_PU_HUE_AUTO_CONTROL | UVC_SELECTOR_PU,
      "hue-auto", 1, 1 },
    { UVC_PU_SATURATION_CONTROL | UVC_SELECTOR_PU,
      "saturation", 2, 1 },
    { UVC_PU_SHARPNESS_CONTROL | UVC_SELECTOR_PU,
      "sharpness", 2, 1 },
    { UVC_PU_GAMMA_CONTROL | UVC_SELECTOR_PU,
      "gamma", 2, 1 },
    { UVC_PU_WHITE_BALANCE_TEMPERATURE_CONTROL | UVC_SELECTOR_PU,
      "white-balance-temperature", 2, 1 },
    { UVC_PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL | UVC_SELECTOR_PU,
      "white-balance-temperature-auto", 1, 1 },
    { UVC_PU_WHITE_BALANCE_COMPONENT_CONTROL | UVC_SELECTOR_PU,
      "white-balance-component", 2, 2 },
    { UVC_PU_WHITE_BALANCE_COMPONENT_AUTO_CONTROL | UVC_SELECTOR_PU,
      "white-balance-component-auto", 1, 1 },
    { UVC_PU_DIGITAL_MULTIPLIER_CONTROL | UVC_SELECTOR_PU,
      "digital-multiplier", 2, 1 },
    { UVC_PU_DIGITAL_MULTIPLIER_LIMIT_CONTROL | UVC_SELECTOR_PU,
      "digital-multiplier-limit", 2, 1 },
    { UVC_PU_ANALOG_VIDEO_STANDARD_CONTROL | UVC_SELECTOR_PU,
      "analog-video-standard", 1, 1 },
    { UVC_PU_ANALOG_LOCK_STATUS_CONTROL | UVC_SELECTOR_PU,
      "analog-lock-status", 1, 1 },
    { UVC_SU_INPUT_SELECT_CONTROL | UVC_SELECTOR_SU,
      "input-select", 1, 1 }
};

/*
 * Mutex and flag used during initialization etc.
 */

TCL_DECLARE_MUTEX(uvcMutex)
static int uvcInitialized = 0;
static int tip609 = 0;

/*
 * Stuff for dynamic linking libusb-1.0.so.0
 */

static void *libusb = NULL;

struct libusb_dl libusb_dl = { 0 };

#ifdef HAVE_LIBUDEV

/*
 * Stuff for dynamic linking libudev.so.
 */

static void *libudev = NULL;

typedef const char *(*fn_device_get_action)(struct udev_device *);
typedef const char *(*fn_device_get_devnode)(struct udev_device *);
typedef const char *(*fn_device_get_property_value)
	(struct udev_device *, const char *);
typedef const char *(*fn_device_get_sysattr_value)
	(struct udev_device *, const char *);
typedef struct udev_device *(*fn_device_new_from_syspath)
	(struct udev *, const char *);
typedef void (*fn_device_unref)(struct udev_device *);
typedef int (*fn_monitor_get_fd)(struct udev_monitor *);
typedef struct udev_device *(*fn_monitor_receive_device)
	(struct udev_monitor *);
typedef void (*fn_monitor_unref)(struct udev_monitor *);
typedef int (*fn_monitor_enable_receiving)(struct udev_monitor *);
typedef struct udev *(*fn_new)(void);
typedef void (*fn_unref)(struct udev *);
typedef int (*fn_monitor_filter_add_match_subsystem_devtype)
	(struct udev_monitor *, const char *, const char *);
typedef struct udev_monitor *(*fn_monitor_new_from_netlink)
	(struct udev *, const char *);
typedef struct udev_enumerate *(*fn_enumerate_new)(struct udev *);
typedef int (*fn_enumerate_add_match_subsystem)
	(struct udev_enumerate *, const char *);
typedef struct udev_list_entry *(*fn_enumerate_get_list_entry)
	(struct udev_enumerate *);
typedef int (*fn_enumerate_scan_devices)(struct udev_enumerate *);
typedef void (*fn_enumerate_unref)(struct udev_enumerate *);
typedef const char *(*fn_list_entry_get_name)(struct udev_list_entry *);
typedef struct udev_list_entry *(*fn_list_entry_get_next)
	(struct udev_list_entry *);

static struct {
    fn_device_get_action device_get_action;
    fn_device_get_devnode device_get_devnode;
    fn_device_get_property_value device_get_property_value;
    fn_device_get_sysattr_value device_get_sysattr_value;
    fn_device_new_from_syspath device_new_from_syspath;
    fn_device_unref device_unref;
    fn_monitor_get_fd monitor_get_fd;
    fn_monitor_receive_device monitor_receive_device;
    fn_monitor_unref monitor_unref;
    fn_monitor_enable_receiving monitor_enable_receiving;
    fn_new new;
    fn_unref unref;
    fn_monitor_filter_add_match_subsystem_devtype
	monitor_filter_add_match_subsystem_devtype;
    fn_monitor_new_from_netlink monitor_new_from_netlink;
    fn_enumerate_new enumerate_new;
    fn_enumerate_add_match_subsystem enumerate_add_match_subsystem;
    fn_enumerate_get_list_entry enumerate_get_list_entry;
    fn_enumerate_scan_devices enumerate_scan_devices;
    fn_enumerate_unref enumerate_unref;
    fn_list_entry_get_name list_entry_get_name;
    fn_list_entry_get_next list_entry_get_next;
} udev_dl;

#define udev_device_get_action udev_dl.device_get_action
#define udev_device_get_devnode udev_dl.device_get_devnode
#define udev_device_get_property_value udev_dl.device_get_property_value
#define udev_device_get_sysattr_value udev_dl.device_get_sysattr_value
#define udev_device_new_from_syspath udev_dl.device_new_from_syspath
#define udev_device_unref udev_dl.device_unref
#define udev_monitor_get_fd udev_dl.monitor_get_fd
#define udev_monitor_receive_device udev_dl.monitor_receive_device
#define udev_monitor_unref udev_dl.monitor_unref
#define udev_new udev_dl.new
#define udev_unref udev_dl.unref
#define udev_monitor_enable_receiving udev_dl.monitor_enable_receiving
#define udev_monitor_filter_add_match_subsystem_devtype \
    udev_dl.monitor_filter_add_match_subsystem_devtype
#define udev_monitor_new_from_netlink udev_dl.monitor_new_from_netlink
#define udev_enumerate_new udev_dl.enumerate_new
#define udev_enumerate_add_match_subsystem \
    udev_dl.enumerate_add_match_subsystem
#define udev_enumerate_get_list_entry udev_dl.enumerate_get_list_entry
#define udev_enumerate_scan_devices udev_dl.enumerate_scan_devices
#define udev_enumerate_unref udev_dl.enumerate_unref
#define udev_list_entry_get_name udev_dl.list_entry_get_name
#define udev_list_entry_get_next udev_dl.list_entry_get_next

#endif

/*
 * Forward declarations.
 */

static int		CheckForTk(TUVCI *tuvci, Tcl_Interp *interp);
static void		CloseAVISegment(TUVC *tuvc, int end);
#ifdef LIBUVC_HAVE_JPEG
static uvc_frame_t *	FrameToJPEG(uvc_frame_t *in, int greyshift);
#endif
static int		WriteFrame(TUVC *tuvc, uvc_frame_t *frame);
static int		StartRecording(TUVC *tuvc, Tcl_Interp *interp,
				       int objc, Tcl_Obj * const objv[]);
static void		WriteAVIHeader(TUVC *tuvc, int end);
static void		FinishRecording(TUVC *tuvc, int lock, int final);
static int		RecordFrameFromData(TUVC *tuvc, Tcl_Interp *interp,
					    int objc, Tcl_Obj * const objv[]);
static int		DataToPhoto(TUVCI *tuvci, Tcl_Interp *interp,
				    int objc, Tcl_Obj * const objv[]);
static void		FrameCallback(uvc_frame_t *frame, void *arg);
static void		FrameReady(ClientData clientData);
static int		FrameReady0(Tcl_Event *evPtr, int flags);
static int		StopCapture(TUVC *tuvc);
static int		StartCapture(TUVC *tuvc);
static int		GetImage(TUVCI *tuvci, TUVC *tuvc, Tcl_Obj *arg);
static void		InitControls(TUVC *tuvc);
static void		GetControls(TUVC *tuvc, Tcl_Obj *list);
static void		PrintVal(UCTRL *uctrl, unsigned char *data,
				 Tcl_DString *dsPtr, Tcl_Obj *list);
static int		SetControls(TUVC *tuvc, int objc,
				    Tcl_Obj * const objv[]);
static void		UvcObjCmdDeleted(ClientData clientData);
static int		UvcObjCmd(ClientData clientData, Tcl_Interp *interp,
				  int objc, Tcl_Obj * const objv[]);
#ifdef HAVE_LIBUDEV
static char *		UdevUVCName(TUVCI *tuvci, struct udev_device *dev,
				    Tcl_DString *dsPtr, Tcl_DString *ds2Ptr);
static void		DecodeProp(TUVCI *tuvci, Tcl_DString *dsPtr,
				   const char *val);
static void		UdevMonitor(ClientData clientData, int mask);
static void		UdevScan(TUVCI *tuvci, struct udev_enumerate *udevEnum);
#endif


#ifdef HAVE_LIBUDEV
/*
 *-------------------------------------------------------------------------
 *
 * UdevUVCName, DecodeProp --
 *
 *	Given udev_device pointer make UVC name in caller
 *	provided Tcl_DString(s). Returns UVC name as string or NULL.
 *
 *-------------------------------------------------------------------------
 */

static char *
UdevUVCName(TUVCI *tuvci, struct udev_device *dev,
	    Tcl_DString *dsPtr, Tcl_DString *ds2Ptr)
{
    const char *val, *busStr, *devStr;
    int idVendor, idProduct;
    char buffer[128];

    val = udev_device_get_property_value(dev, "ID_USB_INTERFACES");
    if ((val == NULL) || (strstr(val, ":0e02") == NULL)) {
	/* not an UVC device */
	return NULL;
    }
    val = udev_device_get_property_value(dev, "ID_VENDOR_ID");
    if ((val == NULL) || (sscanf(val, "%x", &idVendor) != 1)) {
	return NULL;
    }
    val = udev_device_get_property_value(dev, "ID_MODEL_ID");
    if ((val == NULL) || (sscanf(val, "%x", &idProduct) != 1)) {
	return NULL;
    }
    busStr = udev_device_get_sysattr_value(dev, "busnum");
    devStr = udev_device_get_sysattr_value(dev, "devnum");
    if ((busStr != NULL) && (devStr != NULL)) {
	sprintf(buffer, "%04X:%04X:%s.%s", idVendor, idProduct,
		busStr, devStr);
    } else {
	sprintf(buffer, "%04X:%04X", idVendor, idProduct);
    }
    Tcl_DStringAppend(dsPtr, buffer, -1);
    if (ds2Ptr != NULL) {
	val = udev_device_get_property_value(dev, "ID_VENDOR_ENC");
	if (val != NULL) {
	    DecodeProp(tuvci, ds2Ptr, val);
	}
	Tcl_DStringAppend(ds2Ptr, "\0", 1);
	val = udev_device_get_property_value(dev, "ID_MODEL_ENC");
	if (val != NULL) {
	    DecodeProp(tuvci, ds2Ptr, val);
	}
    }
    return Tcl_DStringValue(dsPtr);
}

static void
DecodeProp(TUVCI *tuvci, Tcl_DString *dsPtr, const char *val)
{
    Tcl_DString raw, enc;

    Tcl_DStringInit(&raw);
    while (*val != '\0') {
	if ((val[0] == '\\') && (val[1] == 'x')) {
	    char buf[4];
	    int ch = 0;

	    strncpy(buf, val + 2, 2);
	    buf[2] = '\0';
	    sscanf(buf, "%x", &ch);
	    if (ch <= 0) {
		ch = '?';
	    }
	    buf[3] = ch;
	    Tcl_DStringAppend(&raw, buf + 3, 1);
	    val += strlen(buf) + 2;
	} else {
	    Tcl_DStringAppend(&raw, val, 1);
	    ++val;
	}
    }
    Tcl_ExternalToUtfDString(tuvci->enc, Tcl_DStringValue(&raw),
			     Tcl_DStringLength(&raw), &enc);
    Tcl_DStringFree(&raw);
    Tcl_DStringAppend(dsPtr, Tcl_DStringValue(&enc),
		      Tcl_DStringLength(&enc));
    Tcl_DStringFree(&enc);
}

/*
 *-------------------------------------------------------------------------
 *
 * UdevMonitor --
 *
 *	File handler for udev events. Depending on plug/unplug
 *	events, the table of devices is updated and the listen
 *	callback command is invoked.
 *
 *-------------------------------------------------------------------------
 */

static void
UdevMonitor(ClientData clientData, int mask)
{
    TUVCI *tuvci = (TUVCI *) clientData;
    Tcl_Interp *interp = tuvci->interp;
    struct udev_device *dev;
    Tcl_HashEntry *hPtr;
    const char *action;
    Tcl_DString ds;
    char *devName = NULL;
    int isNew;

    if (!(mask & TCL_READABLE)) {
	return;
    }
    dev = udev_monitor_receive_device(tuvci->udevMon);
    if (dev == NULL) {
	return;
    }
    action = udev_device_get_action(dev);
    Tcl_DStringInit(&ds);
    if (strcmp(action, "add") == 0) {
	Tcl_DString *dsPtr;

	dsPtr = (Tcl_DString *) ckalloc(sizeof(Tcl_DString));
	Tcl_DStringInit(dsPtr);
	devName = UdevUVCName(tuvci, dev, &ds, dsPtr);
	if (devName != NULL) {
	    hPtr = Tcl_CreateHashEntry(&tuvci->devs,
				       (ClientData) devName, &isNew);
	    if (!isNew) {
		action = NULL;
		Tcl_DStringFree((Tcl_DString *) Tcl_GetHashValue(hPtr));
		ckfree((char *) Tcl_GetHashValue(hPtr));
	    }
	    Tcl_SetHashValue(hPtr, (ClientData) dsPtr);
	} else {
	    Tcl_DStringFree(dsPtr);
	    ckfree((char *) dsPtr);
	    action = NULL;
	}
    } else if (strcmp(action, "remove") == 0) {
	devName = UdevUVCName(tuvci, dev, &ds, NULL);
	hPtr = NULL;
	if (devName != NULL) {
	    hPtr = Tcl_FindHashEntry(&tuvci->devs, (ClientData) devName);
	}
	if (hPtr == NULL) {
	    struct udev_enumerate *udevEnum;
	    struct udev_list_entry *item;
	    Tcl_DString ds2;
	    Tcl_HashTable avail;
	    Tcl_HashSearch search;
	    int found = 0;

	    /*
	     * Sync the table the long way...
	     */
	    udevEnum = udev_enumerate_new(tuvci->udev);
	    if (udevEnum == NULL) {
		action = NULL;
		goto docb;
	    }
	    Tcl_InitHashTable(&avail, TCL_STRING_KEYS);
	    Tcl_DStringInit(&ds2);
	    udev_enumerate_add_match_subsystem(udevEnum, "usb");
	    udev_enumerate_scan_devices(udevEnum);
	    item = udev_enumerate_get_list_entry(udevEnum);
	    while (item != NULL) {
		struct udev_device *dev2 = udev_device_new_from_syspath(
			tuvci->udev, udev_list_entry_get_name(item));

		if (dev2 != NULL) {
		    Tcl_DStringSetLength(&ds2, 0);
		    devName = UdevUVCName(tuvci, dev2, &ds2, NULL);
		    if (devName != NULL) {
			hPtr = Tcl_FindHashEntry(&tuvci->devs,
						 (ClientData) devName);
			if (hPtr != NULL) {
			    Tcl_CreateHashEntry(&avail, (ClientData) devName,
						&isNew);
			}
		    }
		    udev_device_unref(dev2);
		}
		item = udev_list_entry_get_next(item);
	    }
	    udev_enumerate_unref(udevEnum);
	    Tcl_DStringFree(&ds2);
	    hPtr = Tcl_FirstHashEntry(&tuvci->devs, &search);
	    while (hPtr != NULL) {
		devName = (char *) Tcl_GetHashKey(&tuvci->devs, hPtr);
		if (Tcl_FindHashEntry(&avail, (ClientData) devName) == NULL) {
		    /*
		     * This should be the/an orphaned entry.
		     */
		    Tcl_DStringSetLength(&ds, 0);
		    Tcl_DStringAppend(&ds, devName, -1);
		    devName = Tcl_DStringValue(&ds);
		    found = 1;
		    /*
		     * Remove the entry, finally.
		     */
		    Tcl_DStringFree((Tcl_DString *) Tcl_GetHashValue(hPtr));
		    ckfree((char *) Tcl_GetHashValue(hPtr));
		    Tcl_DeleteHashEntry(hPtr);
		    break;
		}
		hPtr = Tcl_NextHashEntry(&search);
	    }
	    Tcl_DeleteHashTable(&avail);
	    if (!found) {
		action = NULL;
	    }
	    /*
	     * Flag the table to be refreshed later.
	     */
	    tuvci->devsNeedRefresh = 1;
	} else {
	    Tcl_DStringFree((Tcl_DString *) Tcl_GetHashValue(hPtr));
	    ckfree((char *) Tcl_GetHashValue(hPtr));
	    Tcl_DeleteHashEntry(hPtr);
	}
    } else {
	action = NULL;
    }
docb:
    if ((tuvci->cbCmdLen > 0) && (action != NULL) &&
	(interp != NULL) && !Tcl_InterpDeleted(interp)) {
	int ret;

	Tcl_DStringSetLength(&tuvci->cbCmd, tuvci->cbCmdLen);
	Tcl_DStringAppendElement(&tuvci->cbCmd, action);
	if (devName == NULL) {
	    devName = "";
	}
	Tcl_DStringAppendElement(&tuvci->cbCmd, devName);
	Tcl_Preserve((ClientData) interp);
	ret = Tcl_EvalEx(interp, Tcl_DStringValue(&tuvci->cbCmd),
			 Tcl_DStringLength(&tuvci->cbCmd), TCL_EVAL_GLOBAL);
	if (ret != TCL_OK) {
	    Tcl_AddErrorInfo(interp, "\n    (uvc udev monitor)");
	    Tcl_BackgroundException(interp, ret);
	}
	Tcl_Release((ClientData) interp);
    }
    Tcl_DStringFree(&ds);
    udev_device_unref(dev);
}

/*
 *-------------------------------------------------------------------------
 *
 * UdevScan --
 *
 *	Refresh device list using udev.
 *
 *-------------------------------------------------------------------------
 */

static void
UdevScan(TUVCI *tuvci, struct udev_enumerate *udevEnum)
{
    struct udev_list_entry *item;
    struct udev_device *dev;
    Tcl_HashEntry *hPtr;
    Tcl_DString ds;
    char *devName;
    int isNew, needFree = 0;

    tuvci->devsNeedRefresh = 0;
    if (udevEnum == NULL) {
	Tcl_HashSearch search;

	udevEnum = udev_enumerate_new(tuvci->udev);
	if (udevEnum == NULL) {
	    return;
	}
	needFree = 1;
	hPtr = Tcl_FirstHashEntry(&tuvci->devs, &search);
	while (hPtr != NULL) {
	    Tcl_DStringFree((Tcl_DString *) Tcl_GetHashValue(hPtr));
	    ckfree((char *) Tcl_GetHashValue(hPtr));
	    Tcl_DeleteHashEntry(hPtr);
	    hPtr = Tcl_NextHashEntry(&search);
	}
    }
    Tcl_DStringInit(&ds);
    udev_enumerate_add_match_subsystem(udevEnum, "usb");
    udev_enumerate_scan_devices(udevEnum);
    item = udev_enumerate_get_list_entry(udevEnum);
    while (item != NULL) {
	dev = udev_device_new_from_syspath(tuvci->udev,
					   udev_list_entry_get_name(item));
	if (dev != NULL) {
	    Tcl_DString *dsPtr;

	    Tcl_DStringSetLength(&ds, 0);
	    dsPtr = (Tcl_DString *) ckalloc(sizeof(Tcl_DString));
	    Tcl_DStringInit(dsPtr);
	    devName = UdevUVCName(tuvci, dev, &ds, dsPtr);
	    if (devName != NULL) {
		hPtr = Tcl_CreateHashEntry(&tuvci->devs,
					   (ClientData) devName, &isNew);
		if (!isNew) {
		    Tcl_DStringFree((Tcl_DString *) Tcl_GetHashValue(hPtr));
		    ckfree((char *) Tcl_GetHashValue(hPtr));
		}
		Tcl_SetHashValue(hPtr, (ClientData) dsPtr);
	    } else {
		Tcl_DStringFree(dsPtr);
		ckfree((char *) dsPtr);
	    }
	    udev_device_unref(dev);
	}
	item = udev_list_entry_get_next(item);
    }
    if (needFree) {
	udev_enumerate_unref(udevEnum);
    }
    Tcl_DStringFree(&ds);
}
#endif

/*
 *-------------------------------------------------------------------------
 *
 * CheckForTk --
 *
 *	Check availability of Tk. Return standard Tcl error
 *	and appropriate error message if unavailable.
 *
 *-------------------------------------------------------------------------
 */

static int
CheckForTk(TUVCI *tuvci, Tcl_Interp *interp)
{
    if (tuvci->checkedTk > 0) {
	return TCL_OK;
    } else if (tuvci->checkedTk < 0) {
	Tcl_SetResult(interp, "can't find package Tk", TCL_STATIC);
	return TCL_ERROR;
    }
#ifdef USE_TK_STUBS
    if (Tk_InitStubs(interp, "8.4-", 0) == NULL) {
	tuvci->checkedTk = -1;
	return TCL_ERROR;
    }
#else
    if (Tcl_PkgRequire(interp, "Tk", "8.4-", 0) == NULL) {
	tuvci->checkedTk = -1;
	return TCL_ERROR;
    }
#endif
    tuvci->checkedTk = 1;
    return TCL_OK;
}

/*
 *-------------------------------------------------------------------------
 *
 * CloseAVISegment --
 *
 *	Recording: close current and optionally start next 2G AVI
 *	segment in output file.
 *
 *-------------------------------------------------------------------------
 */

static void
CloseAVISegment(TUVC *tuvc, int end)
{
    Tcl_Size toWrite = 0, written = 0;
    Tcl_WideInt pos;
    struct AVIX_HDR xhdr;
    static const struct AVIX_HDR xhdr0 = {
	{ 'R', 'I', 'F', 'F' },
	0,
	{ 'A', 'V', 'I', 'X' },
	{ 'L', 'I', 'S', 'T' },
	0,
	{ 'm', 'o', 'v', 'i' }
    };

    pos = Tcl_Seek(tuvc->rchan, 0, SEEK_CUR);
    if (tuvc->avi.totsize > tuvc->avi.segsize) {
	Tcl_Seek(tuvc->rchan, tuvc->avi.segstart, SEEK_SET);
	xhdr = xhdr0;
	PUT32LE(&xhdr.riff_size, tuvc->avi.segsize + 16);
	PUT32LE(&xhdr.data_size, tuvc->avi.segsize + 4);
	toWrite = sizeof(xhdr);
	written = Tcl_WriteRaw(tuvc->rchan, (const char *) &xhdr, toWrite);
	Tcl_Seek(tuvc->rchan, pos, SEEK_SET);
    } else {
	tuvc->avi.nframes0 = tuvc->avi.nframes;
	tuvc->avi.segsize0 = tuvc->avi.segsize;
	WriteAVIHeader(tuvc, 0);
	if (tuvc->rstate == REC_ERROR) {
	    return;
	}
    }
    tuvc->avi.segsize = 0;
    tuvc->avi.segstart = pos;
    if (!end && (written == toWrite)) {
	xhdr = xhdr0;
	toWrite = sizeof(xhdr);
	written = Tcl_WriteRaw(tuvc->rchan, (const char *) &xhdr, toWrite);
    }
    if (written != toWrite) {
	tuvc->rstate = REC_ERROR;
    }
}

#ifdef LIBUVC_HAVE_JPEG
/*
 *-------------------------------------------------------------------------
 *
 * FrameToJPEG --
 *
 *	Convert frame to JPEG. Input frame must not be JPEG yet.
 *	Returns allocated and populated new frame or NULL on error.
 *
 *-------------------------------------------------------------------------
 */

static uvc_frame_t *
FrameToJPEG(uvc_frame_t *in, int greyshift)
{
    uvc_frame_t *out, *tmpFrame = in;
    uvc_error_t uret;

    if (in->frame_format == UVC_FRAME_FORMAT_MJPEG) {
	return NULL;
    }
    if (in->frame_format == UVC_FRAME_FORMAT_GRAY16) {
	tmpFrame = uvc_allocate_frame(in->width * in->height);
	if (tmpFrame == NULL) {
	    return NULL;
	}
	uret = uvc_gray16to8(in, tmpFrame, greyshift);
	if (uret) {
	    uvc_free_frame(tmpFrame);
	    return NULL;
	}
    } else if ((in->frame_format != UVC_FRAME_FORMAT_RGB) &&
	       (in->frame_format != UVC_FRAME_FORMAT_GRAY8)) {
	tmpFrame = uvc_allocate_frame(in->width * in->height * 3);
	if (tmpFrame == NULL) {
	    return NULL;
	}
	uret = uvc_any2rgb(in, tmpFrame);
	if (uret) {
	    uvc_free_frame(tmpFrame);
	    return NULL;
	}
    }
    out = uvc_allocate_frame(tmpFrame->data_bytes);
    if (out == NULL) {
	if (tmpFrame != in) {
	    uvc_free_frame(tmpFrame);
	}
	return NULL;
    }
    uret = uvc_rgb2mjpeg(tmpFrame, out);
    if (tmpFrame != in) {
	uvc_free_frame(tmpFrame);
    }
    if (uret) {
	uvc_free_frame(out);
	return NULL;
    }
    return out;
}
#endif

/*
 *-------------------------------------------------------------------------
 *
 * WriteFrame --
 *
 *	Recording: write given frame onto recording output channel.
 *	When called from the libuvc thread, the TUVC.rmutex should
 *	have been acquired by the caller to ensure the output
 *	channel stays valid. The recording frame rate may differ
 *	from the hardware frame rate. Thus, some time calculation
 *	takes place here to write a frame when time is due to the
 *	configured recording frame rate. The result is 1 if a
 *	frame was written, 0 if skipped due to timing constraints,
 *	or -1 on write error.
 *
 *-------------------------------------------------------------------------
 */

static int
WriteFrame(TUVC *tuvc, uvc_frame_t *frame)
{
    Tcl_Size toWrite, written, fWritten;
    struct timeval now, diff;
    uvc_frame_t *newFrame = NULL;

    if (tuvc->rchan == NULL) {
	tuvc->rstate = REC_ERROR;
    }
    gettimeofday(&now, NULL);
    diff.tv_sec = now.tv_sec - frame->capture_time.tv_sec;
    diff.tv_usec = now.tv_usec - frame->capture_time.tv_usec;
    if (diff.tv_usec < 0) {
	diff.tv_sec -= 1;
	diff.tv_usec += 1000000;
    }
    if ((diff.tv_sec > 0) || ((diff.tv_sec == 0) && (diff.tv_usec > 0))) {
	now = frame->capture_time;
    } else {
	/* Clock went back. */
    }
    diff.tv_sec = tuvc->rtv.tv_sec - now.tv_sec;
    diff.tv_usec = tuvc->rtv.tv_usec - now.tv_usec;
    if (diff.tv_usec < 0) {
	diff.tv_sec -= 1;
	diff.tv_usec += 1000000;
    }
    if ((diff.tv_sec > 0) || ((diff.tv_sec == 0) && (diff.tv_usec > 0))) {
	return (tuvc->rstate == REC_ERROR) ? -1 : 0;
    }
    tuvc->rtv = now;
    diff.tv_sec = tuvc->rtv.tv_sec - tuvc->ltv.tv_sec;
    diff.tv_usec = tuvc->rtv.tv_usec - tuvc->ltv.tv_usec;
    if (diff.tv_usec < 0) {
	diff.tv_sec -= 1;
	diff.tv_usec += 1000000;
    }
    tuvc->ltv = tuvc->rtv;

    tuvc->rtv.tv_sec += tuvc->rrate.tv_sec;
    tuvc->rtv.tv_usec += tuvc->rrate.tv_usec;
    if (tuvc->rtv.tv_usec > 1000000) {
	tuvc->rtv.tv_sec += 1;
	tuvc->rtv.tv_usec -= 1000000;
    }
    if (frame->data_bytes == 0) {
	return 0;
    }
    if (tuvc->rstate == REC_ERROR) {
	return -1;
    }
    if (Tcl_DStringLength(&tuvc->rbdStr) > 0) {
#ifndef LIBUVC_HAVE_JPEG
	tuvc->rstate = REC_ERROR;
	return -1;
#else
	int n;
	char buffer[256];

	/*
	 * HTTP MJPEG streaming webcam mode.
	 */
	if (frame->frame_format != UVC_FRAME_FORMAT_MJPEG) {
	    newFrame = FrameToJPEG(frame, tuvc->greyshift);
	    if (newFrame == NULL) {
		tuvc->rstate = REC_ERROR;
		return -1;
	    }
	    frame = newFrame;
	}
	n = Tcl_DStringLength(&tuvc->rbdStr);
	sprintf(buffer, "\r\nContent-type: image/jpeg\r\n"
		"Content-length: %d\r\n\r\n", (int) frame->data_bytes);
	Tcl_DStringAppend(&tuvc->rbdStr, buffer, -1);
	toWrite = Tcl_DStringLength(&tuvc->rbdStr);
	written = Tcl_WriteRaw(tuvc->rchan, Tcl_DStringValue(&tuvc->rbdStr),
			       toWrite);
	Tcl_DStringSetLength(&tuvc->rbdStr, n);
	if (written == toWrite) {
	    toWrite = frame->data_bytes;
	    written = Tcl_WriteRaw(tuvc->rchan, (const char *) frame->data,
				   toWrite);
	}
#endif
    } else {
	/*
	 * AVI file.
	 */
	int size, sizea;
	struct CHUNK_HDR hdr;
	static const struct CHUNK_HDR hdr0 = {
	    { '0', '0', 'd', 'b' },
	    0
	};

#ifdef LIBUVC_HAVE_JPEG
	if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
	    size = frame->data_bytes;
	} else if (memcmp(&tuvc->avi.avi_hdrv.strh.handler, "MJPG", 4) == 0) {
	    newFrame = FrameToJPEG(frame, tuvc->greyshift);
	    if (newFrame == NULL) {
		tuvc->rstate = REC_ERROR;
		return -1;
	    }
	    frame = newFrame;
	    size = frame->data_bytes;
	} else
#endif
	{
	    size = frame->height * frame->step;
	}
	sizea = (size + 3) & ~3;
	hdr = hdr0;
	PUT32LE(&hdr.size, sizea);
	fWritten = 0;
	toWrite = sizeof(hdr);
	written = Tcl_WriteRaw(tuvc->rchan, (const char *) &hdr, toWrite);
	if (written == toWrite) {
	    toWrite = size;
	    written = Tcl_WriteRaw(tuvc->rchan, (const char *) frame->data,
				   toWrite);
	    fWritten = written;
	}

	/* Align to next 32 bit boundary. */
	if ((written == toWrite) && (sizea > size)) {
	    static const char four0[4] = {
		0, 0, 0, 0
	    };

	    toWrite = sizea - size;
	    written = Tcl_WriteRaw(tuvc->rchan, four0, toWrite);
	}

	tuvc->avi.nframes++;
	tuvc->avi.totsize += sizea + sizeof(hdr);
	tuvc->avi.segsize += sizea + sizeof(hdr);

	if (fWritten == size) {
	    if (tuvc->avi.segsize > 0x7F000000) {
		CloseAVISegment(tuvc, 0);
		tuvc->avi.curr_idx = tuvc->avi.num_idx = 0;
		if (tuvc->avi.idx != NULL) {
		    ckfree((char *) tuvc->avi.idx);
		    tuvc->avi.idx = NULL;
		}
	    } else if (tuvc->avi.totsize == tuvc->avi.segsize) {
		/* Add index entry. */
		if (tuvc->avi.curr_idx >= tuvc->avi.num_idx) {
		    int newsize = tuvc->avi.num_idx + 512;
		    struct AVI_IDX *newidx;

		    newidx = attemptckrealloc((char *) tuvc->avi.idx,
					      newsize * sizeof(struct AVI_IDX));
		    if (newidx == NULL) {
			tuvc->avi.curr_idx = tuvc->avi.num_idx = 0;
			if (tuvc->avi.idx != NULL) {
			    ckfree((char *) tuvc->avi.idx);
			    tuvc->avi.idx = NULL;
			}
		    } else {
			tuvc->avi.num_idx = newsize;
			tuvc->avi.idx = newidx;
		    }
		}
		if (tuvc->avi.idx != NULL) {
		    struct AVI_IDX *idx = tuvc->avi.idx + tuvc->avi.curr_idx;

		    memcpy(idx->id, hdr0.id, sizeof(hdr0.id));
		    PUT32LE(&idx->flags, 0);
		    PUT32LE(&idx->offset, tuvc->avi.idx_off);
		    PUT32LE(&idx->size, sizea);
		    tuvc->avi.curr_idx++;
		    tuvc->avi.idx_off += sizea + sizeof(struct CHUNK_HDR);
		}
	    }
	}

	/* Compute average frame rate. */
	if (tuvc->avi.nframes == 0) {
	    tuvc->avi.rate = diff;
	} else {
	    tuvc->avi.rate.tv_sec += diff.tv_sec;
	    tuvc->avi.rate.tv_sec /= 2;
	    tuvc->avi.rate.tv_usec += diff.tv_usec;
	    tuvc->avi.rate.tv_usec /= 2;
	}
    }
    if (written != toWrite) {
	tuvc->rstate = REC_ERROR;
    }
    if (newFrame != NULL) {
	uvc_free_frame(newFrame);
    }
    return (tuvc->rstate == REC_ERROR) ? -1 : 1;
}

/*
 *-------------------------------------------------------------------------
 *
 * StartRecording --
 *
 *	Prepare recording from given parameters (channel, mode, etc.)
 *	The channel used for writing frames is detached from the
 *	calling interpreter and thus can be used by the libuvc
 *	thread. However, proper guarding against race conditions
 *	is required using the UVC.rmutex when writing a frame and
 *	when closing the channel. Timing computations for adapting
 *	the recording frame rate are performed.
 *
 *-------------------------------------------------------------------------
 */

static int
StartRecording(TUVC *tuvc, Tcl_Interp *interp,
	       int objc, Tcl_Obj * const objv[])
{
    int i, mode, doMJPG = 0, doUser = 0;
    double rate = 0;
    const char *p, *rbdStr = NULL;
    Tcl_Channel chan = NULL, stack[2];
    Tcl_HashEntry *hPtr;
    long li;
    UFMT *ufmt;
    Tcl_WideInt pos0 = 0;

    if (objc < 5) {
	Tcl_WrongNumArgs(interp, 2, objv, "devid start ...");
	return TCL_ERROR;
    }
    for (i = 4; i < objc; i++) {
	p = Tcl_GetString(objv[i]);
	if (strcmp(p, "-mjpeg") == 0) {
#ifndef LIBUVC_HAVE_JPEG
	    Tcl_SetResult(interp, "-mjpeg is not supported",
			  TCL_STATIC);
	    return TCL_ERROR;
#else
	    doMJPG++;
#endif
	} else if (strcmp(p, "-user") == 0) {
	    doMJPG++;
	    doUser++;
	} else if (strcmp(p, "-fps") == 0) {
	    if (++i >= objc) {
		Tcl_SetResult(interp, "-fps option needs a value",
			      TCL_STATIC);
		return TCL_ERROR;
	    }
	    if (Tcl_GetDoubleFromObj(interp, objv[i], &rate) != TCL_OK) {
		return TCL_ERROR;
	    }
	} else if (strcmp(p, "-boundary") == 0) {
	    if (++i >= objc) {
		Tcl_SetResult(interp, "-boundary option needs a value",
			      TCL_STATIC);
		return TCL_ERROR;
	    }
#ifndef LIBUVC_HAVE_JPEG
	    Tcl_SetResult(interp, "-boundary is not supported",
			  TCL_STATIC);
	    return TCL_ERROR;
#else
	    rbdStr = Tcl_GetString(objv[i]);
#endif
	} else if (strcmp(p, "-chan") == 0) {
	    if (++i >= objc) {
		Tcl_SetResult(interp, "-chan option needs a value",
			      TCL_STATIC);
		return TCL_ERROR;
	    }
	    chan = Tcl_GetChannel(interp, Tcl_GetString(objv[i]), &mode);
	    if (chan == NULL) {
		return TCL_ERROR;
	    }
	    if ((mode & TCL_WRITABLE) == 0) {
		Tcl_SetResult(interp, "channel is not writable",
			      TCL_STATIC);
		return TCL_ERROR;
	    }
	}
    }
    li = tuvc->usefmt;
    hPtr = Tcl_FindHashEntry(&tuvc->fmts, (ClientData) li);
    if (hPtr == NULL) {
	Tcl_SetResult(interp, "unsupported format", TCL_STATIC);
	return TCL_ERROR;
    }
    ufmt = (UFMT *) Tcl_GetHashValue(hPtr);
    if (chan == NULL) {
	Tcl_SetResult(interp, "no channel given", TCL_STATIC);
	return TCL_ERROR;
    }
    stack[0] = Tcl_GetTopChannel(chan);
    stack[1] = Tcl_GetStackedChannel(chan);
    if (((stack[0] != NULL) && (stack[0] != chan)) || (stack[1] != NULL)) {
	Tcl_SetResult(interp, "stacked channels are not supported", TCL_STATIC);
	return TCL_ERROR;
    }
    if ((Tcl_SetChannelOption(interp, chan, "-blocking", "0") != TCL_OK) ||
	(Tcl_SetChannelOption(interp, chan, "-buffering", "none") != TCL_OK) ||
	(Tcl_SetChannelOption(interp, chan, "-translation", "binary")
	 != TCL_OK)) {
	return TCL_ERROR;
    }
    if ((rbdStr == NULL) || (strlen(rbdStr) == 0)) {
	pos0 = Tcl_Seek(chan, 0, SEEK_CUR);
	if (pos0 == (Tcl_WideInt) -1) {
	    Tcl_SetResult(interp, "not a random access channel", TCL_STATIC);
	    return TCL_ERROR;
	}
    }
    if (Tcl_DetachChannel(interp, chan) != TCL_OK) {
	Tcl_SetResult(interp, "cannot detach channel", TCL_STATIC);
	return TCL_ERROR;
    }
    Tcl_MutexLock(&tuvc->rmutex);
    FinishRecording(tuvc, 0, 0);
    tuvc->rchan = chan;
    if ((rate > 0.0) && (rate < tuvc->fps)) {
	tuvc->rrate.tv_sec = 1.0 / rate;
	tuvc->rrate.tv_usec = 1000000.0 / rate;
    } else if (tuvc->fps <= 0) {
	tuvc->rrate.tv_sec = 1;
	tuvc->rrate.tv_usec = 0;
    } else {
	tuvc->rrate.tv_sec = 1 / tuvc->fps;
	tuvc->rrate.tv_usec = 1000000 / tuvc->fps;
    }
    if ((rbdStr != NULL) && (strlen(rbdStr) > 0)) {
	Tcl_DStringAppend(&tuvc->rbdStr, rbdStr, -1);
    } else {
	int n;
	static const struct AVI_HDR avi_hdr = {
	    { 'R', 'I', 'F', 'F' },
	    0,
	    { 'A', 'V', 'I', ' ' },
	    { 'L', 'I', 'S', 'T' },
	    0,
	    { 'h', 'd', 'r', 'l' },
	    { 'a', 'v', 'i', 'h' },
	    0,
	    { }
	};
	static const struct AVI_HDR_VIDEO avi_hdrv = {
	    { 'L', 'I', 'S', 'T' },
	    0,
	    { 's', 't', 'r', 'l' },
	    { 's', 't', 'r', 'h' },
	    0,
	    {
		{ 'v', 'i', 'd', 's' }
	    },
	    { 's', 't', 'r', 'f' },
	    0,
	    { }
	};
	static const struct AVI_HDR_ODML avi_hdro = {
	    { 'L', 'I', 'S', 'T' },
	    0,
	    { 'o', 'd', 'm', 'l' },
	    { 'd', 'm', 'l', 'h' },
	    0,
	    0
	};
	static const struct AVI_DATA avi_data = {
	    { 'L', 'I', 'S', 'T' },
	    0,
	    { 'm', 'o', 'v', 'i' },
	};

	/* Setup AVI writer. */
	tuvc->avi.pos0 = pos0;
	tuvc->avi.avi_hdr = avi_hdr;
	PUT32LE(&tuvc->avi.avi_hdr.avih_size,
		sizeof(struct RIFF_avih));
	tuvc->avi.avi_hdrv = avi_hdrv;
	PUT32LE(&tuvc->avi.avi_hdrv.strl_size,
		sizeof(struct RIFF_strh) +
		sizeof(struct RIFF_strf_vids) + 20);
	PUT32LE(&tuvc->avi.avi_hdrv.strh_size,
		sizeof(struct RIFF_strh));
	PUT32LE(&tuvc->avi.avi_hdrv.strf_size,
		sizeof(struct RIFF_strf_vids));
	tuvc->avi.avi_hdro = avi_hdro;
	PUT32LE(&tuvc->avi.avi_hdro.strl_size,
		sizeof(unsigned int) + 12);
	PUT32LE(&tuvc->avi.avi_hdro.strh_size,
		sizeof(unsigned int));
	tuvc->avi.avi_data = avi_data;

	PUT32LE(&tuvc->avi.avi_hdr.avih.width, ufmt->width);
	PUT32LE(&tuvc->avi.avi_hdr.avih.height, ufmt->height);
	n = tuvc->rrate.tv_sec * 1000000 + tuvc->rrate.tv_usec;
	PUT32LE(&tuvc->avi.avi_hdr.avih.uspf, n);
	if (ufmt->iscomp || doMJPG) {
	    n = 24 * n / 1000;
	} else {
	    n = ufmt->bpp * n / 1000;
	}
	n = n * ufmt->width * ufmt->height;
	PUT32LE(&tuvc->avi.avi_hdr.avih.bps, n);
	PUT32LE(&tuvc->avi.avi_hdr.avih.nstreams, 1);
	tuvc->avi.hdrsize = Tcl_WriteRaw(tuvc->rchan,
					 (const char *) &tuvc->avi.avi_hdr,
					 sizeof(tuvc->avi.avi_hdr));
	if (ufmt->iscomp || doMJPG) {
	    memcpy(&tuvc->avi.avi_hdrv.strh.handler, "MJPG", 4);
	    memcpy(&tuvc->avi.avi_hdrv.strf.compr, "MJPG", 4);
	} else {
	    memcpy(&tuvc->avi.avi_hdrv.strh.handler, ufmt->fourcc, 4);
	    memcpy(&tuvc->avi.avi_hdrv.strf.compr, ufmt->fourcc, 4);
	}
	n = tuvc->rrate.tv_sec * 1000000 + tuvc->rrate.tv_usec;
	PUT32LE(&tuvc->avi.avi_hdrv.strh.scale, n);
	PUT32LE(&tuvc->avi.avi_hdrv.strh.rate, 1000000);
	PUT32LE(&tuvc->avi.avi_hdrv.strf.size, sizeof(tuvc->avi.avi_hdrv.strf));
	PUT32LE(&tuvc->avi.avi_hdrv.strf.width, ufmt->width);
	PUT32LE(&tuvc->avi.avi_hdrv.strf.height, ufmt->height);
	PUT16LE(&tuvc->avi.avi_hdrv.strf.planes, 1);
	PUT16LE(&tuvc->avi.avi_hdrv.strf.bits, ufmt->bpp);
	n = ufmt->bpp * ufmt->width * ufmt->height;
	PUT32LE(&tuvc->avi.avi_hdrv.strf.image_size, n);
	tuvc->avi.hdrsize += Tcl_WriteRaw(tuvc->rchan,
					  (const char *) &tuvc->avi.avi_hdrv,
					  sizeof(tuvc->avi.avi_hdrv));
	tuvc->avi.hdrsize += Tcl_WriteRaw(tuvc->rchan,
					  (const char *) &tuvc->avi.avi_hdro,
					  sizeof(tuvc->avi.avi_hdro));
	Tcl_WriteRaw(tuvc->rchan, (const char *) &tuvc->avi.avi_data,
		     sizeof(tuvc->avi.avi_data));
	tuvc->avi.segsize0 = 4;
	WriteAVIHeader(tuvc, 0);
	tuvc->avi.curr_idx = tuvc->avi.num_idx = 0;
	tuvc->avi.idx_off = 4;
	if (tuvc->avi.idx != NULL) {
	    ckfree((char *) tuvc->avi.idx);
	    tuvc->avi.idx = NULL;
	}
    }
    /* Reserve 500us for processing. */
    tuvc->rrate.tv_usec -= 500;
    if (tuvc->rrate.tv_usec < 0) {
	tuvc->rrate.tv_sec -= 1;
	tuvc->rrate.tv_usec += 1000000;
    }
    gettimeofday(&tuvc->ltv, NULL);
    tuvc->rtv = tuvc->ltv;
    if (doUser) {
	tuvc->ruser = 1;
	tuvc->rstate = tuvc->running ? REC_RECORD : REC_PAUSE;
    } else {
	tuvc->ruser = 0;
	if (tuvc->running) {
	    tuvc->rstate = tuvc->conv ? REC_RECPRI : REC_RECORD;
	} else {
	    tuvc->rstate = tuvc->conv ? REC_PAUSEPRI : REC_PAUSE;
	}
    }
    Tcl_MutexUnlock(&tuvc->rmutex);
    return TCL_OK;
}

/*
 *-------------------------------------------------------------------------
 *
 * WriteAVIHeader --
 *
 *	(Re)write AVI file header with current chunk sizes at
 *	the very begin of the AVI file.
 *
 *-------------------------------------------------------------------------
 */

static void
WriteAVIHeader(TUVC *tuvc, int end)
{
    int size, idx_size;
    Tcl_WideInt pos;

    if (end && (tuvc->avi.idx != NULL)) {
	/* Write index. */
	struct CHUNK_HDR idxh;
	static const struct CHUNK_HDR idxh0 = {
	    { 'i', 'd', 'x', '1' },
	    0
	};

	idxh = idxh0;
	idx_size = tuvc->avi.curr_idx * sizeof(struct AVI_IDX);
	PUT32LE(&idxh.size, idx_size);
	Tcl_WriteRaw(tuvc->rchan, (const char *) &idxh, sizeof(idxh));
	Tcl_WriteRaw(tuvc->rchan, (const char *) tuvc->avi.idx, idx_size);

	/* Mark index present. */
	PUT32LE(&tuvc->avi.avi_hdr.avih.flags, 0x10);
	idx_size += sizeof(struct CHUNK_HDR);
    } else {
	/* Mark index absent. */
	PUT32LE(&tuvc->avi.avi_hdr.avih.flags, 0);
	idx_size = 0;
    }

    /* For MJPG use computed average frame rate. */
    if (memcmp(&tuvc->avi.avi_hdrv.strh.handler, "MJPG", 4) == 0) {
	int n;

	n = tuvc->avi.rate.tv_sec * 1000000 + tuvc->avi.rate.tv_usec;
	PUT32LE(&tuvc->avi.avi_hdr.avih.uspf, n);
	PUT32LE(&tuvc->avi.avi_hdrv.strh.scale, n);
    }
    size = tuvc->avi.hdrsize + tuvc->avi.segsize0;
    PUT32LE(&tuvc->avi.avi_hdr.riff_size, size + idx_size);
    size = tuvc->avi.hdrsize - 20;
    PUT32LE(&tuvc->avi.avi_hdr.hdrl_size, size);
    size = tuvc->avi.nframes0;
    PUT32LE(&tuvc->avi.avi_hdr.avih.nframes, size);
    PUT32LE(&tuvc->avi.avi_hdrv.strh.length, size);
    size = tuvc->avi.segsize0 + 4;
    PUT32LE(&tuvc->avi.avi_data.data_size, size);
    size = tuvc->avi.nframes;
    PUT32LE(&tuvc->avi.avi_hdro.nframes, size);

    pos = Tcl_Seek(tuvc->rchan, 0, SEEK_CUR);
    Tcl_Seek(tuvc->rchan, tuvc->avi.pos0, SEEK_SET);
    Tcl_WriteRaw(tuvc->rchan, (const char *) &tuvc->avi.avi_hdr,
		 sizeof(tuvc->avi.avi_hdr));
    Tcl_WriteRaw(tuvc->rchan, (const char *) &tuvc->avi.avi_hdrv,
		 sizeof(tuvc->avi.avi_hdrv));
    Tcl_WriteRaw(tuvc->rchan, (const char *) &tuvc->avi.avi_hdro,
		 sizeof(tuvc->avi.avi_hdro));
    Tcl_WriteRaw(tuvc->rchan, (const char *) &tuvc->avi.avi_data,
		 sizeof(tuvc->avi.avi_data));
    if (Tcl_Seek(tuvc->rchan, pos, SEEK_SET) == (Tcl_WideInt) -1) {
	tuvc->rstate = REC_ERROR;
    }

    if (end) {
	tuvc->avi.curr_idx = tuvc->avi.num_idx = 0;
	if (tuvc->avi.idx != NULL) {
	    ckfree((char *) tuvc->avi.idx);
	    tuvc->avi.idx = NULL;
	}
    }
}

/*
 *-------------------------------------------------------------------------
 *
 * FinishRecording --
 *
 *	Close recording channel and release resources. Must be
 *	called from the thread which opened the UVC device.
 *	Optionally TUVC.rmutex is locked, optionally it is
 *	finalized when the UVC device gets closed.
 *
 *-------------------------------------------------------------------------
 */

static void
FinishRecording(TUVC *tuvc, int lock, int final)
{
    if (lock) {
	Tcl_MutexLock(&tuvc->rmutex);
    }
    if ((tuvc->rchan != NULL) &&
	(Tcl_DStringLength(&tuvc->rbdStr) == 0)) {
	CloseAVISegment(tuvc, 1);
	WriteAVIHeader(tuvc, 1);
    }
    Tcl_DStringFree(&tuvc->rbdStr);
    if (tuvc->rchan != NULL) {
	Tcl_Close(NULL, tuvc->rchan);
	tuvc->rchan = NULL;
	memset(&tuvc->avi, 0, sizeof(tuvc->avi));
    }
    if (lock) {
	Tcl_MutexUnlock(&tuvc->rmutex);
    }
    if (final) {
	Tcl_MutexFinalize(&tuvc->rmutex);
    }
}

/*
 *-------------------------------------------------------------------------
 *
 * RecordFrameFromData --
 *
 *	Write single frame from byte array data.
 *
 *-------------------------------------------------------------------------
 */

static int
RecordFrameFromData(TUVC *tuvc, Tcl_Interp *interp,
		    int objc, Tcl_Obj * const objv[])
{
    int width, height, bpp, ret;
    Tcl_Size length;
    unsigned char *data;
    long li;
    Tcl_HashEntry *hPtr;
    UFMT *ufmt;
    uvc_frame_t *frame;

    if (objc != 8) {
	Tcl_WrongNumArgs(interp, 2, objv,
			 "devid width height bpp bytearray");
	return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[4], &width) != TCL_OK) {
	return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[5], &height) != TCL_OK) {
	return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[6], &bpp) != TCL_OK) {
	return TCL_ERROR;
    }
    data = Tcl_GetByteArrayFromObj(objv[7], &length);
    li = tuvc->usefmt;
    hPtr = Tcl_FindHashEntry(&tuvc->fmts, li);
    if (hPtr == NULL) {
	Tcl_SetResult(interp, "unsupported format", TCL_STATIC);
	return TCL_ERROR;
    }
    ufmt = (UFMT *) Tcl_GetHashValue(hPtr);
    if ((length < width * height * bpp) ||
	(width != ufmt->width) || (height != ufmt->height)) {
	Tcl_SetResult(interp, "incompatible frame data", TCL_STATIC);
	return TCL_ERROR;
    }
    if (!tuvc->ruser ||
	((tuvc->rstate != REC_RECORD) && (tuvc->rstate != REC_PAUSE))) {
	Tcl_SetResult(interp, "wrong recording state for frame",
		      TCL_STATIC);
	return TCL_ERROR;
    }
    frame = uvc_allocate_frame(0);
    if (frame == NULL) {
	Tcl_SetResult(interp, "out of memory", TCL_STATIC);
	return TCL_ERROR;
    }
    frame->library_owns_data = 0;
    frame->width = width;
    frame->height = height;
    frame->step = width * bpp;
    frame->sequence = 0;
    frame->source = 0;
    frame->data = data;
    frame->data_bytes = length;
    switch (bpp) {
    case 1:
	frame->frame_format = UVC_FRAME_FORMAT_GRAY8;
	break;
    case 2:
	frame->frame_format = UVC_FRAME_FORMAT_GRAY16;
	break;
    default:
	frame->frame_format = UVC_FRAME_FORMAT_RGB;
	break;
    }
    gettimeofday(&frame->capture_time, NULL);
    ret = WriteFrame(tuvc, frame);
    uvc_free_frame(frame);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(ret));
    return TCL_OK;
}

/*
 *-------------------------------------------------------------------------
 *
 * DataToPhoto --
 *
 *	Put byte array data to a Tk photo image.
 *
 *-------------------------------------------------------------------------
 */

static int
DataToPhoto(TUVCI *tuvci, Tcl_Interp *interp,
	    int objc, Tcl_Obj * const objv[])
{
    int width, height, bpp;
    Tcl_Size length;
    int rot = 0, mirx = 0, miry = 0, mirror;
    unsigned char *data;
    Tk_PhotoHandle photo;
    char *name;
    Tk_PhotoImageBlock block;

    if (CheckForTk(tuvci, interp) != TCL_OK) {
	return TCL_ERROR;
    }
    if ((objc < 7) || (objc > 10)) {
	Tcl_WrongNumArgs(interp, 2, objv,
			 "photo width height bpp bytearray "
			 "?rotation mirrorx mirrory?");
	return TCL_ERROR;
    }
    if (Tk_MainWindow(interp) == NULL) {
	Tcl_SetResult(interp, "application has been destroyed",
		      TCL_STATIC);
	return TCL_ERROR;
    }
    name = Tcl_GetString(objv[2]);
    photo = Tk_FindPhoto(interp, name);
    if (photo == NULL) {
	Tcl_SetObjResult(interp,
	    Tcl_ObjPrintf("can't use \"%s\": not a photo image", name));
	return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[3], &width) != TCL_OK) {
	return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[4], &height) != TCL_OK) {
	return TCL_ERROR;
    }
    if (Tcl_GetIntFromObj(interp, objv[5], &bpp) != TCL_OK) {
	return TCL_ERROR;
    }
    if ((objc > 7) && (Tcl_GetIntFromObj(interp, objv[7], &rot) != TCL_OK)) {
	return TCL_ERROR;
    }
    if ((objc > 8) &&
	(Tcl_GetBooleanFromObj(interp, objv[8], &mirx) != TCL_OK)) {
	return TCL_ERROR;
    }
    if ((objc > 9) &&
	(Tcl_GetBooleanFromObj(interp, objv[9], &miry) != TCL_OK)) {
	return TCL_ERROR;
    }
    data = Tcl_GetByteArrayFromObj(objv[6], &length);
    if ((length < width * height * bpp) ||
	((bpp != 1) && (bpp != 3))) {
	Tcl_SetResult(interp, "unsupported data format", TCL_STATIC);
	return TCL_ERROR;
    }
    if (bpp == 1) {
	block.pixelSize = 1;
	block.offset[0] = 0;
	block.offset[1] = 0;
	block.offset[2] = 0;
	block.offset[3] = 1;
    } else {
	block.pixelSize = 3;
	block.offset[0] = 0;
	block.offset[1] = 1;
	block.offset[2] = 2;
	block.offset[3] = 4;
    }
    block.width = width;
    block.height = height;
    block.pitch = width * bpp;
    block.pixelPtr = data;
    mirror = (mirx ? 1 : 0) | (miry ? 2 : 0);
    rot = rot % 360;
    if (rot < 45) {
	rot = 0;
    } else if (rot < 135) {
	rot = 90;
    } else if (rot < 225) {
	rot = 180;
    } else if (rot < 315) {
	rot = 270;
    } else {
	rot = 0;
    }
    if ((mirror & 3) == 3) {
	rot = (rot + 180) % 360;
    }
    switch (rot) {
    case 270:	/* = 90 CW */
	block.pitch = block.pixelSize;
	block.pixelPtr += width * block.pixelSize * (height - 1);
	block.pixelSize *= -width;
	block.offset[3] = block.pixelSize + 1;	/* no alpha */
	block.width = height;
	block.height = width;
	break;
    case 180:	/* = 180 CW */
	block.pitch = -block.pitch;
	block.pixelPtr += (width * height - 1) * block.pixelSize;
	block.pixelSize = -block.pixelSize;
	block.offset[3] = block.pixelSize + 1;	/* no alpha */
	break;
    case 90:	/* = 270 CW */
	block.pitch = -block.pixelSize;
	block.pixelPtr += (width - 1) * block.pixelSize;
	block.pixelSize *= width;
	block.offset[3] = block.pixelSize + 1;	/* no alpha */
	block.width = height;
	block.height = width;
	break;
    }
    if ((mirror & 3) == 2) {
	/* mirror in X */
	block.pixelPtr += (block.width - 1) * block.pixelSize;
	block.pixelSize = -block.pixelSize;
	block.offset[3] = block.pixelSize + 1;      /* no alpha */
    }
    if ((mirror & 3) == 1) {
	/* mirror in Y */
	block.pixelPtr += block.pitch * (block.height - 1);
	block.pitch = -block.pitch;
    }
    if (Tk_PhotoExpand(interp, photo, block.width, block.height) != TCL_OK) {
	return TCL_ERROR;
    }
    if (Tk_PhotoPutBlock(interp, photo, &block, 0, 0, block.width,
			 block.height, TK_PHOTO_COMPOSITE_SET) != TCL_OK) {
	return TCL_ERROR;
    }
    return TCL_OK;
}

/*
 *-------------------------------------------------------------------------
 *
 * FrameCallback --
 *
 *	Invoked by a internal libuvc thread to indicate a frame
 *	ready to be processed further. Frame is converted to RGB
 *	and the interpreter associated with the UVC device woken
 *	up by marking a Tcl_AsyncHandler or by queuing an event.
 *
 *-------------------------------------------------------------------------
 */

static void
FrameCallback(uvc_frame_t *frame, void *arg)
{
    TUVC *tuvc = (TUVC *) arg;
    uvc_frame_t *newFrame;
    uvc_error_t uret;
    TUEVT *event;

    if (tuvc->tid == NULL) {
	/* should never happen */
	return;
    }
    if (tuvc->rstate == REC_RECPRI) {
	Tcl_MutexLock(&tuvc->rmutex);
	WriteFrame(tuvc, frame);
	Tcl_MutexUnlock(&tuvc->rmutex);
    }
    if (tuvc->conv && (frame->frame_format != UVC_FRAME_FORMAT_GRAY8) &&
	(frame->frame_format != UVC_FRAME_FORMAT_RGB)) {
	if (frame->frame_format == UVC_FRAME_FORMAT_GRAY16) {
	    newFrame = uvc_allocate_frame(frame->data_bytes / 2);
	    if (newFrame == NULL) {
		return;
	    }
	    uret = uvc_gray16to8(frame, newFrame, tuvc->greyshift);
	} else {
	    newFrame = uvc_allocate_frame(frame->data_bytes);
	    if (newFrame == NULL) {
		return;
	    }
#ifdef LIBUVC_HAVE_JPEG
	    if (frame->frame_format == UVC_FRAME_FORMAT_MJPEG) {
		uret = uvc_mjpeg2rgb(frame, newFrame);
	    } else
#endif
	    {
		uret = uvc_any2rgb(frame, newFrame);
	    }
	}
	if (uret) {
	    uvc_free_frame(newFrame);
	    return;
	}
    } else {
	newFrame = frame;
    }
    Tcl_MutexLock(&uvcMutex);
    if (tuvc->frame != NULL) {
	uvc_frame_t *oldFrame = tuvc->frame;

	tuvc->frame = newFrame;
	newFrame = oldFrame;
	tuvc->counters[2] += 1;		/* frame dropped */
    } else {
	tuvc->frame = newFrame;
	newFrame = NULL;
    }
    tuvc->counters[0] += 1;
    if ((tuvc->tid != NULL) && (tuvc->numev == 0)) {
	int isNew;

	event = (TUEVT *) ckalloc(sizeof(TUEVT));
	event->hdr.proc = FrameReady0;
	event->hdr.nextPtr = NULL;
	event->tuvc = tuvc;
	event->hPtr =
	    Tcl_CreateHashEntry(&tuvc->evts, (ClientData) event, &isNew);
	if (tip609) {
	    /* TCL_QUEUE_TAIL_ALERT_IF_EMPTY */
	    Tcl_ThreadQueueEvent(tuvc->tid, &event->hdr, TCL_QUEUE_TAIL | 4);
	} else {
	    Tcl_ThreadQueueEvent(tuvc->tid, &event->hdr, TCL_QUEUE_TAIL);
	    Tcl_ThreadAlert(tuvc->tid);
	}
	tuvc->numev++;
    }
    Tcl_MutexUnlock(&uvcMutex);
    if (newFrame != NULL) {
	uvc_free_frame(newFrame);
    }
}

/*
 *-------------------------------------------------------------------------
 *
 * FrameReady, FrameReady0 --
 *
 *	A frame became ready. FrameReady is a do-when-idle handler
 *	triggered by FrameReady0 which is an event callback function
 *	triggered by the FrameCallback function which runs in a
 *	different libuvc controlled thread. The top-level function
 *	invokes the Tcl callback procedure which then can retrieve
 *	the frame buffer using the "uvc image ..." command.
 *
 *-------------------------------------------------------------------------
 */

static void
FrameReady(ClientData clientData)
{
    TUVC *tuvc = (TUVC *) clientData;
    Tcl_Interp *interp = tuvc->interp;
    int ret;

    Tcl_MutexLock(&uvcMutex);
    if (tuvc->idle) {
	tuvc->numev = 0;
    }
    Tcl_MutexUnlock(&uvcMutex);
    if (!tuvc->ruser && (tuvc->rstate == REC_RECORD)) {
	uvc_frame_t *frame;

	Tcl_MutexLock(&uvcMutex);
	frame = tuvc->frame;
	tuvc->frame = NULL;
	Tcl_MutexUnlock(&uvcMutex);
	if (frame != NULL) {
	    WriteFrame(tuvc, frame);
	}
	Tcl_MutexLock(&uvcMutex);
	if ((frame != NULL) && (tuvc->frame == NULL)) {
	    /* Put back last frame */
	    tuvc->frame = frame;
	    frame = NULL;
	}
	Tcl_MutexUnlock(&uvcMutex);
	if (frame != NULL) {
	    uvc_free_frame(frame);
	}
    }
    if (tuvc->frame == NULL) {
	/* should never happen */
	return;
    }
    Tcl_DStringSetLength(&tuvc->cbCmd, tuvc->cbCmdLen);
    Tcl_DStringAppendElement(&tuvc->cbCmd, tuvc->devId);
    Tcl_Preserve((ClientData) interp);
    ret = Tcl_EvalEx(interp, Tcl_DStringValue(&tuvc->cbCmd),
		     Tcl_DStringLength(&tuvc->cbCmd), TCL_EVAL_GLOBAL);
    if (ret != TCL_OK) {
	Tcl_AddErrorInfo(interp, "\n    (uvc event handler)");
	Tcl_BackgroundException(interp, ret);
	StopCapture(tuvc);
    }
    Tcl_Release((ClientData) interp);
}

static int
FrameReady0(Tcl_Event *evPtr, int flags)
{
    TUEVT *tevPtr = (TUEVT *) evPtr;
    TUVC *tuvc = tevPtr->tuvc;
    int doit = 0;

    if (tuvc == NULL) {
	return 1;
    }
    Tcl_MutexLock(&uvcMutex);
    if (tevPtr->hPtr != NULL) {
	Tcl_DeleteHashEntry(tevPtr->hPtr);
    }
    if (tuvc->tid != NULL) {
	if (!tuvc->idle) {
	    tuvc->numev--;
	}
	doit = 1;
    } else {
	tuvc->numev = 0;
    }
    Tcl_MutexUnlock(&uvcMutex);
    if (doit) {
	if (tuvc->idle) {
	    Tcl_CancelIdleCall(FrameReady, (ClientData) tuvc);
	    Tcl_DoWhenIdle(FrameReady, (ClientData) tuvc);
	} else {
	    FrameReady((ClientData) tuvc);
	}
    }
    return 1;
}

/*
 *-------------------------------------------------------------------------
 *
 * StopCapture --
 *
 *	Stop capture if running. UVC streaming is turned off.
 *	A pending idle call to FrameReady is cancelled, too.
 *	The thread id to which further events would be queued
 *	has to be cleared, events in flight are invalidated.
 *
 *-------------------------------------------------------------------------
 */

static int
StopCapture(TUVC *tuvc)
{
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;

    if (tuvc->running > 0) {
	uvc_stop_streaming(tuvc->devh);
	tuvc->tid = NULL;
	Tcl_CancelIdleCall(FrameReady, (ClientData) tuvc);
	tuvc->running = 0;
	if (tuvc->rstate == REC_RECPRI) {
	    tuvc->rstate = REC_PAUSEPRI;
	} else if (tuvc->rstate == REC_RECORD) {
	    tuvc->rstate = REC_PAUSE;
	}
    }
    Tcl_MutexLock(&uvcMutex);
    hPtr = Tcl_FirstHashEntry(&tuvc->evts, &search);
    while (hPtr != NULL) {
	TUEVT *event = (TUEVT *) Tcl_GetHashKey(&tuvc->evts, hPtr);

	event->tuvc = NULL;
	event->hPtr = NULL;
	Tcl_DeleteHashEntry(hPtr);
	hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_MutexUnlock(&uvcMutex);
    return TCL_OK;
}

/*
 *-------------------------------------------------------------------------
 *
 * StartCapture --
 *
 *	Setup image acquisition:
 *	  - set capture format and size
 *	  - estabilsh async handler for FrameReady callback
 *	  - start UVC streaming
 *
 *-------------------------------------------------------------------------
 */

static int
StartCapture(TUVC *tuvc)
{
    Tcl_Interp *interp = tuvc->interp;
    uvc_stream_ctrl_t ctrl;
    uvc_error_t uret;
    int i;
    static const struct {
	enum uvc_frame_format fmt;
	int iscomp;
    } tryfmts[] = {
#ifdef LIBUVC_HAVE_JPEG
	{ UVC_FRAME_FORMAT_MJPEG, 1 },
#endif
	{ UVC_FRAME_FORMAT_YUYV, 0 },
	{ UVC_FRAME_FORMAT_UYVY, 0 },
	{ UVC_FRAME_FORMAT_GRAY16, 0 },
	{ UVC_FRAME_FORMAT_GRAY8, 0 },
	{ UVC_FRAME_FORMAT_RGB, 0 }
    };

    if (tuvc->running > 0) {
	return TCL_OK;
    }

    /* set format/size */
    uret = UVC_ERROR_INVALID_MODE;
    for (i = 0;  i < sizeof(tryfmts) / sizeof(tryfmts[0]); i++) {
	if (!tuvc->iscomp && tryfmts[i].iscomp) {
	    continue;
	}
	uret = uvc_get_stream_ctrl_format_size(tuvc->devh, &ctrl,
					       tryfmts[i].fmt,
					       tuvc->width, tuvc->height,
					       tuvc->fps);
	if (uret == UVC_SUCCESS) {
	    break;
	}
	i++;
    }
    if (uret < 0) {
	Tcl_SetObjResult(interp,
			 Tcl_ObjPrintf("error setting format: %s",
				       uvc_strerror(uret)));
	return TCL_ERROR;
    }

    /* start capture */
    tuvc->running = 1;
    tuvc->counters[0] = tuvc->counters[1] = tuvc->counters[2] = 0;
    tuvc->tid = Tcl_GetCurrentThread();
    tuvc->numev = 0;
    uret = uvc_start_streaming(tuvc->devh, &ctrl, FrameCallback, tuvc, 0);
    if (uret < 0) {
	tuvc->running = 0;
	tuvc->tid = NULL;
	Tcl_SetObjResult(interp,
			 Tcl_ObjPrintf("error starting streaming: %s",
				       uvc_strerror(uret)));
	return TCL_ERROR;
    }
    if (tuvc->rstate == REC_PAUSEPRI) {
	gettimeofday(&tuvc->ltv, NULL);
	tuvc->rtv = tuvc->ltv;
	tuvc->rstate = REC_RECPRI;
    } else if (tuvc->rstate == REC_PAUSE) {
	gettimeofday(&tuvc->ltv, NULL);
	tuvc->rtv = tuvc->ltv;
	tuvc->rstate = REC_RECORD;
    }
    return TCL_OK;
}

/*
 *-------------------------------------------------------------------------
 *
 * GetImage --
 *
 *	Retrieve last captured frame as photo image or byte array.
 *
 *-------------------------------------------------------------------------
 */

static int
GetImage(TUVCI *tuvci, TUVC *tuvc, Tcl_Obj *arg)
{
    Tcl_Interp *interp = tuvc->interp;
    uvc_frame_t *frame;
    Tk_PhotoHandle photo = NULL;
    int result = TCL_OK, done = 0;
    char *name;

    if (arg != NULL) {
	if (CheckForTk(tuvci, tuvc->interp) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (Tk_MainWindow(interp) == NULL) {
	    Tcl_SetResult(interp, "application has been destroyed",
			  TCL_STATIC);
	    return TCL_ERROR;
	}
	name = Tcl_GetString(arg);
	photo = Tk_FindPhoto(interp, name);
	if (photo == NULL) {
	    Tcl_SetObjResult(interp,
		Tcl_ObjPrintf("can't use \"%s\": not a photo image", name));
	    return TCL_ERROR;
	}
    }

    /* Temporarily take out last frame. */
    Tcl_MutexLock(&uvcMutex);
    frame = tuvc->frame;
    tuvc->frame = NULL;
    Tcl_MutexUnlock(&uvcMutex);
    if (frame == NULL) {
	/* no image available */
noImage:
	if (photo != NULL) {
	    Tcl_SetObjResult(interp, Tcl_NewIntObj(0));
	} else {
	    Tcl_SetResult(interp, "no image available", TCL_STATIC);
	    result = TCL_ERROR;
	}
	goto done;
    }
    if ((photo == NULL) && (frame->frame_format == UVC_FRAME_FORMAT_GRAY16)) {
	goto doByteArray;
    }
    if ((frame->frame_format != UVC_FRAME_FORMAT_RGB) &&
	(frame->frame_format != UVC_FRAME_FORMAT_GRAY8)) {
	uvc_frame_t *newFrame;
	uvc_error_t uret;
	int frameSize;

	switch (frame->frame_format) {
	case UVC_FRAME_FORMAT_YUYV:
	case UVC_FRAME_FORMAT_UYVY:
	case UVC_FRAME_FORMAT_MJPEG:
	    frameSize = frame->width * frame->height * 3;
	    break;
	case UVC_FRAME_FORMAT_GRAY16:
	    frameSize = frame->width * frame->height;
	    break;
	default:
	    goto noImage;
	}
	newFrame = uvc_allocate_frame(frameSize);
	if (newFrame == NULL) {
	    goto noImage;
	}
	switch (frame->frame_format) {
	case UVC_FRAME_FORMAT_YUYV:
	    uret = uvc_yuyv2rgb(frame, newFrame);
	    break;
	case UVC_FRAME_FORMAT_UYVY:
	    uret = uvc_uyvy2rgb(frame, newFrame);
	    break;
#ifdef LIBUVC_HAVE_JPEG
	case UVC_FRAME_FORMAT_MJPEG:
	    uret = uvc_mjpeg2rgb(frame, newFrame);
	    break;
#endif
	case UVC_FRAME_FORMAT_GRAY16:
	    uret = uvc_gray16to8(frame, newFrame, tuvc->greyshift);
	    break;
	default:
	    uret = UVC_ERROR_NOT_SUPPORTED;
	    break;
	}
	if (uret) {
	    uvc_free_frame(newFrame);
	    goto noImage;
	}
	uvc_free_frame(frame);
	frame = newFrame;
    }
    if (photo != NULL) {
	Tk_PhotoImageBlock block;
	int rot = tuvc->rotate;
	int width = frame->width;
	int height = frame->height;

	if (frame->frame_format == UVC_FRAME_FORMAT_GRAY8) {
	    block.pixelSize = 1;
	    block.offset[0] = 0;
	    block.offset[1] = 0;
	    block.offset[2] = 0;
	    block.offset[3] = 1;
	} else {
	    block.pixelSize = 3;
	    block.offset[0] = 0;
	    block.offset[1] = 1;
	    block.offset[2] = 2;
	    block.offset[3] = 4;
	}
	block.width = width;
	block.height = height;
	block.pitch = frame->step;
	block.pixelPtr = frame->data;

	if ((tuvc->mirror & 3) == 3) {
	    rot = (rot + 180) % 360;
	}
	switch (rot) {
	case 270:	/* = 90 CW */
	    block.pitch = block.pixelSize;
	    block.pixelPtr += width * block.pixelSize * (height - 1);
	    block.pixelSize *= -width;
	    block.offset[3] = block.pixelSize + 1;	/* no alpha */
	    block.width = height;
	    block.height = width;
	    break;
	case 180:	/* = 180 CW */
	    block.pitch = -block.pitch;
	    block.pixelPtr += (width * height - 1) * block.pixelSize;
	    block.pixelSize = -block.pixelSize;
	    block.offset[3] = block.pixelSize + 1;	/* no alpha */
	    break;
	case 90:	/* = 270 CW */
	    block.pitch = -block.pixelSize;
	    block.pixelPtr += (width - 1) * block.pixelSize;
	    block.pixelSize *= width;
	    block.offset[3] = block.pixelSize + 1;	/* no alpha */
	    block.width = height;
	    block.height = width;
	    break;
	}
	if ((tuvc->mirror & 3) == 2) {
	    /* mirror in X */
	    block.pixelPtr += (block.width - 1) * block.pixelSize;
	    block.pixelSize = -block.pixelSize;
	    block.offset[3] = block.pixelSize + 1;      /* no alpha */
	}
	if ((tuvc->mirror & 3) == 1) {
	    /* mirror in Y */
	    block.pixelPtr += block.pitch * (block.height - 1);
	    block.pitch = -block.pitch;
	}

	if (Tk_PhotoExpand(interp, photo, block.width, block.height)
	    != TCL_OK) {
	    result = TCL_ERROR;
	    goto done;
	}
	if (Tk_PhotoPutBlock(interp, photo, &block, 0, 0,
			     block.width, block.height,
			     TK_PHOTO_COMPOSITE_SET) != TCL_OK) {
	    result = TCL_ERROR;
	} else {
	    Tcl_SetObjResult(interp, Tcl_NewIntObj(1));
	    done = 1;
	}
    }
doByteArray:
    if (photo == NULL) {
	unsigned char *rawPtr;
	Tcl_Size rawSize;
	Tcl_Obj *list[4];

	list[0] = Tcl_NewIntObj(frame->width);
	list[1] = Tcl_NewIntObj(frame->height);
	if (frame->frame_format == UVC_FRAME_FORMAT_GRAY16) {
	    rawSize = frame->width * frame->height * 2;
	    list[2] = Tcl_NewIntObj(2);
	} else if (frame->frame_format == UVC_FRAME_FORMAT_GRAY8) {
	    rawSize = frame->width * frame->height;
	    list[2] = Tcl_NewIntObj(1);
	} else {
	    rawSize = frame->width * frame->height * 3;
	    list[2] = Tcl_NewIntObj(3);
	}
	rawPtr = frame->data;
	list[3] = Tcl_NewByteArrayObj(rawPtr, rawSize);
	Tcl_SetObjResult(interp, Tcl_NewListObj(4, list));
	done = 1;
    }
done:
    Tcl_MutexLock(&uvcMutex);
    if ((frame != NULL) && (tuvc->frame == NULL)) {
	/* Put back last frame */
	tuvc->frame = frame;
	frame = NULL;
    }
    if (done) {
	tuvc->counters[1] += 1;
    }
    Tcl_MutexUnlock(&uvcMutex);
    if (frame != NULL) {
	uvc_free_frame(frame);
    }
    return result;
}

/*
 *-------------------------------------------------------------------------
 *
 * InitControls --
 *
 *	Fill (or release) TUVC controls with meta information about
 *	the device's controls.
 *
 *-------------------------------------------------------------------------
 */

static void
InitControls(TUVC *tuvc)
{
    int i, isNew;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    UCTRL *uctrl;
    UFMT *ufmt, *ufmt0 = NULL;

    /* first, free up old stuff */
    hPtr = Tcl_FirstHashEntry(&tuvc->ctrl, &search);
    while (hPtr != NULL) {
	uctrl = (UCTRL *) Tcl_GetHashValue(hPtr);
	ckfree((char *) uctrl);
	hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&tuvc->ctrl);
    Tcl_InitHashTable(&tuvc->ctrl, TCL_STRING_KEYS);
    hPtr = Tcl_FirstHashEntry(&tuvc->fmts, &search);
    while (hPtr != NULL) {
	ufmt = (UFMT *) Tcl_GetHashValue(hPtr);
	Tcl_DStringFree(&ufmt->str);
	ckfree((char *) ufmt);
	hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&tuvc->fmts);
    Tcl_InitHashTable(&tuvc->fmts, TCL_ONE_WORD_KEYS);

    /* done, when there's no opened device */
    if (tuvc->devh == NULL) {
	return;
    }

    /* fill in new information */
    for (i = 0; i < sizeof(UvcCtrlInfo) / sizeof(UvcCtrlInfo[0]); i++) {
	uvc_error_t uret;
	int index, len;

	switch (UvcCtrlInfo[i].code & UVC_SELECTOR) {
	case UVC_SELECTOR_CT:
	    if (uvc_get_camera_terminal(tuvc->devh) == NULL) {
		continue;
	    }
	    index = (uvc_get_camera_terminal(tuvc->devh)->bTerminalID << 8) |
		tuvc->devh->info->ctrl_if.bInterfaceNumber;
	    break;
	case UVC_SELECTOR_PU:
	    if (uvc_get_processing_units(tuvc->devh) == NULL) {
		continue;
	    }
	    index = (uvc_get_processing_units(tuvc->devh)->bUnitID << 8) |
		tuvc->devh->info->ctrl_if.bInterfaceNumber;
	    break;
	case UVC_SELECTOR_SU:
	    if (uvc_get_selector_units(tuvc->devh) == NULL) {
		continue;
	    }
	    index = (uvc_get_selector_units(tuvc->devh)->bUnitID << 8) |
		tuvc->devh->info->ctrl_if.bInterfaceNumber;
	    break;
	default:
	    continue;
	}
	uctrl = (UCTRL *) ckalloc(sizeof(UCTRL));
	memset(uctrl, 0, sizeof(UCTRL));
	uctrl->code = UvcCtrlInfo[i].code;
	uctrl->name = UvcCtrlInfo[i].name;
	uctrl->type = UvcCtrlInfo[i].type;
	uctrl->count = UvcCtrlInfo[i].count;
	len = uctrl->type * uctrl->count;
	uret = libusb_control_transfer(tuvc->devh->usb_devh, 0xa1, UVC_GET_CUR,
				       (uctrl->code << 8) & 0xFF00, index,
				       uctrl->cur, len, 0);
	if (uret != len) {
	    ckfree((char *) uctrl);
	    continue;
	}
	uret = libusb_control_transfer(tuvc->devh->usb_devh, 0xa1, UVC_GET_MIN,
				       (uctrl->code << 8) & 0xFF00, index,
				       uctrl->min, len, 0);
	if (uret == len) {
	    uctrl->flags |= CTRL_HAS_MIN;
	}
	uret = libusb_control_transfer(tuvc->devh->usb_devh, 0xa1, UVC_GET_MAX,
				       (uctrl->code << 8) & 0xFF00, index,
				       uctrl->max, len, 0);
	if (uret == len) {
	    uctrl->flags |= CTRL_HAS_MAX;
	}
	uret = libusb_control_transfer(tuvc->devh->usb_devh, 0xa1, UVC_GET_RES,
				       (uctrl->code << 8) & 0xFF00, index,
				       uctrl->res, len, 0);
	if (uret == len) {
	    uctrl->flags |= CTRL_HAS_RES;
	}
	uret = libusb_control_transfer(tuvc->devh->usb_devh, 0xa1, UVC_GET_DEF,
				       (uctrl->code << 8) & 0xFF00, index,
				       uctrl->def, len, 0);
	if (uret == len) {
	    uctrl->flags |= CTRL_HAS_DEF;
	}
	hPtr =
	    Tcl_CreateHashEntry(&tuvc->ctrl, (ClientData) uctrl->name, &isNew);
	if (!isNew) {
	    UCTRL *oldctrl = (UCTRL *) Tcl_GetHashValue(hPtr);

	    ckfree((char *) oldctrl);
	}
	Tcl_SetHashValue(hPtr, (ClientData) uctrl);
    }

    /* format table: frame-size, frame-rate, etc. */
    if (tuvc->devh->info != NULL) {
	int k;
	long index = 0;
	uvc_streaming_interface_t *sif;
	uvc_format_desc_t *fm;
	uvc_frame_desc_t *fd;
	char buffer[64];

	/*
	 * First indices of table are uncompressed formats,
	 * then MJPEG formats (if any) follow.
	 */
	for (k = 0; k < 2; k++) {
	    for (sif = tuvc->devh->info->stream_ifs;
		 sif != NULL; sif = sif->next) {
		for (fm = sif->format_descs; fm != NULL; fm = fm->next) {
		    if (k == 0 &&
			fm->bDescriptorSubtype != UVC_VS_FORMAT_UNCOMPRESSED) {
			continue;
		    } else if (k &&
			       fm->bDescriptorSubtype != UVC_VS_FORMAT_MJPEG) {
			continue;
		    }
		    for (fd = fm->frame_descs; fd != NULL; fd = fd->next) {
			int r;

			ufmt = (UFMT *) ckalloc(sizeof(UFMT));
			ufmt->width = fd->wWidth;
			ufmt->height = fd->wHeight;
			ufmt->bpp = k ? 24 : fm->bBitsPerPixel;
			memcpy(&ufmt->fourcc, &fm->fourccFormat, 4);
			memset(ufmt->fpsList, 0, sizeof(ufmt->fpsList));
			Tcl_DStringInit(&ufmt->str);
			Tcl_DStringAppendElement(&ufmt->str, "frame-size");
			sprintf(buffer, "%dx%d", ufmt->width, ufmt->height);
			Tcl_DStringAppendElement(&ufmt->str, buffer);
			ufmt->fps = 10000000 / fd->dwDefaultFrameInterval;
			Tcl_DStringAppendElement(&ufmt->str, "frame-rate");
			sprintf(buffer, "%d", ufmt->fps);
			Tcl_DStringAppendElement(&ufmt->str, buffer);
			if (fd->intervals) {
			    uint32_t *ip = fd->intervals;

			    Tcl_DStringAppendElement(&ufmt->str,
						     "frame-rate-values");
			    Tcl_DStringStartSublist(&ufmt->str);
			    i = 0;
			    while (*ip) {
				r = 10000000 / *ip;
				ufmt->fpsList[i++] = r;
				sprintf(buffer, "%d", r);
				Tcl_DStringAppendElement(&ufmt->str, buffer);
				++ip;
			    }
			    Tcl_DStringEndSublist(&ufmt->str);
			} else {
			    Tcl_DStringAppendElement(&ufmt->str,
						     "frame-rate-min");
			    r = 10000000 / fd->dwMinFrameInterval;
			    sprintf(buffer, "%d", r);
			    Tcl_DStringAppendElement(&ufmt->str, buffer);
			    Tcl_DStringAppendElement(&ufmt->str,
						     "frame-rate-max");
			    r = 10000000 / fd->dwMaxFrameInterval;
			    sprintf(buffer, "%d", r);
			    Tcl_DStringAppendElement(&ufmt->str, buffer);
			}
			ufmt->iscomp = (k > 0);
			Tcl_DStringAppendElement(&ufmt->str, "mjpeg");
			Tcl_DStringAppendElement(&ufmt->str,
						 ufmt->iscomp ? "1" : "0");
			hPtr = Tcl_CreateHashEntry(&tuvc->fmts,
						   (ClientData) index, &isNew);
			if (!isNew) {
			    UFMT *uold = (UFMT *) Tcl_GetHashValue(hPtr);

			    if (uold == ufmt0) {
				ufmt0 = ufmt;
			    }
			    Tcl_DStringFree(&uold->str);
			    ckfree((char *) uold);
			}
			Tcl_SetHashValue(hPtr, (ClientData) ufmt);
			if (ufmt0 == NULL) {
			    ufmt0 = ufmt;
			}
			index++;
		    }
		}
	    }
	}
	if (ufmt0 != NULL) {
	    tuvc->width = ufmt0->width;
	    tuvc->height = ufmt0->height;
	    tuvc->fps = ufmt0->fps;
	    tuvc->usefmt = 0;
	    tuvc->iscomp = ufmt0->iscomp;
	}
    }
}

/*
 *-------------------------------------------------------------------------
 *
 * GetControls, PrintVal --
 *
 *	Read out current values of device controls and return
 *	these as list made up of key value pairs. Added meta
 *	information entries to support user interface:
 *
 *	  <name>-values    comma separated list of menu choices
 *	  <name>-minimum   minimum value for bool/integer
 *	  <name>-maximum   minimum value for bool/integer
 *	  <name>-step      interval step value for integer
 *	  <name>-default   default value for bool/integer
 *
 *-------------------------------------------------------------------------
 */

static void
GetControls(TUVC *tuvc, Tcl_Obj *list)
{
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    Tcl_DString ds;

    Tcl_ListObjAppendElement(NULL, list,
	     Tcl_NewStringObj("update-mode", -1));
    Tcl_ListObjAppendElement(NULL, list,
	     Tcl_NewStringObj(tuvc->idle ? "1" : "0", 1));
    Tcl_DStringInit(&ds);
    hPtr = Tcl_FirstHashEntry(&tuvc->ctrl, &search);
    while (hPtr != NULL) {
	UCTRL *uctrl = (UCTRL *) Tcl_GetHashValue(hPtr);
	int index = -1;

	Tcl_ListObjAppendElement(NULL, list,
		Tcl_NewStringObj((char *) uctrl->name, -1));
	switch (uctrl->code & UVC_SELECTOR) {
	case UVC_SELECTOR_CT:
	    index = (uvc_get_camera_terminal(tuvc->devh)->bTerminalID << 8) |
		tuvc->devh->info->ctrl_if.bInterfaceNumber;
	    break;
	case UVC_SELECTOR_PU:
	    index = (uvc_get_processing_units(tuvc->devh)->bUnitID << 8) |
		tuvc->devh->info->ctrl_if.bInterfaceNumber;
	    break;
	case UVC_SELECTOR_SU:
	    index = (uvc_get_selector_units(tuvc->devh)->bUnitID << 8) |
		tuvc->devh->info->ctrl_if.bInterfaceNumber;
	    break;
	}
	if (index != -1) {
	    int len = uctrl->type * uctrl->count;

	    libusb_control_transfer(tuvc->devh->usb_devh, 0xa1, UVC_GET_CUR,
				    (uctrl->code << 8) & 0xFF00, index,
				    uctrl->cur, len, 0);
	}
	PrintVal(uctrl, uctrl->cur, &ds, list);
	if (uctrl->flags & CTRL_HAS_MIN) {
	    Tcl_DStringSetLength(&ds, 0);
	    Tcl_DStringAppend(&ds, uctrl->name, -1);
	    Tcl_DStringAppend(&ds, "-minimum", -1);
	    Tcl_ListObjAppendElement(NULL, list,
		     Tcl_NewStringObj(Tcl_DStringValue(&ds),
				      Tcl_DStringLength(&ds)));
	    PrintVal(uctrl, uctrl->min, &ds, list);
	}
	if (uctrl->flags & CTRL_HAS_MAX) {
	    Tcl_DStringSetLength(&ds, 0);
	    Tcl_DStringAppend(&ds, uctrl->name, -1);
	    Tcl_DStringAppend(&ds, "-maximum", -1);
	    Tcl_ListObjAppendElement(NULL, list,
		     Tcl_NewStringObj(Tcl_DStringValue(&ds),
				      Tcl_DStringLength(&ds)));
	    PrintVal(uctrl, uctrl->max, &ds, list);
	}
	if (uctrl->flags & CTRL_HAS_RES) {
	    Tcl_DStringSetLength(&ds, 0);
	    Tcl_DStringAppend(&ds, uctrl->name, -1);
	    Tcl_DStringAppend(&ds, "-step", -1);
	    Tcl_ListObjAppendElement(NULL, list,
		     Tcl_NewStringObj(Tcl_DStringValue(&ds),
				      Tcl_DStringLength(&ds)));
	    PrintVal(uctrl, uctrl->res, &ds, list);
	}
	if (uctrl->flags & CTRL_HAS_DEF) {
	    Tcl_DStringSetLength(&ds, 0);
	    Tcl_DStringAppend(&ds, uctrl->name, -1);
	    Tcl_DStringAppend(&ds, "-default", -1);
	    Tcl_ListObjAppendElement(NULL, list,
		     Tcl_NewStringObj(Tcl_DStringValue(&ds),
				      Tcl_DStringLength(&ds)));
	    PrintVal(uctrl, uctrl->def, &ds, list);
	}
	hPtr = Tcl_NextHashEntry(&search);
    }
}

static void
PrintVal(UCTRL *uctrl, unsigned char *data, Tcl_DString *dsPtr, Tcl_Obj *list)
{
    int i;
    unsigned char *dp = data;
    char buffer[64];

    Tcl_DStringSetLength(dsPtr, 0);
    for (i = 0; i < uctrl->count; i++) {
	int v = 0;

	switch (uctrl->type) {
	case 1:
	    v = dp[0];
	    dp += uctrl->type;
	    break;
	case 2:
	    v = dp[0] | (dp[1] << 8);
	    dp += uctrl->type;
	    break;
	case 4:
	    v = dp[0] | (dp[1] << 8) | (dp[2] << 16) | (dp[3] << 24);
	    dp += uctrl->type;
	    break;
	}
	sprintf(buffer, "%s%d", (i == 0) ? "" : ",", v);
	Tcl_DStringAppend(dsPtr, buffer, -1);
    }
    Tcl_ListObjAppendElement(NULL, list,
	     Tcl_NewStringObj(Tcl_DStringValue(dsPtr),
			      Tcl_DStringLength(dsPtr)));
}

/*
 *-------------------------------------------------------------------------
 *
 * SetControls --
 *
 *	Set device controls given list of key value pairs.
 *
 *-------------------------------------------------------------------------
 */

static int
SetControls(TUVC *tuvc, int objc, Tcl_Obj * const objv[])
{
    Tcl_Interp *interp = tuvc->interp;
    Tcl_HashEntry *hPtr;
    int i, k, n;

    for (i = 0; i < objc; i += 2) {
	UCTRL *uctrl;
	char *name, *valStr;

	name = Tcl_GetString(objv[i]);
	if (strcmp(name, "update-mode") == 0) {
	    int flag;

	    if (Tcl_GetBooleanFromObj(NULL, objv[i + 1], &flag) == TCL_OK) {
		if (flag != tuvc->idle) {
		    Tcl_CancelIdleCall(FrameReady, (ClientData) tuvc);
		    Tcl_MutexLock(&uvcMutex);
		    tuvc->numev = 0;
		    tuvc->idle = flag;
		    Tcl_MutexUnlock(&uvcMutex);
		}
	    }
	    continue;
	}
	hPtr = Tcl_FindHashEntry(&tuvc->ctrl, name);
	if (hPtr == NULL) {
	    continue;
	}
	uctrl = (UCTRL *) Tcl_GetHashValue(hPtr);
	valStr = Tcl_GetString(objv[i + 1]);
	for (k = n = 0; k < uctrl->count; k++) {
	    long lv;
	    char *endPtr = NULL;

	    lv = strtol(valStr, &endPtr, 0);
	    switch (uctrl->type) {
	    case 1:
		uctrl->cur[n++] = lv;
		break;
	    case 2:
		uctrl->cur[n++] = lv;
		uctrl->cur[n++] = lv >> 8;
		break;
	    case 4:
		uctrl->cur[n++] = lv;
		uctrl->cur[n++] = lv >> 8;
		uctrl->cur[n++] = lv >> 16;
		uctrl->cur[n++] = lv >> 24;
		break;
	    }
	    if ((endPtr == NULL) || (*endPtr != ',')) {
		break;
	    }
	    valStr = endPtr + 1;
	}
	k = -1;
	switch (uctrl->code & UVC_SELECTOR) {
	case UVC_SELECTOR_CT:
	    k = (uvc_get_camera_terminal(tuvc->devh)->bTerminalID << 8) |
		tuvc->devh->info->ctrl_if.bInterfaceNumber;
	    break;
	case UVC_SELECTOR_PU:
	    k = (uvc_get_processing_units(tuvc->devh)->bUnitID << 8) |
		tuvc->devh->info->ctrl_if.bInterfaceNumber;
	    break;
	case UVC_SELECTOR_SU:
	    k = (uvc_get_selector_units(tuvc->devh)->bUnitID << 8) |
		tuvc->devh->info->ctrl_if.bInterfaceNumber;
	    break;
	}
	if (k != -1) {
	    uvc_error_t uret;

	    n = uctrl->type * uctrl->count;
	    uret = libusb_control_transfer(tuvc->devh->usb_devh, 0x21,
					   UVC_SET_CUR,
					   (uctrl->code << 8) & 0xFF00, k,
					   uctrl->cur, n, 0);
	    if (uret < 0) {
		Tcl_SetObjResult(interp,
			 Tcl_ObjPrintf("error setting \"%s\": %s",
				       Tcl_GetString(objv[i]),
				       uvc_strerror(uret)));
		return TCL_ERROR;
	    } else if (uret != n) {
		Tcl_SetObjResult(interp,
			 Tcl_ObjPrintf("error setting \"%s\": short write",
				       Tcl_GetString(objv[i])));
		return TCL_ERROR;
	    }
	}
    }
    return TCL_OK;
}

/*
 *-------------------------------------------------------------------------
 *
 * UvcObjCmdDeleted --
 *
 *	Destructor of "uvc" Tcl command. Closes all open devices and
 *	releases all resources.
 *
 *-------------------------------------------------------------------------
 */

static void
UvcObjCmdDeleted(ClientData clientData)
{
    TUVCI *tuvci = (TUVCI *) clientData;
    Tcl_HashEntry *hPtr;
    Tcl_HashSearch search;
    TUVC *tuvc;

    hPtr = Tcl_FirstHashEntry(&tuvci->tuvcc, &search);
    while (hPtr != NULL) {
	tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	StopCapture(tuvc);
	uvc_close(tuvc->devh);
	tuvc->devh = NULL;
	uvc_unref_device(tuvc->dev);
	tuvc->dev = NULL;
	uvc_exit(tuvc->ctx);
	tuvc->ctx = NULL;
	Tcl_DStringFree(&tuvc->devName);
	Tcl_DStringFree(&tuvc->cbCmd);
	FinishRecording(tuvc, 1, 1);
	InitControls(tuvc);
	Tcl_DeleteHashTable(&tuvc->evts);
	ckfree((char *) tuvc);
	hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&tuvci->tuvcc);
    if (tuvci->ctx != NULL) {
	uvc_exit(tuvci->ctx);
    }
#ifdef HAVE_LIBUDEV
    tuvci->interp = NULL;
    Tcl_DStringFree(&tuvci->cbCmd);
    hPtr = Tcl_FirstHashEntry(&tuvci->devs, &search);
    while (hPtr != NULL) {
	Tcl_DStringFree((Tcl_DString *) Tcl_GetHashValue(hPtr));
	ckfree((char *) Tcl_GetHashValue(hPtr));
	hPtr = Tcl_NextHashEntry(&search);
    }
    Tcl_DeleteHashTable(&tuvci->devs);
    if (tuvci->udevMon != NULL) {
	Tcl_DeleteFileHandler(udev_monitor_get_fd(tuvci->udevMon));
	udev_monitor_unref(tuvci->udevMon);
	tuvci->udevMon = NULL;
    }
    if (tuvci->udev != NULL) {
	udev_unref(tuvci->udev);
	tuvci->udev = NULL;
    }
#endif
    Tcl_FreeEncoding(tuvci->enc);
    ckfree((char *) tuvci);
}

/*
 *-------------------------------------------------------------------------
 *
 * UvcObjCmd --
 *
 *	"uvc" Tcl command dealing with libuvc devices.
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *-------------------------------------------------------------------------
 */

static int
UvcObjCmd(ClientData clientData, Tcl_Interp *interp,
	   int objc, Tcl_Obj * const objv[])
{
    TUVCI *tuvci = (TUVCI *) clientData;
    TUVC *tuvc;
    Tcl_HashEntry *hPtr;
    int ret = TCL_OK, command;
    uvc_error_t uret;

    static const char *cmdNames[] = {
	"close", "convmode", "counters", "devices",
	"format", "greyshift", "image", "info", "listen",
	"listformats", "mbcopy", "mcopy", "mirror", "open",
	"orientation", "parameters", "record", "start",
	"state", "stop", "tophoto", NULL
    };
    enum cmdCode {
	CMD_close, CMD_convmode, CMD_counters, CMD_devices,
	CMD_format, CMD_greyshift, CMD_image, CMD_info, CMD_listen,
	CMD_listformats, CMD_mbcopy, CMD_mcopy, CMD_mirror, CMD_open,
	CMD_orientation, CMD_parameters, CMD_record, CMD_start,
	CMD_state, CMD_stop, CMD_tophoto
    };
    static const char *recNames[] = {
	"frame", "pause", "resume", "start", "state", "stop", NULL
    };
    enum recCode {
	REC_frame, REC_pause, REC_resume, REC_start, REC_state, REC_stop
    };

    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "option ...");
	return TCL_ERROR;
    }
    if (Tcl_GetIndexFromObj(interp, objv[1], cmdNames, "option", 0,
			    &command) != TCL_OK) {
	return TCL_ERROR;
    }

    switch ((enum cmdCode) command) {

    case CMD_close:
	if (objc != 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	    Tcl_DeleteHashEntry(hPtr);
	    StopCapture(tuvc);
	    uvc_close(tuvc->devh);
	    tuvc->devh = NULL;
	    uvc_unref_device(tuvc->dev);
	    tuvc->dev = NULL;
	    uvc_exit(tuvc->ctx);
	    tuvc->ctx = NULL;
	    Tcl_DStringFree(&tuvc->devName);
	    Tcl_DStringFree(&tuvc->cbCmd);
	    FinishRecording(tuvc, 1, 1);
	    InitControls(tuvc);
	    Tcl_DeleteHashTable(&tuvc->evts);
	    ckfree((char *) tuvc);
	} else {
devNotFound:
	    Tcl_SetObjResult(interp,
		Tcl_ObjPrintf("device \"%s\" not found",
			      Tcl_GetString(objv[2])));
	    ret = TCL_ERROR;
	}
	break;

    case CMD_convmode:
	if (objc != 3 && objc != 4) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid ?flag?");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);

	    if (objc > 3) {
		int conv;

		if (Tcl_GetBooleanFromObj(interp, objv[3], &conv) != TCL_OK) {
		    return TCL_ERROR;
		}
		if (tuvc->conv != conv) {
		    FinishRecording(tuvc, 1, 0);
		}
		tuvc->conv = conv;
	    } else {
		Tcl_SetBooleanObj(Tcl_GetObjResult(interp), tuvc->conv);
	    }
	} else {
	    goto devNotFound;
	}
	break;

    case CMD_counters:
	if (objc != 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    Tcl_Obj *r[3];

	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	    r[0] = Tcl_NewWideIntObj(tuvc->counters[0]);
	    r[1] = Tcl_NewWideIntObj(tuvc->counters[1]);
	    r[2] = Tcl_NewWideIntObj(tuvc->counters[2]);
	    Tcl_SetObjResult(interp, Tcl_NewListObj(3, r));
	} else {
	    goto devNotFound;
	}
	break;

    case CMD_devices:
	if (objc != 2) {
	    Tcl_WrongNumArgs(interp, 2, objv, NULL);
	    return TCL_ERROR;
	}
	if (tuvci->ctx == NULL) {
	    Tcl_SetResult(interp, "libuvc not initialized", TCL_STATIC);
	    return TCL_ERROR;
#ifdef HAVE_LIBUDEV
	} else if (tuvci->udevMon != NULL) {
	    Tcl_Obj *list = Tcl_NewListObj(0, NULL);
	    Tcl_HashSearch search;

	    if (tuvci->devsNeedRefresh) {
		UdevScan(tuvci, NULL);
	    }
	    hPtr = Tcl_FirstHashEntry(&tuvci->devs, &search);
	    while (hPtr != NULL) {
		char *p = (char *) Tcl_GetHashKey(&tuvci->devs, hPtr);

		Tcl_ListObjAppendElement(NULL, list, Tcl_NewStringObj(p, -1));
		p = Tcl_DStringValue((Tcl_DString *) Tcl_GetHashValue(hPtr));
		Tcl_ListObjAppendElement(NULL, list, Tcl_NewStringObj(p, -1));
		p += strlen(p) + 1;
		Tcl_ListObjAppendElement(NULL, list, Tcl_NewStringObj(p, -1));
		hPtr = Tcl_NextHashEntry(&search);
	    }
	    Tcl_SetObjResult(interp, list);
#endif
	} else {
	    uvc_device_t **devlist = NULL;
	    int i;
	    Tcl_Obj *list;

	    uret = uvc_get_device_list(tuvci->ctx, &devlist);
	    if (uret < 0) {
		Tcl_SetObjResult(interp,
				 Tcl_ObjPrintf("error getting devices: %s",
					       uvc_strerror(uret)));
		return TCL_ERROR;
	    }
	    list = Tcl_NewListObj(0, NULL);
	    for (i = 0; (devlist != NULL) && (devlist[i] != NULL); i++) {
		uvc_device_descriptor_t *desc = NULL;
		char *p, buffer[128];
		Tcl_DString ds;

		if (uvc_get_device_descriptor(devlist[i], &desc) < 0) {
		    continue;
		}
		Tcl_DStringInit(&ds);
		sprintf(buffer, "%04X:%04X:%d.%d",
			desc->idVendor, desc->idProduct,
			uvc_get_bus_number(devlist[i]),
			uvc_get_device_address(devlist[i]));
		Tcl_DStringAppend(&ds, buffer, -1);
		Tcl_ListObjAppendElement(NULL, list,
			Tcl_NewStringObj(Tcl_DStringValue(&ds),
					 Tcl_DStringLength(&ds)));
		Tcl_DStringFree(&ds);
		if (desc->manufacturer != NULL) {
		    p = Tcl_ExternalToUtfDString(tuvci->enc,
				desc->manufacturer, -1, &ds);
		    Tcl_ListObjAppendElement(NULL, list,
			    Tcl_NewStringObj(p, Tcl_DStringLength(&ds)));
		    Tcl_DStringFree(&ds);
		} else {
		    Tcl_ListObjAppendElement(NULL, list, Tcl_NewObj());
		}
		if (desc->product != NULL) {
		    p = Tcl_ExternalToUtfDString(tuvci->enc,
				desc->product, -1, &ds);
		    Tcl_ListObjAppendElement(NULL, list,
			    Tcl_NewStringObj(p, Tcl_DStringLength(&ds)));
		    Tcl_DStringFree(&ds);
		} else {
		    Tcl_ListObjAppendElement(NULL, list, Tcl_NewObj());
		}
		uvc_free_device_descriptor(desc);
	    }
	    uvc_free_device_list(devlist, 1);
	    Tcl_SetObjResult(interp, list);
	}
	break;

    case CMD_format:
	if ((objc < 3) || (objc > 5)) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid ?fmt ?fps??");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr == NULL) {
	    goto devNotFound;
	}
	tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	if (objc > 3) {
	    UFMT *ufmt;
	    int k, fps = 0;
	    long lk;

	    if (Tcl_GetIntFromObj(interp, objv[3], &k) != TCL_OK) {
		return TCL_ERROR;
	    }
	    if ((objc > 4) &&
		(Tcl_GetIntFromObj(interp, objv[4], &fps) != TCL_OK)) {
		return TCL_ERROR;
	    }
	    lk = k;
	    hPtr = Tcl_FindHashEntry(&tuvc->fmts, (ClientData) lk);
	    if (hPtr == NULL) {
		Tcl_SetObjResult(interp,
		    Tcl_ObjPrintf("format %d not found", k));
		return TCL_ERROR;
	    }
	    if (tuvc->running) {
		Tcl_SetResult(interp, "capture still running", TCL_STATIC);
		return TCL_ERROR;
	    }
	    /* Stop recording due to format change. */
	    if (tuvc->rstate > REC_STOP) {
		tuvc->rstate = REC_STOP;
	    }
	    FinishRecording(tuvc, 1, 0);
	    /* Set new format. */
	    ufmt = (UFMT *) Tcl_GetHashValue(hPtr);
	    tuvc->width = ufmt->width;;
	    tuvc->height = ufmt->height;
	    tuvc->usefmt = k;
	    tuvc->fps = ufmt->fps;
	    tuvc->iscomp = ufmt->iscomp;
	    if ((fps > 0) && (ufmt->fpsList[0] > 0)) {
		k = 0;
		while (ufmt->fpsList[k] > fps) {
		    k++;
		}
		if (ufmt->fpsList[k] > 0) {
		    tuvc->fps = ufmt->fpsList[k];
		} else if (--k >= 0) {
		    tuvc->fps = ufmt->fpsList[k];
		}
	    }
	} else {
	    Tcl_Obj *list[2];

	    list[0] = Tcl_NewIntObj(tuvc->usefmt);
	    list[1] = Tcl_NewIntObj(tuvc->fps);
	    Tcl_SetObjResult(interp, Tcl_NewListObj(2, list));
	}
	break;

    case CMD_greyshift:
	if (objc != 3 && objc != 4) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid ?shift?");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);

	    if (objc > 3) {
		int shift;

		if (Tcl_GetIntFromObj(interp, objv[3], &shift) != TCL_OK) {
		    return TCL_ERROR;
		}
		tuvc->greyshift = shift;
	    } else {
		Tcl_SetIntObj(Tcl_GetObjResult(interp), tuvc->greyshift);
	    }
	} else {
	    goto devNotFound;
	}
	break;

    case CMD_image:
	if ((objc < 3) || (objc > 4)) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid ?photoImage?");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	    ret = GetImage(tuvci, tuvc, (objc > 3) ? objv[3] : NULL);
	} else {
	    goto devNotFound;
	}
	break;

    case CMD_info:
	if (objc > 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "?devid?");
	    return TCL_ERROR;
	}
	if (objc == 2) {
	    Tcl_HashSearch search;
	    Tcl_Obj *list = Tcl_NewListObj(0, NULL);

	    hPtr = Tcl_FirstHashEntry(&tuvci->tuvcc, &search);
	    while (hPtr != NULL) {
		tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
		Tcl_ListObjAppendElement(NULL, list,
			Tcl_NewStringObj(tuvc->devId, -1));
		hPtr = Tcl_NextHashEntry(&search);
	    }
	    Tcl_SetObjResult(interp, list);
	} else {
	    hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	    if (hPtr != NULL) {
		Tcl_Obj *r[2];

		tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
		r[0] = Tcl_NewStringObj(Tcl_DStringValue(&tuvc->devName), -1);
		Tcl_DStringSetLength(&tuvc->cbCmd, tuvc->cbCmdLen);
		r[1] = Tcl_NewStringObj(Tcl_DStringValue(&tuvc->cbCmd), -1);
		Tcl_SetObjResult(interp, Tcl_NewListObj(2, r));
	    } else {
		goto devNotFound;
	    }
	}
	break;

    case CMD_listen:
	if (objc > 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "?cmd?");
	    return TCL_ERROR;
	}
#ifdef HAVE_LIBUDEV
	if (tuvci->udevMon != NULL) {
	    if (objc == 2) {
		Tcl_DStringSetLength(&tuvci->cbCmd, tuvci->cbCmdLen);
		Tcl_SetObjResult(interp,
			Tcl_NewStringObj(Tcl_DStringValue(&tuvci->cbCmd),
					 Tcl_DStringLength(&tuvci->cbCmd)));
	    } else {
		Tcl_DStringSetLength(&tuvci->cbCmd, 0);
		Tcl_DStringAppend(&tuvci->cbCmd, Tcl_GetString(objv[2]), -1);
		tuvci->cbCmdLen = Tcl_DStringLength(&tuvci->cbCmd);
	    }
	}
#endif
	break;

    case CMD_listformats:
	if (objc != 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr == NULL) {
	    goto devNotFound;
	} else {
	    Tcl_HashSearch search;
	    Tcl_Obj *dict;
	    UFMT *ufmt;
	    long lk;

	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	    dict = Tcl_NewDictObj();
	    hPtr = Tcl_FirstHashEntry(&tuvc->fmts, &search);
	    while (hPtr != NULL) {
		ufmt = (UFMT *) Tcl_GetHashValue(hPtr);
		lk = (long) Tcl_GetHashKey(&tuvc->fmts, hPtr);
		Tcl_DictObjPut(NULL, dict, Tcl_NewIntObj((int) lk),
		    Tcl_NewStringObj(Tcl_DStringValue(&ufmt->str),
				     Tcl_DStringLength(&ufmt->str)));
		hPtr = Tcl_NextHashEntry(&search);
	    }
	    Tcl_SetObjResult(interp, dict);
	}
	break;

    case CMD_mbcopy: {
	int mask0, mask;
	Tcl_Size i, srcLen, dstLen;
	unsigned char *src, *dst;

	if (objc != 5) {
	    Tcl_WrongNumArgs(interp, 2, objv, "bytearray1 bytearray2 mask");
	    return TCL_ERROR;
	}
	if (Tcl_GetIntFromObj(interp, objv[4], &mask0) != TCL_OK) {
	    return TCL_ERROR;
	}
	dst = Tcl_GetByteArrayFromObj(objv[2], &dstLen);
	src = Tcl_GetByteArrayFromObj(objv[3], &srcLen);
	if ((srcLen != dstLen) || (srcLen % 3)) {
	    Tcl_SetResult(interp, "incompatible bytearrays", TCL_STATIC);
	    return TCL_ERROR;
	}
	mask = (mask0 >> 16) & 0xff;	/* red */
	if (mask != 0) {
	    for (i = 0; i < srcLen; i += 3) {
		dst[i] = (dst[i] & (~mask)) | (src[i] & mask);
	    }
	}
	mask = (mask0 >> 8) & 0xff;	/* green */
	if (mask != 0) {
	    for (i = 1; i < srcLen; i += 3) {
		dst[i] = (dst[i] & (~mask)) | (src[i] & mask);
	    }
	}
	mask = mask0 & 0xff;		/* blue */
	if (mask != 0) {
	    for (i = 2; i < srcLen; i += 3) {
		dst[i] = (dst[i] & (~mask)) | (src[i] & mask);
	    }
	}
	break;
    }

    case CMD_mcopy: {
	char *name;
	Tk_PhotoHandle ph1, ph2;
	int mask0, mask, nops = 0, x, y;
	Tk_PhotoImageBlock block1, block2;
	unsigned char *src, *dst;

	if (objc != 5) {
	    Tcl_WrongNumArgs(interp, 2, objv, "photo1 photo2 mask");
	    return TCL_ERROR;
	}
	if (CheckForTk(tuvci, interp) != TCL_OK) {
	    return TCL_ERROR;
	}
	name = Tcl_GetString(objv[2]);
	ph1 = Tk_FindPhoto(interp, name);
	if (ph1 == NULL) {
	    Tcl_SetObjResult(interp,
		Tcl_ObjPrintf("can't use \"%s\": not a photo image", name));
	    return TCL_ERROR;
	}
	name = Tcl_GetString(objv[3]);
	ph2 = Tk_FindPhoto(interp, name);
	if (ph2 == NULL) {
	    Tcl_SetObjResult(interp,
		Tcl_ObjPrintf("can't use \"%s\": not a photo image", name));
	    return TCL_ERROR;
	}
	if (Tcl_GetIntFromObj(interp, objv[4], &mask0) != TCL_OK) {
	    return TCL_ERROR;
	}
	Tk_PhotoGetImage(ph1, &block1);
	Tk_PhotoGetImage(ph2, &block2);
	if ((block1.width != block2.width) ||
	    (block1.height != block2.height) ||
	    (block1.pixelSize != block2.pixelSize) ||
	    (block1.pixelSize != 4)) {
	    Tcl_SetResult(interp, "incompatible photo images", TCL_STATIC);
	    return TCL_ERROR;
	}
	mask = (mask0 >> 24) & 0xff;	/* alpha */
	if (mask != 0) {
	    for (y = 0; y < block1.height; y++) {
		dst = block1.pixelPtr + y * block1.pitch;
		src = block2.pixelPtr + y * block2.pitch;
		dst += block1.offset[3];
		src += block2.offset[3];
		for (x = 0; x < block1.width; x++) {
		    *dst = (*dst & (~mask)) | (*src & mask);
		    dst += block1.pixelSize;
		    src += block2.pixelSize;
		}
	    }
	    ++nops;
	}
	mask = (mask0 >> 16) & 0xff;	/* red */
	if (mask != 0) {
	    for (y = 0; y < block1.height; y++) {
		dst = block1.pixelPtr + y * block1.pitch;
		src = block2.pixelPtr + y * block2.pitch;
		dst += block1.offset[0];
		src += block2.offset[0];
		for (x = 0; x < block1.width; x++) {
		    *dst = (*dst & (~mask)) | (*src & mask);
		    dst += block1.pixelSize;
		    src += block2.pixelSize;
		}
	    }
	    ++nops;
	}
	mask = (mask0 >> 8) & 0xff;	/* green */
	if (mask != 0) {
	    for (y = 0; y < block1.height; y++) {
		dst = block1.pixelPtr + y * block1.pitch;
		src = block2.pixelPtr + y * block2.pitch;
		dst += block1.offset[1];
		src += block2.offset[1];
		for (x = 0; x < block1.width; x++) {
		    *dst = (*dst & (~mask)) | (*src & mask);
		    dst += block1.pixelSize;
		    src += block2.pixelSize;
		}
	    }
	    ++nops;
	}
	mask = mask0 & 0xff;		/* blue */
	if (mask != 0) {
	    for (y = 0; y < block1.height; y++) {
		dst = block1.pixelPtr + y * block1.pitch;
		src = block2.pixelPtr + y * block2.pitch;
		dst += block1.offset[2];
		src += block2.offset[2];
		for (x = 0; x < block1.width; x++) {
		    *dst = (*dst & (~mask)) | (*src & mask);
		    dst += block1.pixelSize;
		    src += block2.pixelSize;
		}
	    }
	    ++nops;
	}
	if (nops) {
	    ret = Tk_PhotoPutBlock(interp, ph1, &block1, 0, 0,
				   block1.width, block1.height,
				   TK_PHOTO_COMPOSITE_SET);
	}
	break;
    }

    case CMD_mirror: {
	int x, y;

	if ((objc != 3) && (objc != 5)) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid ?x y?");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr == NULL) {
	    goto devNotFound;
	}
	tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	if ((objc > 3) &&
	    ((Tcl_GetBooleanFromObj(interp, objv[3], &x) != TCL_OK) ||
	     (Tcl_GetBooleanFromObj(interp, objv[4], &y) != TCL_OK))) {
	    return TCL_ERROR;
	}
	if (objc > 3) {
	    tuvc->mirror = (x ? 1 : 0) | (y ? 2 : 0);
	} else {
	    Tcl_Obj *list[2];

	    list[0] = Tcl_NewBooleanObj(tuvc->mirror & 1);
	    list[1] = Tcl_NewBooleanObj(tuvc->mirror & 2);
	    Tcl_SetObjResult(interp, Tcl_NewListObj(2, list));
	}
	break;
    }

    case CMD_open: {
	char *devName, *p;
	uvc_context_t *ctx;
	uvc_device_t *dev;
	uvc_device_descriptor_t *desc;
	uvc_device_handle_t *devh;
	int vid = 0, pid = 0, isNew;
	int bd[2], *bdp = NULL;

	if (objc != 4) {
	    Tcl_WrongNumArgs(interp, 2, objv, "device callback");
	    return TCL_ERROR;
	}
	uvc_init(&ctx, NULL);
	if (ctx == NULL) {
	    Tcl_SetResult(interp, "libuvc not initialized", TCL_STATIC);
	    return TCL_ERROR;
	}
	devName = Tcl_GetString(objv[2]);
	sscanf(devName, "%x:%x", &vid, &pid);
	p = strchr(devName, ':');
	if (p != NULL) {
	    p = strchr(p + 1, ':');
	    if (p != NULL) {
		sscanf(p + 1, "%d.%d", bd, bd + 1);
		bdp = bd;
	    }
	}
	uret = uvc_find_device_bd(ctx, &dev, vid, pid, bdp);
	if (uret < 0) {
	    Tcl_SetObjResult(interp,
		Tcl_ObjPrintf("error while searching \"%s\": %s",
			      devName, uvc_strerror(uret)));
	    uvc_exit(ctx);
	    return TCL_ERROR;
	}
	if (uvc_get_device_descriptor(dev, &desc) < 0) {
	    uvc_unref_device(dev);
	    Tcl_SetObjResult(interp,
		Tcl_ObjPrintf("error while getting descriptor for \"%s\": %s",
			      devName, uvc_strerror(uret)));
	    uvc_exit(ctx);
	    return TCL_ERROR;
	}
	uret = uvc_open(dev, &devh);
	if (uret < 0) {
	    uvc_free_device_descriptor(desc);
	    uvc_unref_device(dev);
	    Tcl_SetObjResult(interp,
		Tcl_ObjPrintf("error while opening \"%s\": %s",
			      devName, uvc_strerror(uret)));
	    uvc_exit(ctx);
	    return TCL_ERROR;
	}
	tuvc = (TUVC *) ckalloc(sizeof(TUVC));
	memset(tuvc, 0, sizeof(TUVC));
	tuvc->ctx = ctx;
	tuvc->dev = dev;
	tuvc->devh = devh;
	tuvc->mirror = 0;
	tuvc->rotate = 0;
	tuvc->width = 640;
	tuvc->height = 480;
	tuvc->conv = 1;
	tuvc->greyshift = 4;	/* preset for 12 bit sensors */
	tuvc->fps = 30;
	tuvc->interp = interp;
	tuvc->tid = NULL;
	Tcl_InitHashTable(&tuvc->evts, TCL_ONE_WORD_KEYS);
	tuvc->numev = 0;
	tuvc->idle = 0;
	tuvc->running = 0;
	Tcl_DStringInit(&tuvc->devName);
	Tcl_DStringSetLength(&tuvc->devName, 128);
	p = Tcl_DStringValue(&tuvc->devName);
	sprintf(p, "%04X:%04X:%d.%d", desc->idVendor, desc->idProduct,
		uvc_get_bus_number(dev),
		uvc_get_device_address(dev));
	Tcl_DStringSetLength(&tuvc->devName, strlen(p));
	Tcl_DStringInit(&tuvc->cbCmd);
	Tcl_DStringAppend(&tuvc->cbCmd, Tcl_GetString(objv[3]), -1);
	tuvc->cbCmdLen = Tcl_DStringLength(&tuvc->cbCmd);
	sprintf(tuvc->devId, "uvc%d", tuvci->idCount++);
	Tcl_InitHashTable(&tuvc->ctrl, TCL_STRING_KEYS);
	Tcl_InitHashTable(&tuvc->fmts, TCL_ONE_WORD_KEYS);
	hPtr = Tcl_CreateHashEntry(&tuvci->tuvcc, tuvc->devId, &isNew);
	Tcl_SetHashValue(hPtr, (ClientData) tuvc);
	Tcl_SetObjResult(interp, Tcl_NewStringObj(tuvc->devId, -1));
	uvc_free_device_descriptor(desc);
	InitControls(tuvc);
	tuvc->rstate = REC_STOP;
	tuvc->rchan = NULL;
	Tcl_DStringInit(&tuvc->rbdStr);
	Tcl_MutexLock(&tuvc->rmutex);
	Tcl_MutexUnlock(&tuvc->rmutex);
	break;
    }

    case CMD_orientation: {
	if (objc > 4) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid ?degrees?");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr == NULL) {
	    goto devNotFound;
	}
	tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	if (objc > 3) {
	    int degrees;

	    if (Tcl_GetIntFromObj(interp, objv[3], &degrees) != TCL_OK) {
		return TCL_ERROR;
	    }
	    degrees = degrees % 360;
	    if (degrees < 45) {
		tuvc->rotate = 0;
	    } else if (degrees < 135) {
		tuvc->rotate = 90;
	    } else if (degrees < 225) {
		tuvc->rotate = 180;
	    } else if (degrees < 315) {
		tuvc->rotate = 270;
	    } else {
		tuvc->rotate = 0;
	    }
	} else {
	    Tcl_SetObjResult(interp, Tcl_NewIntObj(tuvc->rotate));
	}
	break;
    }

    case CMD_parameters:
	if ((objc < 3) || (objc % 2 == 0)) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid ?key value ...?");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	    if (objc == 3) {
		Tcl_Obj *list = Tcl_NewListObj(0, NULL);

		GetControls(tuvc, list);
		Tcl_SetObjResult(interp, list);
	    } else {
		ret = SetControls(tuvc, objc - 3, objv + 3);
		if (ret == TCL_OK) {
		    Tcl_Obj *list = Tcl_NewListObj(0, NULL);

		    GetControls(tuvc, list);
		    Tcl_SetObjResult(interp, list);
		}
	    }
	} else {
	    goto devNotFound;
	}
	break;

    case CMD_record:
	if (objc < 4) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid cmd ...");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	} else {
	    goto devNotFound;
	}
	if (Tcl_GetIndexFromObj(interp, objv[3], recNames, "option", 0,
				&command) != TCL_OK) {
	    return TCL_ERROR;
	}
	switch ((enum recCode) command) {
	case REC_frame:
	    if (RecordFrameFromData(tuvc, interp, objc, objv) != TCL_OK) {
		return TCL_ERROR;
	    }
	    break;
	case REC_pause:
	    if (objc != 4) {
		Tcl_WrongNumArgs(interp, 2, objv, "devid pause");
		return TCL_ERROR;
	    }
	    if (tuvc->rstate == REC_RECPRI) {
		tuvc->rstate = REC_PAUSEPRI;
	    } else if (tuvc->rstate == REC_RECORD) {
		tuvc->rstate = REC_PAUSE;
	    } else if ((tuvc->rstate != REC_PAUSEPRI) &&
		       (tuvc->rstate != REC_PAUSE)) {
		Tcl_SetResult(interp, "wrong recording state for pause",
			      TCL_STATIC);
		return TCL_ERROR;
	    }
	    break;
	case REC_resume:
	    if (objc != 4) {
		Tcl_WrongNumArgs(interp, 2, objv, "devid resume");
		return TCL_ERROR;
	    }
	    if (tuvc->rstate == REC_PAUSEPRI) {
		if (tuvc->running) {
		    gettimeofday(&tuvc->ltv, NULL);
		    tuvc->rtv = tuvc->ltv;
		    tuvc->rstate = REC_RECPRI;
		}
	    } else if (tuvc->rstate == REC_PAUSE) {
		if (tuvc->running) {
		    gettimeofday(&tuvc->ltv, NULL);
		    tuvc->rtv = tuvc->ltv;
		    tuvc->rstate = REC_RECORD;
		}
	    } else if ((tuvc->rstate != REC_RECPRI) &&
		       (tuvc->rstate != REC_RECORD)) {
		Tcl_SetResult(interp, "wrong recording state for resume",
			      TCL_STATIC);
		return TCL_ERROR;
	    }
	    break;
	case REC_start:
	    if (StartRecording(tuvc, interp, objc, objv) != TCL_OK) {
		return TCL_ERROR;
	    }
	    break;
	case REC_state:
	    if (objc != 4) {
		Tcl_WrongNumArgs(interp, 2, objv, "devid state");
		return TCL_ERROR;
	    }
	    switch (tuvc->rstate) {
	    default:
	    case REC_STOP:
		Tcl_SetResult(interp, "stop", TCL_STATIC);
		break;
	    case REC_RECPRI:
	    case REC_RECORD:
		Tcl_SetResult(interp, "recording", TCL_STATIC);
		break;
	    case REC_PAUSEPRI:
	    case REC_PAUSE:
		Tcl_SetResult(interp, "pause", TCL_STATIC);
		break;
	    case REC_ERROR:
		Tcl_SetResult(interp, "error", TCL_STATIC);
		break;
	    }
	    break;
	case REC_stop:
	    if (objc != 4) {
		Tcl_WrongNumArgs(interp, 2, objv, "devid stop");
		return TCL_ERROR;
	    }
	    if (tuvc->rstate > REC_STOP) {
		tuvc->rstate = REC_STOP;
	    }
	    FinishRecording(tuvc, 1, 0);
	    break;
	}
	break;

    case CMD_start:
	if (objc != 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	    ret = StartCapture(tuvc);
	} else {
	    goto devNotFound;
	}
	break;

    case CMD_state:
	if (objc != 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	    Tcl_SetResult(interp, (tuvc->running < 0) ? "error" :
			  (tuvc->running ? "capture" : "stopped"),
			  TCL_STATIC);
	} else {
	    goto devNotFound;
	}
	break;

    case CMD_stop:
	if (objc != 3) {
	    Tcl_WrongNumArgs(interp, 2, objv, "devid");
	    return TCL_ERROR;
	}
	hPtr = Tcl_FindHashEntry(&tuvci->tuvcc, Tcl_GetString(objv[2]));
	if (hPtr != NULL) {
	    tuvc = (TUVC *) Tcl_GetHashValue(hPtr);
	    ret = StopCapture(tuvc);
	} else {
	    goto devNotFound;
	}
	break;

    case CMD_tophoto:
	if (DataToPhoto(tuvci, interp, objc, objv) != TCL_OK) {
	    return TCL_ERROR;
	}
	break;

    }

    return ret;
}

/*
 *-------------------------------------------------------------------------
 *
 * Tcluvc_Init --
 *
 *	Module initializer:
 *	  - require Tcl/Tk infrastructure
 *	  - dynamic link libusb
 *	  - (optional) setup udev for plug/unplug events
 *	  - initialize module data structures
 *
 * Results:
 *	A standard Tcl result.
 *
 * Side effects:
 *	See the user documentation.
 *
 *-------------------------------------------------------------------------
 */

int
Tcluvc_Init(Tcl_Interp *interp)
{
    uvc_context_t *ctx = NULL;
    uvc_error_t uret;
    TUVCI *tuvci;

#ifdef USE_TCL_STUBS
    if (Tcl_InitStubs(interp, "8.4-", 0) == NULL) {
	return TCL_ERROR;
    }
#else
    if (Tcl_PkgRequire(interp, "Tcl", "8.4-", 0) == NULL) {
	return TCL_ERROR;
    }
#endif
    if (!uvcInitialized) {
#if defined(ANDROID) && !defined(__TERMUX__)
	Tcl_DString ds;
	const char *path;
#endif
	int major = 0, minor = 0;
	const char *val;

	Tcl_MutexLock(&uvcMutex);
	if (uvcInitialized) {
	    Tcl_MutexUnlock(&uvcMutex);
	    goto doInit;
	}

	if ((Tcl_EvalEx(interp, "::tcl::pkgconfig get threaded", -1, 0)
	     != TCL_OK) ||
	    (Tcl_GetStringResult(interp)[0] != '1')) {
	    uvcInitialized = -1;
	    goto doInit;
	}
	Tcl_ResetResult(interp);

	Tcl_GetVersion(&major, &minor, NULL, NULL);
	if ((major > 8) || ((major == 8) && (minor > 6))) {
	    tip609 = 1;
	} else {
	    val = Tcl_GetVar2(interp, "tcl_platform", "tip609",
			      TCL_GLOBAL_ONLY);
	    if ((val != NULL) && *val && (*val != '0')) {
		tip609 = 1;
	    }
	}

	/* dynamic link libusb */
	(void) dlerror();
#if defined(ANDROID) && !defined(__TERMUX__)
	Tcl_DStringInit(&ds);
	path = getenv("INTERNAL_STORAGE");
	if (path != NULL) {
	    char *end;

	    Tcl_DStringAppend(&ds, path, -1);
	    end = strrchr(Tcl_DStringValue(&ds), '/');
	    if (end == NULL) {
		Tcl_DStringAppend(&ds, "/../lib/", -1);
	    } else {
		Tcl_DStringSetLength(&ds, end - Tcl_DStringValue(&ds));
		Tcl_DStringAppend(&ds, "/lib/", -1);
	    }
	}
	Tcl_DStringAppend(&ds, LIBUSB_SO, -1);
	libusb = dlopen(Tcl_DStringValue(&ds), RTLD_NOW | RTLD_GLOBAL);
	Tcl_DStringFree(&ds);
	if (libusb == NULL) {
	    (void) dlerror();
	    libusb = dlopen(LIBUSB_SO, RTLD_NOW | RTLD_GLOBAL);
	}
#else
	libusb = dlopen(LIBUSB_SO, RTLD_NOW);
#endif
	if (libusb == NULL) {
libusbError:
	    Tcl_SetObjResult(interp,
		    Tcl_ObjPrintf("unable to link " LIBUSB_SO ": %s",
				  dlerror()));
	    if (libusb != NULL) {
		dlclose(libusb);
		libusb = NULL;
	    }
	    Tcl_MutexUnlock(&uvcMutex);
	    return TCL_ERROR;
	}

#define USBDLSYM(name)							      \
	libusb_dl.name = (fn_libusb_ ## name) dlsym(libusb, "libusb_" #name); \
	if (libusb_dl.name == NULL) goto libusbError

#define USBDLSYM_NO_CHECK(name)						      \
	libusb_dl.name = (fn_libusb_ ## name) dlsym(libusb, "libusb_" #name);

	USBDLSYM(alloc_transfer);
	USBDLSYM(attach_kernel_driver);
	USBDLSYM(cancel_transfer);
	USBDLSYM(claim_interface);
	USBDLSYM(close);
	USBDLSYM(control_transfer);
	USBDLSYM(detach_kernel_driver);
	USBDLSYM(exit);
	USBDLSYM(free_config_descriptor);
	USBDLSYM(free_device_list);
	USBDLSYM(free_transfer);
	USBDLSYM(get_bus_number);
	USBDLSYM(get_config_descriptor);
	USBDLSYM(get_device_address);
	USBDLSYM(get_device_descriptor);
	USBDLSYM(get_device_list);
	USBDLSYM(get_string_descriptor_ascii);
	USBDLSYM(handle_events);
	USBDLSYM(init);
	USBDLSYM(open);
	USBDLSYM(ref_device);
	USBDLSYM(release_interface);
	USBDLSYM(set_interface_alt_setting);
	USBDLSYM(submit_transfer);
	USBDLSYM(unref_device);
	USBDLSYM(clear_halt);
#ifdef __TERMUX__
	USBDLSYM(wrap_sys_device);
#endif
	USBDLSYM_NO_CHECK(handle_events_completed);

#undef USBDLSYM

#ifdef HAVE_LIBUDEV
	/* dynamic link libudev */
	libudev = dlopen("libudev.so.1", RTLD_NOW);
	if (libudev == NULL) {
	    libudev = dlopen("libudev.so.0", RTLD_NOW);
	    if (libudev == NULL) {
		goto libudevEnd;
libudevError:
		dlclose(libudev);
		libudev = NULL;
		goto libudevEnd;
	    }
	}

#define UDEVDLSYM(name)							\
	udev_dl.name = (fn_ ## name) dlsym(libudev, "udev_" #name);	\
	if (udev_dl.name == NULL) goto libudevError

	UDEVDLSYM(device_get_action);
	UDEVDLSYM(device_get_devnode);
	UDEVDLSYM(device_get_property_value);
	UDEVDLSYM(device_get_sysattr_value);
	UDEVDLSYM(device_new_from_syspath);
	UDEVDLSYM(device_unref);
	UDEVDLSYM(monitor_get_fd);
	UDEVDLSYM(monitor_receive_device);
	UDEVDLSYM(monitor_unref);
	UDEVDLSYM(new);
	UDEVDLSYM(unref);
	UDEVDLSYM(monitor_enable_receiving);
	UDEVDLSYM(monitor_filter_add_match_subsystem_devtype);
	UDEVDLSYM(monitor_new_from_netlink);
	UDEVDLSYM(enumerate_new);
	UDEVDLSYM(enumerate_add_match_subsystem);
	UDEVDLSYM(enumerate_get_list_entry);
	UDEVDLSYM(enumerate_scan_devices);
	UDEVDLSYM(enumerate_unref);
	UDEVDLSYM(list_entry_get_name);
	UDEVDLSYM(list_entry_get_next);

#undef UDEVDLSYM

libudevEnd:
	;
#endif	/* HAVE_LIBUDEV */

	uvcInitialized = 1;
	Tcl_MutexUnlock(&uvcMutex);
    }

doInit:
    if (uvcInitialized < 0) {
	Tcl_SetResult(interp, "thread support unavailable", TCL_STATIC);
	return TCL_ERROR;
    }

    uret = uvc_init(&ctx, NULL);
    if (uret < 0) {
	Tcl_SetObjResult(interp,
			 Tcl_ObjPrintf("error initializing libuvc: %s (%d)",
				       uvc_strerror(uret), uret));
	return TCL_ERROR;
    }
    if (Tcl_PkgProvide(interp, PACKAGE_NAME, PACKAGE_VERSION) != TCL_OK) {
	uvc_exit(ctx);
	return TCL_ERROR;
    }
    tuvci = (TUVCI *) ckalloc(sizeof(TUVCI));
    memset(tuvci, 0, sizeof(TUVCI));
    tuvci->idCount = 0;
    tuvci->checkedTk = 0;
    tuvci->ctx = ctx;
    tuvci->enc = Tcl_GetEncoding(NULL, "utf-8");
    Tcl_InitHashTable(&tuvci->tuvcc, TCL_STRING_KEYS);

#ifdef HAVE_LIBUDEV
    /* setup udev */
    tuvci->interp = interp;
    Tcl_InitHashTable(&tuvci->devs, TCL_STRING_KEYS);
    Tcl_DStringInit(&tuvci->cbCmd);
    tuvci->cbCmdLen = 0;
    tuvci->udev = (libudev == NULL) ? NULL : udev_new();
    if (tuvci->udev != NULL) {
	tuvci->udevMon = udev_monitor_new_from_netlink(tuvci->udev, "udev");
	if (tuvci->udevMon == NULL) {
	    udev_unref(tuvci->udev);
	    tuvci->udev = NULL;
	}
    }
    if (tuvci->udevMon != NULL) {
	struct udev_enumerate *udevEnum;

	/* watch "usb" subsystem */
	udev_monitor_filter_add_match_subsystem_devtype(tuvci->udevMon,
	    "usb", NULL);
	udev_monitor_enable_receiving(tuvci->udevMon);
	Tcl_CreateFileHandler(udev_monitor_get_fd(tuvci->udevMon),
	    TCL_READABLE, UdevMonitor, (ClientData) tuvci);
	/* initial device scan */
	udevEnum = udev_enumerate_new(tuvci->udev);
	if (udevEnum == NULL) {
	    /* trouble ahead... */
	    Tcl_DeleteFileHandler(udev_monitor_get_fd(tuvci->udevMon));
	    udev_monitor_unref(tuvci->udevMon);
	    tuvci->udevMon = NULL;
	    udev_unref(tuvci->udev);
	    tuvci->udev = NULL;
	} else {
	    UdevScan(tuvci, udevEnum);
	    udev_enumerate_unref(udevEnum);
	}
    }
#endif	/* HAVE_LIBUDEV */

    Tcl_CreateObjCommand(interp, "uvc", UvcObjCmd,
			 (ClientData) tuvci, UvcObjCmdDeleted);
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * tab-width: 8
 * End:
 */
