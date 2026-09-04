// /usr/src/linux-headers-6.17.0-41-generic/include/linux/usb/uas.h
// #include "include/scsi_cmds.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/uas.h>
#include <linux/string.h>
#include <linux/completion.h>
#include <linux/mutex.h>

#include "my_uas_dev.h"
#include "uas_scsi.h"

static struct usb_device_id my_uas_table[] = {
	{ USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
	{ USB_INTERFACE_INFO(USB_MSC_CLASS, USB_SCSI_SUBCLASS, USB_UAS_PROTOCOL) },
	{ }
};
MODULE_DEVICE_TABLE(usb, my_uas_table);

static int uas_probe(struct usb_interface *interface, const struct usb_device_id *usb_id)
{
	struct my_uas_dev *dev;
	struct usb_device *udev = interface_to_usbdev(interface);
	struct usb_host_interface *alternate_setting;
	struct usb_endpoint_descriptor *endpoint_desc;
	int interface_num; //to get the current interface number
    struct usb_host_endpoint *eps[3];  //to create endpoint pipe array
    int n;
	int i;
	int ret;  //store the ret value
    u8 *read10_buffer;  //buffer to pass to store the data read by the read 10 command

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

    memset(dev , 0 , sizeof(*dev));   

    //initializing the mutex variale stored inside the my_uas_dev structure
    mutex_init(&dev->lock);

	dev->udev = usb_get_dev(udev);
	dev->interface = interface;
	usb_set_intfdata(interface, dev);  //associate device specific data with interface 

	interface_num = interface->cur_altsetting->desc.bInterfaceNumber;

	// Device defaults to alt 0 = BBB (0x50). Switch to alt 1 = UAS (0x62).
	ret = usb_set_interface(udev, interface_num, 1);   //Makes a particular alternate setting be current
    //Here it makes the alternate setting 1 to be the current one
	if (ret) 
    {
		dev_err(&interface->dev,"%s: usb_set_interface(alt1) failed: %d\n", DRIVER_NAME, ret);
		goto err;
	}

	alternate_setting = interface->cur_altsetting;

	dev_info(&interface->dev, "%s: probe VID:PID=%04x:%04x speed=%d alt=%u proto=%02x eps=%u\n", DRIVER_NAME, le16_to_cpu(udev->descriptor.idVendor), le16_to_cpu(udev->descriptor.idProduct), udev->speed, alternate_setting->desc.bAlternateSetting, alternate_setting->desc.bInterfaceProtocol, alternate_setting->desc.bNumEndpoints);

	if (alternate_setting->desc.bInterfaceProtocol != USB_UAS_PROTOCOL) {
		dev_err(&interface->dev, "%s: still not UAS after alt1\n",	DRIVER_NAME);
		ret = -ENODEV;
		goto err;
	}

	for (i = 0; i < alternate_setting->desc.bNumEndpoints; i++) {
		u8 field;
		unsigned max_streams;

		endpoint_desc = &alternate_setting->endpoint[i].desc;

		dev_info(&interface->dev, "%s: ep[%d] addr=0x%02x attr=0x%02x maxpkt=%d\n", DRIVER_NAME, i, endpoint_desc->bEndpointAddress, endpoint_desc->bmAttributes, usb_endpoint_maxp(endpoint_desc));

		if (udev->speed >= USB_SPEED_SUPER) 
        {
			struct usb_ss_ep_comp_descriptor *ss = &alternate_setting->endpoint[i].ss_ep_comp;

			field = ss->bmAttributes & 0x1f;
			max_streams = field ? (1U << field) : 0;
			dev_info(&interface->dev, "%s: ep[%d] burst=%u MaxStreams=%u\n", DRIVER_NAME, i, ss->bMaxBurst, max_streams);
		}
	}

	dev->ep_cmd = EP_CMD;
	dev->ep_status = EP_STATUS;
	dev->ep_data_in = EP_DATA_IN;
	dev->ep_data_out = EP_DATA_OUT;

    //printing all the address of the parsed endpoint pipes
	dev_info(&interface->dev, "%s: cmd=0x%02x status=0x%02x data_in=0x%02x data_out=0x%02x\n", DRIVER_NAME, dev->ep_cmd, dev->ep_status, dev->ep_data_in, dev->ep_data_out);

    //populate the endpoint array
    unsigned int pipe_in = usb_rcvbulkpipe(udev , dev -> ep_data_in);
    eps[0] = usb_pipe_endpoint(udev, pipe_in);
    unsigned int pipe_out = usb_sndbulkpipe(udev , dev -> ep_data_out);
    eps[1] = usb_pipe_endpoint(udev, pipe_out);
    unsigned int pipe_status = usb_rcvbulkpipe(udev , dev -> ep_status);  //status pipe is IN Bulk pipe
    eps[2] = usb_pipe_endpoint(udev , pipe_status);

    if(!eps[0] || !eps[1] || !eps[2])
    {
        dev_err(&interface->dev, "%s: missing data endpoints for streams\n", DRIVER_NAME);
	    ret = -ENODEV;
	    goto err;
    }

    n = usb_alloc_streams(interface, eps, 3, 32, GFP_KERNEL);
    if (n < 0) 
    {
	    dev_err(&interface->dev, "%s: usb_alloc_streams failed: %d\n", DRIVER_NAME, n);
	    ret = n;
	    goto err;
    }

    dev -> num_streams = n;
    dev_info(&interface->dev, "%s: allocated %d streams\n", DRIVER_NAME, dev->num_streams);

    ret = do_tur_cmd(dev);
    if(ret)
    {
        printk(KERN_WARNING "my_uas_driver: tur failed %d\n", ret);
    }
    else
    {
        printk(KERN_INFO "my_uas_driver: tur is completed and done successfully\n");
    }


    ret = do_scsi_inquiry(dev);
    if(ret)
    {
        printk(KERN_ERR "my_uas_driver: Inquiry command failed\n");
    }
    else
    {
        printk(KERN_INFO "my_uas_driver: INQUIRY is completed and done successfully\n");
    }

    ret = do_read_capacity_command(dev);
    if(ret)
    {
        printk(KERN_ERR "my_uas_driver: READ CAPACITY command failed\n");
    }
    else
    {
        printk(KERN_INFO "my_uas_driver: READ CAPACITY is completed and done successfully\n");
    }


    read10_buffer = kzalloc(dev->block_len, GFP_KERNEL);
    if (read10_buffer) 
    {
        ret = do_read10(dev, 0, 1, read10_buffer);
        if (ret)
        {
            printk(KERN_ERR "my_uas_driver: READ10 failed %d\n", ret);
        }
        else 
        {
            printk(KERN_INFO "my_uas_driver: READ10 LBA0 first16: %*ph\n", 16, read10_buffer);
            if (dev->block_len >= 512)
            {
                printk(KERN_INFO "my_uas_driver: MBR sig %02x %02x\n", read10_buffer[510], read10_buffer[511]);
            }    
            printk(KERN_INFO "my_uas_driver: READ10 is done and completed successfully\n");
        }
        kfree(read10_buffer);
    }


	dev_info(&interface->dev, "%s: probe OK (UAS alt1)\n", DRIVER_NAME);
	return 0;

err:
	usb_set_intfdata(interface, NULL);
	usb_put_dev(dev->udev);
	kfree(dev);
	return ret;
}

static void uas_disconnect(struct usb_interface *interface)
{
	struct my_uas_dev *dev = usb_get_intfdata(interface);

    if (dev->num_streams > 0) 
    {
        struct usb_host_endpoint *eps[3];

        unsigned int pipe_in, pipe_out, pipe_status;

        pipe_in = usb_rcvbulkpipe(dev->udev, dev->ep_data_in);
        pipe_out = usb_sndbulkpipe(dev->udev, dev->ep_data_out);
        pipe_status = usb_rcvbulkpipe(dev->udev, dev->ep_status);

        eps[0] = usb_pipe_endpoint(dev->udev, pipe_in);
        eps[1] = usb_pipe_endpoint(dev->udev, pipe_out);
        eps[2] = usb_pipe_endpoint(dev->udev, pipe_status);

        if (eps[0] && eps[1] && eps[2])
            usb_free_streams(interface, eps, 3, GFP_KERNEL);

        dev->num_streams = 0;
    }


	usb_set_intfdata(interface, NULL);
	if (dev) 
    {
		usb_put_dev(dev->udev);
		kfree(dev);
	}
	dev_info(&interface->dev, "%s: disconnect\n", DRIVER_NAME);
}

static struct usb_driver my_uas_driver = {    //identifies usb device to the usb core
	.name = DRIVER_NAME,
	.id_table = my_uas_table,
	.probe = uas_probe,
	.disconnect = uas_disconnect,
};

static int __init uas_init(void)
{
	int ret;

	pr_info("%s: init — registering\n", DRIVER_NAME);
	ret = usb_register(&my_uas_driver);
	if (ret) {
		pr_err("%s: usb_register failed %d\n", DRIVER_NAME, ret);
		return ret;
	}
	pr_info("%s: registered\n", DRIVER_NAME);
	return 0;
}

static void __exit uas_exit(void)
{
	usb_deregister(&my_uas_driver);
	pr_info("%s: exit\n", DRIVER_NAME);
}

module_init(uas_init);
module_exit(uas_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Siddarth M");
MODULE_DESCRIPTION("UAS driver for Cablet RTL9210 — probe-alt1-endpoints");
