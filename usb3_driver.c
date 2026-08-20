#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>  //__init __exit macros
#include<linux/uaccess.h>  //for copy_to_user / copy_from_user
#include<linux/fs.h> //for file operations structure
#include <linux/slab.h> // Required for kmalloc()
#include<linux/usb.h> //required for usb structures and macros
#include<linux/dma-mapping.h>
#include <linux/delay.h>   //to provide delay 


#define VENDOR_ID   0x8564
#define PRODUCT_ID  0x4000
// #define BLOCK_SIZE  512



static const char *my_speed_str(enum usb_device_speed speed)
{
	switch (speed) 
    {
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
    {USB_DEVICE(VENDOR_ID , PRODUCT_ID) },
    {USB_INTERFACE_INFO(0x08 , 0x06 , 0x50)} , //using this feild will help us create and use the interface details of usb devices
    {}
};

/*IMP IMP
   USB drivers use ID table to support hotplugging. Export this with MODULE_DEVICE_TABLE(usb,...). 
   This must be set or your driver’s probe function will never get called.
IMP IMP*/

//we need to make the user space know about this usb device table 
MODULE_DEVICE_TABLE(usb , my_usb_table);

//private usb device structure to hold necessary information we needed later
//bcz , probe function's local arguments die after probe returns
struct my_usb_dev {
	struct usb_device	*udev;   /* whole device; needed for pipes/URBs */
	struct usb_interface	*intf;   /* the interface we claimed */

	/* Bulk pipes — same role as us->recv_bulk_pipe / send_bulk_pipe */
	u8	ep_in;		//0x81 — EPIN address */
	u8	ep_out;		//0x02 - EPOUT address*/
	u16	maxpkt_in;
	u16	maxpkt_out;

    u32 tag; //increments for every CBW , CSW must watch the tag is correct or not 
	/* Later (don’t add yet unless you want placeholders):
	 * struct urb *urb;
	 * void *bulk_buf;
	 * struct mutex lock;
	 */
};



//CBW structure definition - 31 bytes
struct bulk_cb_wrapper{
    __le32 Signature;
    __u32 Tag;
    __le32 DataTransferLength;
    __u8 Flags;
    __u8 Lun;     //Logical Unit
    __u8 Length;  //CDB length(eg : 6 for SCSI)
    __u8 CDB[16];  //the actual SCSI command bytes
}__attribute__((packed));

#define CBW_SIGNATURE 0x43425355
#define CBW_BULK_FLAG_IN 0x80
#define CBW_BULK_FLAG_OUT 0x00


//BBB CSW — 13 bytes 
struct bulk_cs_wrapper {
	__le32	Signature;		// 'USBS' = 0x53425355 
	__u32	Tag;
	__le32	Residue;
	__u8	Status;			// 0 OK, 1 fail, 2 phase error 
} __attribute__((packed));

#define CSW_SIGNATURE 0x53425355 
#define CSW_BULK_STATUS_OK 0
#define CSW_BULK_STATUS_FAIL 1
#define CSW_BULK_STATUS_PHASE_ERR 2


static int send_cbw(struct my_usb_dev *dev, struct bulk_cb_wrapper *cbw)
{
    int ret;
	int actual = 0;
	unsigned int pipe;

	pipe = usb_sndbulkpipe(dev->udev, dev->ep_out); //endpoint OUT 

	ret = usb_bulk_msg(dev->udev , pipe , cbw , sizeof(*cbw) , &actual , 5000);	//timeout jiffies 5s

    //if usb_bulk_msg failed
	if (ret) 
    {
		printk(KERN_ERR "my_usb_driver: send_cbw failed: %d\n", ret);
		return ret;
	}

    //
	if (actual != sizeof(*cbw)) 
    {
		printk(KERN_ERR "my_usb_driver: send_cbw short xfer: %d\n",actual);
		return -EIO;
	}
	return 0;
}



static int recv_csw(struct my_usb_dev *dev, struct bulk_cs_wrapper *csw, u32 expected_tag)
{
	int ret;
	int actual = 0;
	unsigned int pipe;

	memset(csw, 0, sizeof(*csw));  //structure init to get the struct csw

	pipe = usb_rcvbulkpipe(dev->udev, dev->ep_in);  //get the IN endpoints info

	ret = usb_bulk_msg(dev->udev, pipe , csw , sizeof(*csw) ,  &actual , 5000);  //timeout 5s -> jiffies

	if (ret)  //check for return value of usb_bulk_msg
    {
		printk(KERN_ERR "my_usb_driver: recv_csw failed: %d\n", ret);
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

	if (csw->Status != CSW_BULK_STATUS_OK) 
    {
		printk(KERN_ERR "my_usb_driver: CSW status = %u\n (FAIL or Error)", csw->Status);
		return -EIO;
	}

	return 0;
}


static int do_scsi_inquiry(struct my_usb_dev *dev)
{
    //create cbw(populate cbw)
    struct bulk_cb_wrapper *cbw;
    struct bulk_cs_wrapper *csw;
    unsigned int pipe;
    int ret;
    int actual = 0;
    u8 *buffer;
    u32 tag;


    cbw = kzalloc(sizeof(*cbw) , GFP_KERNEL);
    csw = kzalloc(sizeof(*csw) , GFP_KERNEL);

    if(!cbw)
    {
        ret =  -ENOMEM;
        goto out;
    }
    if(!csw)
    {
        ret = -ENOMEM;
        goto out;
    }

    buffer = kzalloc(36 , GFP_KERNEL);  //bcz we are going to read 36 bytes of data from the device during inquiry command
    if(!buffer)
    {
        return -ENOMEM;
    }

    memset(cbw , 0 , sizeof(*cbw));

    //populating the cbw data to send first using send_cbw
    cbw -> Signature = cpu_to_le32(CBW_SIGNATURE);
    tag = ++(dev -> tag);
    cbw -> Tag = tag;
    cbw -> DataTransferLength = cpu_to_le32(36);  //data size
    cbw -> Flags = CBW_BULK_FLAG_IN;
    cbw -> Lun = 0;
	cbw -> Length = 6;          // Command Data Block(CDB) length in bytes
	cbw -> CDB[0] = 0x12;       // INQUIRY opcode
	cbw -> CDB[4] = 36;         // allocation length

    //sending the CBW
    ret = send_cbw(dev , cbw);
    if(ret)
    {                                
        //goto statement
        goto out;
    }

    pipe = usb_rcvbulkpipe(dev -> udev , dev -> ep_in);

    ret = usb_bulk_msg(dev -> udev , pipe , buffer , 36 , &actual ,  5000);
    if(ret)
    {
        printk(KERN_INFO "my_usb_driver : Inquiry data receiving failed %d\n",ret);
        goto out;
    }

    ret = recv_csw(dev , csw , tag);
    if(ret)
    {
        printk(KERN_INFO "my_usb_driver : recieve csw failed\n");
        goto out;
    }

    printk(KERN_INFO "my_usb_driver: INQUIRY vendorID=%.8s productID=%.16s\n", (char *)&buffer[8], (char *)&buffer[16]);

    return 0;

out:
    kfree(csw);
    kfree(cbw);
    kfree(buffer);   //freeing the buffer allocated to store the data
    return ret;       //return the error value

}


static int do_test_unit_ready(struct my_usb_dev *dev)
{
    struct bulk_cb_wrapper *cbw;
    struct bulk_cs_wrapper *csw;

    int ret = 0;
    //unsigned int actual = 0;
    // unsigned int pipe;
    u32 tag;

    cbw = kzalloc(sizeof(*cbw) , GFP_KERNEL);
    csw = kzalloc(sizeof(*csw) , GFP_KERNEL);
    if(!cbw || !csw)
    {
        ret =  -ENOMEM;
        goto out;
    }

    memset(cbw , 0 , sizeof(*cbw));

    cbw -> Signature = cpu_to_le32(CBW_SIGNATURE);
    tag = ++(dev -> tag);
    cbw -> Tag = tag;
    cbw -> DataTransferLength = cpu_to_le32(0);  //data size
    cbw -> Flags = CBW_BULK_FLAG_OUT;
    cbw -> Lun = 0;
	cbw -> Length = 6;          // Command Data Block(CDB) length in bytes
	cbw -> CDB[0] = 0x00;       // TUR(Test Unit Ready) opcode
    

    ret = send_cbw(dev , cbw);
    if(ret)
    {
        goto out;
    }

    ret = recv_csw(dev, csw, tag);
	if (ret) 
    { 
        printk(KERN_WARNING "my_usb_driver: TEST UNIT READY failed (no media or not ready?): %d\n", ret);
		goto out;
	}

	printk(KERN_INFO "my_usb_driver: TEST UNIT READY — media ready\n");

    ret = 0;
    goto out;

out:
	kfree(csw);
	kfree(cbw);
	return ret;


}


static int do_read_capacity(struct my_usb_dev *dev)
{
    struct bulk_cb_wrapper *cbw;
    struct bulk_cs_wrapper *csw;
    int ret = 0;
    unsigned int pipe;
    u8 *buffer;
    u32 tag;
	u32 last_lba, block_len;
	u64 total_bytes;
    int actual = 0;

    cbw = kzalloc(sizeof(*cbw) , GFP_KERNEL);
    csw = kzalloc(sizeof(*csw) , GFP_KERNEL);
    if(!csw || !cbw)
    {
        ret = -ENOMEM;
        goto out;
    }

    buffer = kzalloc(8 , GFP_KERNEL);   //READ CAPACITY -> takes in 8 bytes of data usually
    if(!buffer)
    {
        ret = -ENOMEM;
        goto out;
    }

    memset(cbw , 0 , sizeof(*cbw));

    cbw -> Signature = cpu_to_le32(CBW_SIGNATURE);
    tag = ++(dev -> tag);
    cbw -> Tag = tag;
    cbw -> DataTransferLength = cpu_to_le32(8);  //data size
    cbw -> Flags = CBW_BULK_FLAG_IN;
    cbw -> Lun = 0;
	cbw -> Length = 10;          // Command Data Block(CDB) length in bytes  => READ CAPACITY(10) CDB is 10 bytes
	cbw -> CDB[0] = 0x25;       // READ CAPACITY opcode => READ CAPACITY(10)
    
    ret = send_cbw(dev , cbw);
    if(ret)
    {
        goto out;
    }

    pipe = usb_rcvbulkpipe(dev -> udev , dev -> ep_in);

    ret = usb_bulk_msg(dev -> udev , pipe , buffer , 8 , &actual , 5000);
    if(ret)
    {
        printk(KERN_INFO "my_usb_driver : Read Capacity data receiving failed %d\n",ret);
        goto out;
    }

    ret = recv_csw(dev , csw , tag);
    if(ret)
    {
        printk(KERN_ERR "my_usb_driver: READ CAPACITY CSW failed: %d\n", ret);
        goto out;
    }

    //we got the capacity data in 8 bytes ...which is in big endian
    //result is in big endian format
    last_lba = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
	block_len = (buffer[4] << 24) | (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
	total_bytes = ((u64)last_lba + 1) * (u64)block_len;

    printk(KERN_INFO "my_usb_driver: READ CAPACITY last_lba=%u block_len=%u total=%llu bytes (%llu MiB)\n", last_lba, block_len, total_bytes, total_bytes >> 20);

    ret = 0;

out : 
    kfree(csw);
    kfree(cbw);
    kfree(buffer);
    return ret;    
}

static int do_read10(struct my_usb_dev *dev , u32 lba , u16 nBlocks)
{
    struct bulk_cb_wrapper *cbw;
    struct bulk_cs_wrapper *csw;
    int ret = 0;
    int actual = 0;
    unsigned int pipe;
    u8 *buffer;
    u32 tag;
    u32 len;

    len = nBlocks * 512;

    cbw = kzalloc(sizeof(*cbw) , GFP_KERNEL);
    csw = kzalloc(sizeof(*csw) , GFP_KERNEL);
    if(!csw || !cbw)
    {
        ret = -ENOMEM;
        goto out;
    }

    buffer = kzalloc(len , GFP_KERNEL);   //READ CAPACITY -> takes in 8 bytes of data usually
    if(!buffer)
    {
        ret = -ENOMEM;
        goto out;
    }

    memset(cbw , 0 , sizeof(*cbw));

    cbw -> Signature = cpu_to_le32(CBW_SIGNATURE);
    tag = ++(dev -> tag);
    cbw -> Tag = tag;
    cbw -> DataTransferLength = cpu_to_le32(len);  //data size
    cbw -> Flags = CBW_BULK_FLAG_IN;
    cbw -> Lun = 0;
	cbw -> Length = 10;          // Command Data Block(CDB) length in bytes  => READ10 CDB is 10 bytes => tihs is defined by SCSI

    /*This is the CDB structure for example for READ10 SCSI command*/
    // If lba = 0x00001234 and nblocks = 1:
    // CDB: 28 00 00 00 12 34 00 00 01 00
    //            |-- LBA --|    |-len-|

	cbw -> CDB[0] = 0x28;       // READ10 opcode => READ10

    cbw -> CDB[2] = (lba >> 24) & 0xff;  //MSB
    cbw -> CDB[3] = (lba >> 16) & 0xff;
    cbw -> CDB[4] = (lba >> 8) & 0xff;
    cbw -> CDB[5] = (lba ) & 0xff;       //LSB

    cbw -> CDB[7] = (nBlocks >> 8) & 0xff;
    cbw -> CDB[8] = (nBlocks ) & 0xff;


    ret = send_cbw(dev , cbw);
    if(ret)
    {
        goto out;
    }

    pipe = usb_rcvbulkpipe(dev -> udev , dev -> ep_in);

    ret = usb_bulk_msg(dev -> udev , pipe , buffer , len , &actual , 5000);
    if(ret)
    {
        printk(KERN_INFO "my_usb_driver : Read10 data receiving failed %d\n",ret);
        goto out;
    }

    ret = recv_csw(dev , csw , tag);
    if(ret)
    {
        printk(KERN_ERR "my_usb_driver: READ10 CSW failed: %d\n", ret);
        goto out;
    }

    //%*ph 16 => then buffer print 16 bytes from buffer as hex
    // * = “length comes from the next argument” (16).
    // ph = “pointer to hex bytes” → space-separated hex, e.g. fa b8 00 10 ....
    printk(KERN_INFO "my_usb_driver: READ10 LBA=%u got %d bytes, first16: %*ph\n", lba, actual, 16, buffer);


    //parsing the partitions that is starting from 446 byte of the first block
    for (int i = 0; i < 4; i++) 
    {
        u8 *e = &buffer[446 + i * 16];
        u8 type = e[4];
        u32 start, size;

        if (type == 0x00)
            continue;  // empty slot 

        start = e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24);  // change it to le  
        size  = e[12] | (e[13] << 8) | (e[14] << 16) | (e[15] << 24); //change it to le

        printk(KERN_INFO "my_usb_driver: part[%d] type=0x%02x boot=0x%02x start_lba=%u size_sectors=%u (%llu MiB)\n", i, type, e[0], start, size, ((u64)size * 512) >> 20);
    }

    if (nBlocks == 1 && len >= 512)
    {
        printk(KERN_INFO "my_usb_driver: MBR signature %02X %02X (expect 55 AA)\n", buffer[510], buffer[511]);

    }
	   
    ret = 0;
    goto out;
    
out : 
    kfree(csw);
    kfree(cbw);
    kfree(buffer);
    return ret;    
}

//if another driver is already installed and indicates that it is responsible for the device attached ..then 
//...your probe will not get called
//simply the probe function will not get called if another driver has taken claim of this particular device registerd with this device
// struct usb_interface *interface -> this structure already contains the parsed descriptor data of the device(interface descriptor)
static int my_usb_probe(struct usb_interface *interface, const struct usb_device_id *id)
{

    //creating a var for my usb device private structure
    struct my_usb_dev *dev;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if(!dev)
        return -ENOMEM;

    struct usb_device *udev;
    struct usb_endpoint_descriptor *endpoint_desc; //pointer to a structure that can store the info about the endpoint
    struct usb_host_interface *curr_interface_alt_setting; //pointer to store the curr alternate settings of the interface
    int ret;


    udev = interface_to_usbdev(interface);  //udev will contain the parsed descriptor data and info of the device

    if(!udev -> bos)
    {
        pr_info("bos desc not present!!\n");
    }
    else
    {
        pr_info("my_usb_driver : BOS desc is present!!\n");
    }


    //populate the private data structure(my_usb_dev)
    //step 1 -> increment the refernce count of the usb device we claimed
    dev -> udev = usb_get_dev(udev);
    
    //store the current interface of the usb device in our private usb device data structure
    dev -> intf = interface;

    //usb_set_intfdata -> associate driver specific data with an interface
    usb_set_intfdata(interface, dev);


    // struct usb_host_interface *usb_altnum_to_altsetting(const struct usb_interface *intf, unsigned int altnum)
    // get the altsetting structure with a given alternate setting number.

    
    curr_interface_alt_setting = interface -> cur_altsetting;  //usb_host_interface contains : 
                                                                    //├── .desc  (interface descriptor)
                                                                    //└── .endpoint[] arrays (endpoints)

    /*Printing some data fromt the current interface*/                                                                

    printk(KERN_INFO "my_usb_driver : from my_usb_driver_probe() : device connected\n");

    printk(KERN_INFO "my_usb_driver: VID:PID = %04x:%04x\n",le16_to_cpu(udev->descriptor.idVendor), le16_to_cpu(udev->descriptor.idProduct));
                                                            //converts to host machine byte ordering() -> le16_to_cpu()



    /*Printing speed and LPM logs*/
    printk(KERN_INFO "my_usb_driver: speed=%d (%s) bcdUSB=0x%04x\n", udev->speed, my_speed_str(udev->speed), le16_to_cpu(udev->descriptor.bcdUSB)); //speed for the device enumerated 

    printk(KERN_INFO "my_usb_driver: LPM capable=%u  U1_enabled=%u  U2_enabled=%u\n", udev->lpm_capable, udev->usb3_lpm_u1_enabled, udev->usb3_lpm_u2_enabled);



    //printing the device descriptor data
    printk(KERN_INFO "my_usb_driver : dev bLength=%u bDescriptorType=%u bcdUSB=0x%04x\n" , udev -> descriptor.bLength , udev -> descriptor.bDescriptorType , udev -> descriptor.bcdUSB);
    
    if(udev -> descriptor.bLength != 18 || udev->descriptor.bDescriptorType != USB_DT_DEVICE)
    {
        goto err_free;
    }
    
    //printing the class , subclass , protocol for this interface , number of endpoints
    printk(KERN_INFO "my_usb_driver: class=%02x\n", curr_interface_alt_setting->desc.bInterfaceClass);

    printk(KERN_INFO "my_usb_driver: subclass=%02x\n", curr_interface_alt_setting->desc.bInterfaceSubClass);

    printk(KERN_INFO "my_usb_driver: protocol=%02x\n", curr_interface_alt_setting->desc.bInterfaceProtocol);

    printk(KERN_INFO "my_usb_driver: endpoints=%u\n", curr_interface_alt_setting->desc.bNumEndpoints);


    //validate class , subclass and protocol
    if(curr_interface_alt_setting->desc.bInterfaceClass != 0x08 || curr_interface_alt_setting->desc.bInterfaceSubClass != 0x06 || curr_interface_alt_setting->desc.bInterfaceProtocol != 0x50)
    {
        goto err_free;
    }

    //USB_SPEED_HIGH -> 480mbps
    //USB_SPEED_FULL -> 12mbps
    //USB_SPEED_LOW -> 1.5mbps
    if (udev->speed < USB_SPEED_SUPER)
    {
        printk(KERN_WARNING  "my_usb_driver: not SuperSpeed (speed=%d) — SS companions may be unused\n", udev->speed);
    }    
    else
    {
        printk(KERN_INFO "my_usb_driver: SuperSpeed link — parsing SS companions\n");
    }

    //now we are going to get the address of each endpoint associated with this curr altsetting interface 
    // USB data does not go through the interface abstractly — it goes through endpoints.
    //endpoints are in curr_interface_alt_setting->endpoint[i] and the descriptor is  in ..->endpoint[i].desc
    for(int i = 0; i < curr_interface_alt_setting -> desc.bNumEndpoints ; i++)
    {
        endpoint_desc = &curr_interface_alt_setting->endpoint[i].desc;

        printk(KERN_INFO "my_usb_driver: ep[%d] address=0x%02x\n", i , endpoint_desc->bEndpointAddress);

        /* SuperSpeed Endpoint Companion Descriptor (USB 3.x) */
        if (udev->speed >= USB_SPEED_SUPER) 
        {
            struct usb_ss_ep_comp_descriptor *ss_comp = &curr_interface_alt_setting->endpoint[i].ss_ep_comp;

            printk(KERN_INFO "my_usb_driver: ep[%d] SS companion: bMaxBurst=%u bmAttributes=0x%02x wBytesPerInterval=%u\n", i, ss_comp->bMaxBurst, ss_comp->bmAttributes, le16_to_cpu(ss_comp->wBytesPerInterval));

            // streams: lower 5 bits of bmAttributes (if non-zero)
            if (ss_comp->bmAttributes & 0x1f)
            {
                printk(KERN_INFO "my_usb_driver: ep[%d] supports %u streams (MaxStreams field)\n", i, 1 << (ss_comp->bmAttributes & 0x1f));
            }
                
        }


        //store the type of this particular endpoint in the private data structure(ep_in or ep_out)
        if(usb_endpoint_is_bulk_in(endpoint_desc)) //api to check whether the endpoint is IN
        {
            dev -> ep_in = endpoint_desc->bEndpointAddress;
            dev -> maxpkt_in = usb_endpoint_maxp(endpoint_desc);
        }
        else if(usb_endpoint_is_bulk_out(endpoint_desc))  //api to check whether the endpoint is OUT
        {
            dev -> ep_out = endpoint_desc->bEndpointAddress;
            dev -> maxpkt_out = usb_endpoint_maxp(endpoint_desc);
        }

        printk(KERN_INFO "my_usb_driver: ep[%d] attributes=0x%02x\n", i , endpoint_desc->bmAttributes);

        printk(KERN_INFO "my_usb_driver: ep[%d] maxpkt=%d\n", i , usb_endpoint_maxp(endpoint_desc));

        
    }
    if(!dev -> ep_in || !dev -> ep_out)
    {
        goto err_free;
    }

    //do the inquiry scsi command
    //AT first kernel crash happened with inquiry failed with ret value -11
    ret = do_scsi_inquiry(dev);
    if(ret)
    {
        printk(KERN_ERR "my_usb_driver: INQUIRY failed: %d\n", ret);
        // goto err_free;
    }

    //do the test unit data scsi command
    // ret = do_test_unit_ready(dev);
    for (int i = 0; i < 5; i++) 
    {
		ret = do_test_unit_ready(dev);
		if (ret == 0)
        {
            ret = do_read_capacity(dev);
            if(ret)
            {
                printk(KERN_ERR "READ CAPACITY FAILED TO READ DATA with error code %d", ret);
                goto err_free;
            }
            break;
        }
			
		msleep(300);
	}
    if(ret)
    {
        printk(KERN_ERR "my_usb_driver: Test Unit ready FAILED with error code(Is SD Card inserted? ) %d", ret);
    }


    //do the READ10 SCSI command
    ret = do_read10(dev , 0 , 1);
    if(ret)
    {
        printk(KERN_ERR "my_usb_driver: READ10 failed: %d\n", ret);
    }
     

    return 0;

err_free : 
    usb_set_intfdata(interface , NULL);
    usb_put_dev(dev -> udev);  //dec the ref count for the usb device structure
    kfree(dev);
    return -ENODEV;
   
}


//whenever the pluggered device is removed
static void my_usb_disconnect(struct usb_interface *intf)
{
    struct my_usb_dev *dev = usb_get_intfdata(intf); //get the device associated data and store it in the private structure

    usb_set_intfdata(intf , NULL);

    if(dev != NULL)
    {
        usb_put_dev(dev->udev); //decrease the ref count of the particular device
        kfree(dev); //free 
    }
    printk(KERN_INFO "device removed\n");
}



//USB DRIVER structure initialization
//declaring the usb driver => basic definition of the usb driver
//this is like the device we are creating for the particular driver with the callback functions
static struct usb_driver my_usb_driver ={
    .name = "my_usb_driver" ,  //responsible for naming the driver as a whole
    .id_table = my_usb_table,  //it is used to match with this driver with any device 
    .probe = my_usb_probe,
    .disconnect = my_usb_disconnect,
};



static int __init usb_init(void)
{
    pr_info("my_usb_driver : Module loaded successfully.\n");
    int result = usb_register(&my_usb_driver);
    if(result)
    {
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