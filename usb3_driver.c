#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>  //__init __exit macros
#include <linux/slab.h> // Required for kmalloc()
#include <linux/usb.h> //required for usb structures and macros
#include <linux/delay.h>   //to provide delay 

#include "my_usb_dev.h"
#include "scsi_cmds.h"

static const char *my_speed_str(enum usb_device_speed speed)
{
	switch (speed) {
	case USB_SPEED_LOW:
		return "Low (1.5 Mbps)";
	case USB_SPEED_FULL:
		return "Full (12 Mbps)";
	case USB_SPEED_HIGH:
		return "High (480 Mbps)";
	case USB_SPEED_SUPER:
		return "SuperSpeed (5 Gbps)";
	case USB_SPEED_SUPER_PLUS:
		return "SuperSpeed+ (10+ Gbps)";
	default:
		return "Unknown";
	}
}

//creating the usb device id table
//this basically acts as a way for the kernel to match any currently attached device against all the available driver and load the appropriate driver 
static struct usb_device_id my_usb_table[] = {
	{ USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
	{ USB_INTERFACE_INFO(0x08, 0x06, 0x50) }, //using this feild will help us create and use the interface details of usb devices
	{ }
};

/*IMP IMP
   USB drivers use ID table to support hotplugging. Export this with MODULE_DEVICE_TABLE(usb,...). 
   This must be set or your driver’s probe function will never get called.
IMP IMP*/

//we need to make the user space know about this usb device table 
MODULE_DEVICE_TABLE(usb, my_usb_table);

//if another driver is already installed and indicates that it is responsible for the device attached ..then 
//...your probe will not get called
//simply the probe function will not get called if another driver has taken claim of this particular device registerd with this device
// struct usb_interface *interface -> this structure already contains the parsed descriptor data of the device(interface descriptor)
static int my_usb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	//creating a var for my usb device private structure
	struct my_usb_dev *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	struct usb_device *udev;
	struct usb_endpoint_descriptor *endpoint_desc; //pointer to a structure that can store the info about the endpoint
	struct usb_host_interface *curr_interface_alt_setting; //pointer to store the curr alternate settings of the interface
	int ret;

	udev = interface_to_usbdev(interface);  //udev will contain the parsed descriptor data and info of the device

	if (!udev->bos) 
    {
		pr_info("bos desc not present!!\n");
	} 
    else 
    {
		pr_info("my_usb_driver : BOS desc is present!!\n");
	}

	//populate the private data structure(my_usb_dev)
	//step 1 -> increment the refernce count of the usb device we claimed
	dev->udev = usb_get_dev(udev);

	//store the current interface of the usb device in our private usb device data structure
	dev->intf = interface;

	//usb_set_intfdata -> associate driver specific data with an interface
	usb_set_intfdata(interface, dev);

	// struct usb_host_interface *usb_altnum_to_altsetting(const struct usb_interface *intf, unsigned int altnum)
	// get the altsetting structure with a given alternate setting number.

	curr_interface_alt_setting = interface->cur_altsetting;  //usb_host_interface contains : 
								//├── .desc  (interface descriptor)
								//└── .endpoint[] arrays (endpoints)

	printk(KERN_INFO "my_usb_driver : from my_usb_driver_probe() : device connected\n");

	printk(KERN_INFO "my_usb_driver: VID:PID = %04x:%04x\n", le16_to_cpu(udev->descriptor.idVendor), le16_to_cpu(udev->descriptor.idProduct));
							//converts to host machine byte ordering() -> le16_to_cpu()

	/*Printing speed and LPM logs*/
	printk(KERN_INFO "my_usb_driver: speed=%d (%s) bcdUSB=0x%04x\n", udev->speed, my_speed_str(udev->speed), le16_to_cpu(udev->descriptor.bcdUSB)); //speed for the device enumerated 

	printk(KERN_INFO "my_usb_driver: LPM capable=%u  U1_enabled=%u  U2_enabled=%u\n", udev->lpm_capable, udev->usb3_lpm_u1_enabled, udev->usb3_lpm_u2_enabled);

	//printing the device descriptor data
	printk(KERN_INFO "my_usb_driver : dev bLength=%u bDescriptorType=%u bcdUSB=0x%04x\n", udev->descriptor.bLength, udev->descriptor.bDescriptorType, udev->descriptor.bcdUSB);

	if (udev->descriptor.bLength != 18 || udev->descriptor.bDescriptorType != USB_DT_DEVICE) {
		goto err_free;
	}

	//printing the class , subclass , protocol for this interface , number of endpoints
	printk(KERN_INFO "my_usb_driver: class=%02x\n", curr_interface_alt_setting->desc.bInterfaceClass);

	printk(KERN_INFO "my_usb_driver: subclass=%02x\n", curr_interface_alt_setting->desc.bInterfaceSubClass);

	printk(KERN_INFO "my_usb_driver: protocol=%02x\n", curr_interface_alt_setting->desc.bInterfaceProtocol);

	printk(KERN_INFO "my_usb_driver: endpoints=%u\n", curr_interface_alt_setting->desc.bNumEndpoints);

	//validate class , subclass and protocol
	if (curr_interface_alt_setting->desc.bInterfaceClass != 0x08 || curr_interface_alt_setting->desc.bInterfaceSubClass != 0x06 || curr_interface_alt_setting->desc.bInterfaceProtocol != 0x50) {
		goto err_free;
	}

	//USB_SPEED_HIGH -> 480mbps
	//USB_SPEED_FULL -> 12mbps
	//USB_SPEED_LOW -> 1.5mbps
	if (udev->speed < USB_SPEED_SUPER) 
    {
		printk(KERN_WARNING "my_usb_driver: not SuperSpeed (speed=%d) — SS companions may be unused\n", udev->speed);
	} 
    else 
    {
		printk(KERN_INFO "my_usb_driver: SuperSpeed link — parsing SS companions\n");
	}

