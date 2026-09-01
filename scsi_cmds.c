#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/string.h>

#include "bbb_transport.h"
#include "scsi_cmds.h"

int do_scsi_inquiry(struct my_usb_dev *dev)
{
	//create cbw(populate cbw)
	struct bulk_cb_wrapper *cbw;
	struct bulk_cs_wrapper *csw;
	unsigned int pipe;
	int ret;
	int actual = 0;
	u8 *buffer;
	u32 tag;

	cbw = kzalloc(sizeof(*cbw), GFP_KERNEL);
	csw = kzalloc(sizeof(*csw), GFP_KERNEL);

	if (!cbw) {
		ret = -ENOMEM;
		goto out;
	}
	if (!csw) {
		ret = -ENOMEM;
		goto out;
	}

	buffer = kzalloc(36, GFP_KERNEL);  //bcz we are going to read 36 bytes of data from the device during inquiry command
	if (!buffer) {
		return -ENOMEM;
	}

	memset(cbw, 0, sizeof(*cbw));

	//populating the cbw data to send first using send_cbw
	cbw->Signature = cpu_to_le32(CBW_SIGNATURE);
	tag = ++(dev->tag);
	cbw->Tag = tag;
	cbw->DataTransferLength = cpu_to_le32(36);  //data size
	cbw->Flags = CBW_BULK_FLAG_IN;
	cbw->Lun = 0;
	cbw->Length = 6;          // Command Data Block(CDB) length in bytes
	cbw->CDB[0] = 0x12;       // INQUIRY opcode
	cbw->CDB[4] = 36;         // allocation length

	//sending the CBW
	ret = send_cbw(dev, cbw);
	if (ret) {
		//goto statement
		goto out;
	}

	pipe = usb_rcvbulkpipe(dev->udev, dev->ep_in);

	ret = usb_bulk_msg(dev->udev, pipe, buffer, 36, &actual, 5000);
	if (ret) {
		printk(KERN_INFO "my_usb_driver : Inquiry data receiving failed %d\n", ret);
		//if stall -EPIPE(32)
		if (ret == -EPIPE) {
			printk(KERN_ERR "my_usb_driver: STALL on data phase\n");
			clear_bulk_stalls(dev);
		}
		goto out;
	}

	ret = recv_csw(dev, csw, tag);
	if (ret) {
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

int do_test_unit_ready(struct my_usb_dev *dev)
{
	struct bulk_cb_wrapper *cbw;
	struct bulk_cs_wrapper *csw;

	int ret = 0;
	//unsigned int actual = 0;
	// unsigned int pipe;
	u32 tag;

	cbw = kzalloc(sizeof(*cbw), GFP_KERNEL);
	csw = kzalloc(sizeof(*csw), GFP_KERNEL);
	if (!cbw || !csw) {
		ret = -ENOMEM;
		goto out;
	}

	memset(cbw, 0, sizeof(*cbw));

	cbw->Signature = cpu_to_le32(CBW_SIGNATURE);
	tag = ++(dev->tag);
	cbw->Tag = tag;
	cbw->DataTransferLength = cpu_to_le32(0);  //data size
	cbw->Flags = CBW_BULK_FLAG_OUT;
	cbw->Lun = 0;
	cbw->Length = 6;          // Command Data Block(CDB) length in bytes
	cbw->CDB[0] = 0x00;       // TUR(Test Unit Ready) opcode

	ret = send_cbw(dev, cbw);
	if (ret) {
		goto out;
	}

	ret = recv_csw(dev, csw, tag);
	if (ret) {
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

int do_read_capacity(struct my_usb_dev *dev)
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

	cbw = kzalloc(sizeof(*cbw), GFP_KERNEL);
	csw = kzalloc(sizeof(*csw), GFP_KERNEL);
	if (!csw || !cbw) {
		ret = -ENOMEM;
		goto out;
	}

	buffer = kzalloc(8, GFP_KERNEL);   //READ CAPACITY -> takes in 8 bytes of data usually
	if (!buffer) {
		ret = -ENOMEM;
		goto out;
	}

	memset(cbw, 0, sizeof(*cbw));

	cbw->Signature = cpu_to_le32(CBW_SIGNATURE);
	tag = ++(dev->tag);
	cbw->Tag = tag;
	cbw->DataTransferLength = cpu_to_le32(8);  //data size
	cbw->Flags = CBW_BULK_FLAG_IN;
	cbw->Lun = 0;
	cbw->Length = 10;          // Command Data Block(CDB) length in bytes  => READ CAPACITY(10) CDB is 10 bytes
	cbw->CDB[0] = 0x25;       // READ CAPACITY opcode => READ CAPACITY(10)

	ret = send_cbw(dev, cbw);
	if (ret) {
		goto out;
	}

	pipe = usb_rcvbulkpipe(dev->udev, dev->ep_in);

	ret = usb_bulk_msg(dev->udev, pipe, buffer, 8, &actual, 5000);
	if (ret) {
		printk(KERN_INFO "my_usb_driver : Inquiry data receiving failed %d\n", ret);
		//if stall -EPIPE(32)
		if (ret == -EPIPE) {
			printk(KERN_ERR "my_usb_driver: STALL on data phase\n");
			clear_bulk_stalls(dev);
		}
		goto out;
	}

	ret = recv_csw(dev, csw, tag);
	if (ret) {
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

out:
	kfree(csw);
	kfree(cbw);
	kfree(buffer);
	return ret;
}

int do_request_sense(struct my_usb_dev *dev)
{
	struct bulk_cb_wrapper *cbw;
	struct bulk_cs_wrapper *csw;
	int ret = 0;
	unsigned int pipe;
	u8 *buffer;
	u32 tag;
	int actual = 0;
	u8 sense_key, asc, ascq;

	cbw = kzalloc(sizeof(*cbw), GFP_KERNEL);
	csw = kzalloc(sizeof(*csw), GFP_KERNEL);
	if (!csw || !cbw) {
		ret = -ENOMEM;
		goto out;
	}

	buffer = kzalloc(18, GFP_KERNEL);   //REQUEST SENSE -> takes in 8 bytes of data usually
	if (!buffer) {
		ret = -ENOMEM;
		goto out;
	}

	memset(cbw, 0, sizeof(*cbw));

	cbw->Signature = cpu_to_le32(CBW_SIGNATURE);
	tag = ++(dev->tag);
	cbw->Tag = tag;
	cbw->DataTransferLength = cpu_to_le32(18);  //sense data size
	cbw->Flags = CBW_BULK_FLAG_IN;
	cbw->Lun = 0;
	cbw->Length = 6;
	cbw->CDB[0] = 0x03;	//REQUEST SENSE opcode
	cbw->CDB[4] = 18;

	ret = send_cbw(dev, cbw);
	if (ret) {
		goto out;
	}

	pipe = usb_rcvbulkpipe(dev->udev, dev->ep_in);
	ret = usb_bulk_msg(dev->udev, pipe, buffer, 18, &actual, 5000);
	if (ret) {
		printk(KERN_INFO "my_usb_driver : Inquiry data receiving failed %d\n", ret);
		//if stall -EPIPE(32)
		if (ret == -EPIPE) {
			printk(KERN_ERR "my_usb_driver: STALL on data phase\n");
			clear_bulk_stalls(dev);
		}
		goto out;
	}

	ret = recv_csw(dev, csw, tag);
	if (ret) {
		printk(KERN_INFO "my_usb_driver: REQUEST SENSE CSW failed: %d\n", ret);
		goto out;
	}

	sense_key = buffer[2] & 0x0f; //2nd byte lower nibble contains the sense key
	asc = buffer[12];  //Additional sense code
	ascq = buffer[13];

	//print the sense data
	printk(KERN_INFO "my_usb_driver: REQUEST SENSE got %d bytes: key=0x%x ASC=0x%02x ASCQ=0x%02x raw=%*ph\n", actual, sense_key, asc, ascq, 18, buffer);

	ret = 0;
out:
	kfree(buffer);
	kfree(csw);
	kfree(cbw);
	return ret;
}

int do_read10(struct my_usb_dev *dev, u32 lba, u16 nBlocks)
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

	cbw = kzalloc(sizeof(*cbw), GFP_KERNEL);
	csw = kzalloc(sizeof(*csw), GFP_KERNEL);
	if (!csw || !cbw) {
		ret = -ENOMEM;
		goto out;
	}

	buffer = kzalloc(len, GFP_KERNEL);   //READ CAPACITY -> takes in 8 bytes of data usually
	if (!buffer) {
		ret = -ENOMEM;
		goto out;
	}

	memset(cbw, 0, sizeof(*cbw));

	cbw->Signature = cpu_to_le32(CBW_SIGNATURE);
	tag = ++(dev->tag);
	cbw->Tag = tag;
	cbw->DataTransferLength = cpu_to_le32(len);  //data size
	cbw->Flags = CBW_BULK_FLAG_IN;
	cbw->Lun = 0;
	cbw->Length = 10;          // Command Data Block(CDB) length in bytes  => READ10 CDB is 10 bytes => tihs is defined by SCSI

	/*This is the CDB structure for example for READ10 SCSI command*/
	// If lba = 0x00001234 and nblocks = 1:
	// CDB: 28 00 00 00 12 34 00 00 01 00
	//            |-- LBA --|    |-len-|

	cbw->CDB[0] = 0x28;       // READ10 opcode => READ10

	cbw->CDB[2] = (lba >> 24) & 0xff;  //MSB
	cbw->CDB[3] = (lba >> 16) & 0xff;
	cbw->CDB[4] = (lba >> 8) & 0xff;
	cbw->CDB[5] = (lba) & 0xff;       //LSB

	cbw->CDB[7] = (nBlocks >> 8) & 0xff;
	cbw->CDB[8] = (nBlocks) & 0xff;

	ret = send_cbw(dev, cbw);
	if (ret) {
		goto out;
	}

	pipe = usb_rcvbulkpipe(dev->udev, dev->ep_in);

	ret = usb_bulk_msg(dev->udev, pipe, buffer, len, &actual, 5000);
	if (ret) {
		printk(KERN_INFO "my_usb_driver : Inquiry data receiving failed %d\n", ret);
		//if stall -EPIPE(32)
		if (ret == -EPIPE) {
			printk(KERN_ERR "my_usb_driver: STALL on data phase\n");
			clear_bulk_stalls(dev);
		}
		goto out;
	}

	ret = recv_csw(dev, csw, tag);
	if (ret) {
		printk(KERN_ERR "my_usb_driver: READ10 CSW failed: %d\n", ret);
		goto out;
	}

	//%*ph 16 => then buffer print 16 bytes from buffer as hex
	// * = “length comes from the next argument” (16).
	// ph = “pointer to hex bytes” → space-separated hex, e.g. fa b8 00 10 ....
	printk(KERN_INFO "my_usb_driver: READ10 LBA=%u got %d bytes, first16: %*ph\n", lba, actual, 16, buffer);

	//parsing the partitions that is starting from 446 byte of the first block
	for (int i = 0; i < 4; i++) {
		u8 *e = &buffer[446 + i * 16];
		u8 type = e[4];
		u32 start, size;

		if (type == 0x00)
			continue;  // empty slot 

		start = e[8] | (e[9] << 8) | (e[10] << 16) | (e[11] << 24);  // change it to le  
		size = e[12] | (e[13] << 8) | (e[14] << 16) | (e[15] << 24); //change it to le

		printk(KERN_INFO "my_usb_driver: part[%d] type=0x%02x boot=0x%02x start_lba=%u size_sectors=%u (%llu MiB)\n", i, type, e[0], start, size, ((u64)size * 512) >> 20);
	}

	if (nBlocks == 1 && len >= 512) {
		printk(KERN_INFO "my_usb_driver: MBR signature %02X %02X (expect 55 AA)\n", buffer[510], buffer[511]);
	}

	ret = 0;
	goto out;

out:
	kfree(csw);
	kfree(cbw);
	kfree(buffer);
	return ret;
}
