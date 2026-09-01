/* SCSI command helpers over BBB */
#ifndef SCSI_CMDS_H
#define SCSI_CMDS_H

#include "my_usb_dev.h"

int do_scsi_inquiry(struct my_usb_dev *dev);
int do_test_unit_ready(struct my_usb_dev *dev);
int do_read_capacity(struct my_usb_dev *dev);
int do_request_sense(struct my_usb_dev *dev);
int do_read10(struct my_usb_dev *dev, u32 lba, u16 nBlocks);

#endif /* SCSI_CMDS_H */