	//now we are going to get the address of each endpoint associated with this curr altsetting interface 
	// USB data does not go through the interface abstractly — it goes through endpoints.
	//endpoints are in curr_interface_alt_setting->endpoint[i] and the descriptor is  in ..->endpoint[i].desc
	for (int i = 0; i < curr_interface_alt_setting->desc.bNumEndpoints; i++) {
		endpoint_desc = &curr_interface_alt_setting->endpoint[i].desc;

		printk(KERN_INFO "my_usb_driver: ep[%d] address=0x%02x\n", i, endpoint_desc->bEndpointAddress);

		/* SuperSpeed Endpoint Companion Descriptor (USB 3.x) */
		if (udev->speed >= USB_SPEED_SUPER) {
			struct usb_ss_ep_comp_descriptor *ss_comp = &curr_interface_alt_setting->endpoint[i].ss_ep_comp;

			printk(KERN_INFO "my_usb_driver: ep[%d] SS companion: bMaxBurst=%u bmAttributes=0x%02x wBytesPerInterval=%u\n", i, ss_comp->bMaxBurst, ss_comp->bmAttributes, le16_to_cpu(ss_comp->wBytesPerInterval));

			// streams: lower 5(0 : 4) bits of bmAttributes (if non-zero)
			if (ss_comp->bmAttributes & 0x1f) {
				printk(KERN_INFO "my_usb_driver: ep[%d] supports %u streams (MaxStreams field)\n", i, 1 << (ss_comp->bmAttributes & 0x1f));
			} else {
				printk(KERN_INFO "my_usb_driver: device does not support Streams\n");
			}
		}

		//store the type of this particular endpoint in the private data structure(ep_in or ep_out)
		if (usb_endpoint_is_bulk_in(endpoint_desc)) //api to check whether the endpoint is IN
		{
			dev->ep_in = endpoint_desc->bEndpointAddress;
			dev->maxpkt_in = usb_endpoint_maxp(endpoint_desc);
		} else if (usb_endpoint_is_bulk_out(endpoint_desc))  //api to check whether the endpoint is OUT
		{
			dev->ep_out = endpoint_desc->bEndpointAddress;
			dev->maxpkt_out = usb_endpoint_maxp(endpoint_desc);
		}

		printk(KERN_INFO "my_usb_driver: ep[%d] attributes=0x%02x\n", i, endpoint_desc->bmAttributes);

		printk(KERN_INFO "my_usb_driver: ep[%d] maxpkt=%d\n", i, usb_endpoint_maxp(endpoint_desc));
	}
	if (!dev->ep_in || !dev->ep_out) {
		goto err_free;
	}

	//do the inquiry scsi command
	//AT first kernel crash happened with inquiry failed with ret value -11
	ret = do_scsi_inquiry(dev);
	if (ret) {
		printk(KERN_ERR "my_usb_driver: INQUIRY failed: %d\n", ret);
		// goto err_free;
	}

	//do the test unit data scsi command
	// ret = do_test_unit_ready(dev);
	for (int i = 0; i < 5; i++) {
		ret = do_test_unit_ready(dev);
		if (ret == 0) {
			ret = do_read_capacity(dev);
			if (ret) {
				printk(KERN_ERR "READ CAPACITY FAILED TO READ DATA with error code %d", ret);
				goto err_free;
			}
			break;
		}

		msleep(300);
	}
	if (ret) {
		printk(KERN_ERR "my_usb_driver: Test Unit ready FAILED with error code(Is SD Card inserted? ) %d", ret);
		//do the request sense if tur fails
		do_request_sense(dev);
	}

	//do the READ10 SCSI command
	ret = do_read10(dev, 0, 1);
	if (ret) {
		printk(KERN_ERR "my_usb_driver: READ10 failed: %d\n", ret);
	}

	return 0;

err_free:
	usb_set_intfdata(interface, NULL);
	usb_put_dev(dev->udev);  //dec the ref count for the usb device structure
	kfree(dev);
	return -ENODEV;
}

//whenever the pluggered device is removed
static void my_usb_disconnect(struct usb_interface *intf)
{
	struct my_usb_dev *dev = usb_get_intfdata(intf); //get the device associated data and store it in the private structure

	usb_set_intfdata(intf, NULL);

	if (dev != NULL) {
		usb_put_dev(dev->udev); //decrease the ref count of the particular device
		kfree(dev); //free 
	}
	printk(KERN_INFO "device removed\n");
}

//USB DRIVER structure initialization
//declaring the usb driver => basic definition of the usb driver
//this is like the device we are creating for the particular driver with the callback functions
static struct usb_driver my_usb_driver = {
	.name = "my_usb_driver",  //responsible for naming the driver as a whole
	.id_table = my_usb_table,  //it is used to match with this driver with any device 
	.probe = my_usb_probe,
	.disconnect = my_usb_disconnect,
};

static int __init usb_init(void)
{
	pr_info("my_usb_driver : Module loaded successfully.\n");
	int result = usb_register(&my_usb_driver);
	if (result) {
		pr_info("my_usb_driver - error during registering\n");
		return result;
	}

	return 0;
}

static void __exit usb_exit(void)
{
	usb_deregister(&my_usb_driver);
	pr_info("my_usb_driver : BYe , kernel\n");
}

module_init(usb_init);
module_exit(usb_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Siddarth M");
MODULE_DESCRIPTION("A sample usb driver for a peripheral usb device with USB 3.0 with superspeed");
