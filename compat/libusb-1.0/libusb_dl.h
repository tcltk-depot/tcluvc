#ifndef _LIBUSB_DL_H
#define _LIBUSB_DL_H

typedef struct libusb_transfer * LIBUSB_CALL
	(*fn_libusb_alloc_transfer)(int);
typedef int LIBUSB_CALL
	(*fn_libusb_attach_kernel_driver)(libusb_device_handle *, int);
typedef int LIBUSB_CALL
	(*fn_libusb_cancel_transfer)(struct libusb_transfer *);
typedef int LIBUSB_CALL
	(*fn_libusb_claim_interface)(libusb_device_handle *, int);
typedef void LIBUSB_CALL
	(*fn_libusb_close)(libusb_device_handle *);
typedef int LIBUSB_CALL
	(*fn_libusb_control_transfer)(libusb_device_handle *, uint8_t,
				      uint8_t, uint16_t, uint16_t,
				      unsigned char *, uint16_t, unsigned int);
typedef int LIBUSB_CALL
	(*fn_libusb_detach_kernel_driver)(libusb_device_handle *, int);
typedef void LIBUSB_CALL
	(*fn_libusb_exit)(libusb_context *);
typedef void LIBUSB_CALL
	(*fn_libusb_free_config_descriptor)(struct libusb_config_descriptor *);
typedef void LIBUSB_CALL
	(*fn_libusb_free_device_list)(libusb_device **, int);
typedef void LIBUSB_CALL
	(*fn_libusb_free_transfer)(struct libusb_transfer *);
typedef uint8_t LIBUSB_CALL
	(*fn_libusb_get_bus_number)(libusb_device *);
typedef int LIBUSB_CALL
	(*fn_libusb_get_config_descriptor)(libusb_device *, uint8_t,
					   struct libusb_config_descriptor **);
typedef uint8_t LIBUSB_CALL
	(*fn_libusb_get_device_address)(libusb_device *);
typedef int LIBUSB_CALL
	(*fn_libusb_get_device_descriptor)(libusb_device *,
					   struct libusb_device_descriptor *);
typedef ssize_t LIBUSB_CALL
	(*fn_libusb_get_device_list)(libusb_context *, libusb_device ***);
typedef int LIBUSB_CALL
	(*fn_libusb_get_string_descriptor_ascii)(libusb_device_handle *,
						 uint8_t, unsigned char *, int);
typedef int LIBUSB_CALL
	(*fn_libusb_handle_events)(libusb_context *);
typedef int LIBUSB_CALL
	(*fn_libusb_handle_events_completed)(libusb_context *, int *);
typedef int LIBUSB_CALL
	(*fn_libusb_init)(libusb_context **);
typedef int LIBUSB_CALL
	(*fn_libusb_open)(libusb_device *, libusb_device_handle **);
typedef libusb_device * LIBUSB_CALL
	(*fn_libusb_ref_device)(libusb_device *);
typedef int LIBUSB_CALL
	(*fn_libusb_release_interface)(libusb_device_handle *, int);
typedef int LIBUSB_CALL
	(*fn_libusb_set_interface_alt_setting)(libusb_device_handle *,
					       int, int);
typedef int LIBUSB_CALL
	(*fn_libusb_submit_transfer)(struct libusb_transfer *);
typedef void LIBUSB_CALL
	(*fn_libusb_unref_device)(libusb_device *);
typedef int LIBUSB_CALL
	(*fn_libusb_clear_halt)(libusb_device_handle *, unsigned char);

#ifdef __TERMUX__
typedef int LIBUSB_CALL
	(*fn_libusb_wrap_sys_device)(libusb_context *, void *, libusb_device_handle **);
#endif

struct libusb_dl {
  fn_libusb_alloc_transfer alloc_transfer;
  fn_libusb_attach_kernel_driver attach_kernel_driver;
  fn_libusb_cancel_transfer cancel_transfer;
  fn_libusb_claim_interface claim_interface;
  fn_libusb_close close;
  fn_libusb_control_transfer control_transfer;
  fn_libusb_detach_kernel_driver detach_kernel_driver;
  fn_libusb_exit exit;
  fn_libusb_free_config_descriptor free_config_descriptor;
  fn_libusb_free_device_list free_device_list;
  fn_libusb_free_transfer free_transfer;
  fn_libusb_get_bus_number get_bus_number;
  fn_libusb_get_config_descriptor get_config_descriptor;
  fn_libusb_get_device_address get_device_address;
  fn_libusb_get_device_descriptor get_device_descriptor;
  fn_libusb_get_device_list get_device_list;
  fn_libusb_get_string_descriptor_ascii get_string_descriptor_ascii;
  fn_libusb_handle_events handle_events;
  fn_libusb_handle_events_completed handle_events_completed;
  fn_libusb_init init;
  fn_libusb_open open;
  fn_libusb_ref_device ref_device;
  fn_libusb_release_interface release_interface;
  fn_libusb_set_interface_alt_setting set_interface_alt_setting;
  fn_libusb_submit_transfer submit_transfer;
  fn_libusb_unref_device unref_device;
  fn_libusb_clear_halt clear_halt;
#ifdef __TERMUX__
  fn_libusb_wrap_sys_device wrap_sys_device;
#endif
};

extern struct libusb_dl libusb_dl;

#define libusb_alloc_transfer libusb_dl.alloc_transfer
#define libusb_attach_kernel_driver  libusb_dl.attach_kernel_driver
#define libusb_cancel_transfer libusb_dl.cancel_transfer
#define libusb_claim_interface libusb_dl.claim_interface
#define libusb_close libusb_dl.close
#define libusb_control_transfer libusb_dl.control_transfer
#define libusb_detach_kernel_driver libusb_dl.detach_kernel_driver
#define libusb_exit libusb_dl.exit
#define libusb_free_config_descriptor libusb_dl.free_config_descriptor
#define libusb_free_device_list libusb_dl.free_device_list
#define libusb_free_transfer libusb_dl.free_transfer
#define libusb_get_bus_number libusb_dl.get_bus_number
#define libusb_get_config_descriptor libusb_dl.get_config_descriptor
#define libusb_get_device_address libusb_dl.get_device_address
#define libusb_get_device_descriptor libusb_dl.get_device_descriptor
#define libusb_get_device_list libusb_dl.get_device_list
#define libusb_get_string_descriptor_ascii libusb_dl.get_string_descriptor_ascii
#define libusb_handle_events libusb_dl.handle_events
#define libusb_handle_events_completed libusb_dl.handle_events_completed
#define libusb_init libusb_dl.init
#define libusb_open libusb_dl.open
#define libusb_ref_device libusb_dl.ref_device
#define libusb_release_interface libusb_dl.release_interface
#define libusb_set_interface_alt_setting libusb_dl.set_interface_alt_setting
#define libusb_submit_transfer libusb_dl.submit_transfer
#define libusb_unref_device libusb_dl.unref_device
#define libusb_clear_halt libusb_dl.clear_halt
#ifdef __TERMUX__
#define libusb_wrap_sys_device libusb_dl.wrap_sys_device
#endif

#endif /* _LIBUSB_DL_H */
