#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/string.h>

#include "bbb_transport.h"

/* Clear halt on both bulk pipes after STALL / phase error */
int clear_bulk_stalls(struct my_usb_dev *dev)
{
	int ret_in, ret_out;
	unsigned int pipe_in;
	unsigned int pipe_out;

	printk(KERN_WARNING "my_usb_driver: clearing bulk endpoint stalls\n");

	//in pipe
	pipe_in = usb_rcvbulkpipe(dev->udev, dev->ep_in);
	ret_in = usb_clear_halt(dev->udev, pipe_in);     //internally calls CLEAR_FEATURE(ENDPOINT_HALT)

	//out pipe
	pipe_out = usb_sndbulkpipe(dev->udev, dev->ep_out);
	ret_out = usb_clear_halt(dev->udev, pipe_out);     //CLEAR_FEATURE(ENDPOINT_HALT)

	//account for the failure of usb_clear_halt
	if (ret_in)
		printk(KERN_ERR "my_usb_driver: clear_halt IN failed: %d\n", ret_in);

	if (ret_out)
		printk(KERN_ERR "my_usb_driver: clear_halt OUT failed: %d\n", ret_out);

	return (ret_in || ret_out) ? -EIO : 0;
}

int send_cbw(struct my_usb_dev *dev, struct bulk_cb_wrapper *cbw)
{
	int ret;
	int actual = 0;
	unsigned int pipe;

	pipe = usb_sndbulkpipe(dev->udev, dev->ep_out); //endpoint OUT 

	ret = usb_bulk_msg(dev->udev, pipe, cbw, sizeof(*cbw), &actual, 5000);	//timeout jiffies 5s

	//if usb_bulk_msg failed and also check for the bulk endpoints stall
	if (ret) {
		printk(KERN_ERR "my_usb_driver: send_cbw failed: %d\n", ret);
		if (ret == -EPIPE) {
			printk(KERN_ERR "my_usb_driver: STALL on CBW (bulk OUT)\n");
			clear_bulk_stalls(dev);
		}

		return ret;
	}

	//
	if (actual != sizeof(*cbw)) {
		printk(KERN_ERR "my_usb_driver: send_cbw short xfer: %d\n", actual);
		return -EIO;
	}
	return 0;
}

int recv_csw(struct my_usb_dev *dev, struct bulk_cs_wrapper *csw, u32 expected_tag)
{
	int ret;
	int actual = 0;
	unsigned int pipe;

	memset(csw, 0, sizeof(*csw));  //structure init to get the struct csw

	pipe = usb_rcvbulkpipe(dev->udev, dev->ep_in);  //get the IN endpoints info

	ret = usb_bulk_msg(dev->udev, pipe, csw, sizeof(*csw), &actual, 5000);  //timeout 5s -> jiffies

	if (ret) //check for the return value of usb_bulk_msg
	{
		printk(KERN_ERR "my_usb_driver: recv_csw failed: %d\n", ret);
		//check for stalls(error code: -32)
		if (ret == -EPIPE) {
			printk(KERN_ERR "my_usb_driver: STALL on CSW (bulk IN)\n");
			clear_bulk_stalls(dev);
		}
		return ret;
	}

	//actual -> contains the actual length transferred in bytes
	if (actual != sizeof(*csw))  //if the actual length transferred is != 13 bytes (size of csw) then error
	{
		printk(KERN_ERR "my_usb_driver: recv_csw short xfer: %d\n", actual);
		return -EIO;
	}

	if (le32_to_cpu(csw->Signature) != CSW_SIGNATURE)  //to check if the received csw is not the expected signature
	{
		printk(KERN_ERR "my_usb_driver: bad CSW signature 0x%08x\n", le32_to_cpu(csw->Signature));
		return -EIO;
	}

	if (csw->Tag != expected_tag)  //to check if the tag is not correct of csw and cbw
	{
		printk(KERN_ERR "my_usb_driver: CSW tag mismatch (%u != %u)\n", csw->Tag, expected_tag);
		return -EIO;
	}

	//check for all the types of the status errors coming 
	//00h -> status OK
	//01h -> Command failed
	//02h -> phase error
	//04h and 03h -> reserved
	if (csw->Status == CSW_BULK_STATUS_FAIL) {
		printk(KERN_ERR "my_usb_driver: CSW FAIL (status=1) — REQUEST SENSE\n");
		return -EIO;   /* caller can do_request_sense(dev) */
	}

	if (csw->Status == CSW_BULK_STATUS_PHASE_ERR) {
		printk(KERN_ERR "my_usb_driver: CSW PHASE ERROR (status=2) — clear stalls\n");
		clear_bulk_stalls(dev);
		return -EIO;
	}

	if (csw->Status != CSW_BULK_STATUS_OK) {
		printk(KERN_ERR "my_usb_driver: CSW status = %u\n", csw->Status);
		return -EIO;
	}

	return 0;
}
