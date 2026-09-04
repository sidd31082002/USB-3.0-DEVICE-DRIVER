/* Private device data + IDs for UAS driver */
#ifndef MY_UAS_DEV_H
#define MY_UAS_DEV_H

#include <linux/usb.h>
#include <linux/types.h>
#include <linux/mutex.h>

#define DRIVER_NAME	"my_uas_driver"

#define VENDOR_ID	0x0bda
#define PRODUCT_ID	0x9210

#define USB_UAS_PROTOCOL	0x62
#define USB_MSC_CLASS		0x08
#define USB_SCSI_SUBCLASS	0x06

#define EP_CMD		0x04	// bulk OUT — Command 
#define EP_STATUS	0x83	// bulk IN  — Status 
#define EP_DATA_IN	0x81	// bulk IN  — Data-in 
#define EP_DATA_OUT	0x02	// bulk OUT — Data-out 

struct my_uas_dev {
	struct usb_device	*udev;
	struct usb_interface	*interface;
    // Pipes: cmd=0x04 status=0x83 data_in=0x81 data_out=0x02
	u8	ep_cmd;
	u8	ep_status;
	u8	ep_data_in;
	u8	ep_data_out;

    int num_streams;

    u16 tag;

    u32 block_len;          // from READ CAPACITY,  512 normally 
    u32 last_lba;
    u64 capacity_sectors;   // last_lba + 1 

    struct mutex lock;      // one UAS cmd at a time later 

    struct gendisk *disk;
};

#endif /* MY_UAS_DEV_H */
