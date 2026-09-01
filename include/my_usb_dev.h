/* Private device data + IDs for BBB usb3 driver */
#ifndef MY_USB_DEV_H
#define MY_USB_DEV_H

#include <linux/usb.h>
#include <linux/types.h>

#define VENDOR_ID	0x8564
#define PRODUCT_ID	0x4000


//private usb device structure to hold necessary information we needed later
//bcz , probe function's local arguments die after probe returns
struct my_usb_dev {
	struct usb_device	*udev;   //whole device(includes the whole device information)
	struct usb_interface	*intf;   // the interface data we claimed 

	// Bulk pipes — same role as us->recv_bulk_pipe / send_bulk_pipe 
	u8	ep_in;		//0x81 — EPIN address 
	u8	ep_out;		// 0x02 - EPOUT address 
	u16	maxpkt_in;
	u16	maxpkt_out;

	u32 tag; //increments for every CBW , CSW must watch the tag is correct or not 
};

#endif /* MY_USB_DEV_H */
