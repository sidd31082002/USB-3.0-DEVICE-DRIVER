/* BBB CBW/CSW transport — declarations */
#ifndef BBB_TRANSPORT_H
#define BBB_TRANSPORT_H

#include "my_usb_dev.h"

// CBW structure definition - 31 bytes 
struct bulk_cb_wrapper {
	__le32 Signature;
	__u32 Tag;
	__le32 DataTransferLength;
	__u8 Flags;
	__u8 Lun;     //Logical Unit
	__u8 Length;  //CDB length(eg : 6 for SCSI)
	__u8 CDB[16];  //the actual SCSI command bytes
} __attribute__((packed));

#define CBW_SIGNATURE 0x43425355
#define CBW_BULK_FLAG_IN 0x80
#define CBW_BULK_FLAG_OUT 0x00

// BBB CSW — 13 bytes 
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

int clear_bulk_stalls(struct my_usb_dev *dev);
int send_cbw(struct my_usb_dev *dev, struct bulk_cb_wrapper *cbw);
int recv_csw(struct my_usb_dev *dev, struct bulk_cs_wrapper *csw, u32 expected_tag);

#endif /* BBB_TRANSPORT_H */
