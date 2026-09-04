/* SCSI / UAS command helpers */
#ifndef UAS_SCSI_H
#define UAS_SCSI_H

#include "my_uas_dev.h"

int do_read10(struct my_uas_dev *dev, u32 lba, u16 nblocks, u8 *buf);
int do_read_capacity_command(struct my_uas_dev *dev);
int do_scsi_inquiry(struct my_uas_dev *dev);
int do_tur_cmd(struct my_uas_dev *dev);

#endif /* UAS_SCSI_H */
