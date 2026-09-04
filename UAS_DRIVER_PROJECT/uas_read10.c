#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/usb/uas.h>
#include <linux/string.h>
#include <linux/completion.h>
#include <linux/mutex.h>

#include "my_uas_dev.h"
#include "uas_urb.h"
#include "uas_scsi.h"


//fill a CMD IU for read10
static void fill_command_for_READ10(struct command_iu *cmd_iu, u16 tag, u32 lba, u16 nblocks)
{
    memset(cmd_iu, 0, sizeof(*cmd_iu));

    cmd_iu->iu_id = IU_ID_COMMAND;
    cmd_iu->tag = cpu_to_be16(tag);
    cmd_iu->prio_attr = UAS_SIMPLE_TAG;
    cmd_iu->len = 0;

    cmd_iu->cdb[0] = 0x28;   // READ(10) opcode

    cmd_iu->cdb[2] = (lba >> 24) & 0xff;
    cmd_iu->cdb[3] = (lba >> 16) & 0xff;
    cmd_iu->cdb[4] = (lba >> 8) & 0xff;
    cmd_iu->cdb[5] = lba & 0xff;

    cmd_iu->cdb[7] = (nblocks >> 8) & 0xff;
    cmd_iu->cdb[8] = nblocks & 0xff;
}


int do_read10(struct my_uas_dev *dev, u32 lba, u16 nblocks, u8 *buf)
{
    struct command_iu *cmd_iu;
    struct sense_iu *sense_iu;
    struct completion done_status;
    struct completion done_data;
    struct urb *status_urb = NULL;
    struct urb *data_in_urb = NULL;
    unsigned int data_in_pipe;
    unsigned int cmd_pipe;
    unsigned int status_pipe;
    int actual = 0;
    int ret;
    u16 tag;
    u32 len;

    if (!buf || nblocks == 0)
        return -EINVAL;

    if (!dev->block_len)
        return -EINVAL;

    //finding the total length (bytes) to be read
    //len = number of blocks * size of each block (from read capacity)    
    len = nblocks * dev->block_len;

    //locking from here
    mutex_lock(&dev->lock);

    cmd_iu = kzalloc(sizeof(*cmd_iu), GFP_KERNEL);
    sense_iu = kzalloc(sizeof(*sense_iu), GFP_KERNEL);
    if (!cmd_iu || !sense_iu) 
    {
        ret = -ENOMEM;
        goto out;
    }

    tag = ++dev->tag;
    if (tag == 0)
    {
        tag = ++dev->tag;
    }    
    if (tag > dev->num_streams)
    {
        tag = 1;
    }    

    init_completion(&done_status);
    init_completion(&done_data);

    //fill the command iu for the read10 command
    fill_command_for_READ10(cmd_iu, tag, lba, nblocks);

    // status URB
    status_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!status_urb) 
    {
        ret = -ENOMEM;
        goto out;
    }

    status_pipe = usb_rcvbulkpipe(dev->udev, dev->ep_status);
    usb_fill_bulk_urb(status_urb, dev->udev, status_pipe, sense_iu, sizeof(*sense_iu), status_done_complete, &done_status);
    status_urb->stream_id = tag;

    ret = usb_submit_urb(status_urb, GFP_KERNEL);
    if (ret) 
    {
        printk(KERN_ERR "my_uas_driver: status urb submit fail %d (READ10)\n", ret);
        goto free_urb;
    }

    // data URB — buf, length = len
    data_in_urb = usb_alloc_urb(0, GFP_KERNEL);
    if (!data_in_urb) {
        ret = -ENOMEM;
        usb_kill_urb(status_urb);
        goto free_urb;
    }

    data_in_pipe = usb_rcvbulkpipe(dev->udev, dev->ep_data_in);
    usb_fill_bulk_urb(data_in_urb, dev->udev, data_in_pipe, buf, len, data_done_complete, &done_data);
    data_in_urb->stream_id = tag;

    ret = usb_submit_urb(data_in_urb, GFP_KERNEL);
    if (ret) {
        printk(KERN_ERR "my_uas_driver: data urb submit fail %d (READ10)\n", ret);
        usb_kill_urb(status_urb);
        goto free_urb;
    }

    cmd_pipe = usb_sndbulkpipe(dev->udev, dev->ep_cmd);
    ret = usb_bulk_msg(dev->udev, cmd_pipe, cmd_iu, sizeof(*cmd_iu),
                       &actual, 5000);
    if (ret) {
        printk(KERN_ERR "my_uas_driver: cmd_iu send fail %d (READ10)\n", ret);
        usb_kill_urb(status_urb);
        usb_kill_urb(data_in_urb);
        goto free_urb;
    }

    if (!wait_for_completion_timeout(&done_data, msecs_to_jiffies(5000))) {
        printk(KERN_ERR "my_uas_driver: READ10 data timeout\n");
        usb_kill_urb(status_urb);
        usb_kill_urb(data_in_urb);
        ret = -ETIMEDOUT;
        goto free_urb;
    }

    if (!wait_for_completion_timeout(&done_status, msecs_to_jiffies(5000))) {
        printk(KERN_ERR "my_uas_driver: READ10 status timeout\n");
        usb_kill_urb(status_urb);
        usb_kill_urb(data_in_urb);
        ret = -ETIMEDOUT;
        goto free_urb;
    }

    if (data_in_urb->status) {
        ret = data_in_urb->status;
        goto free_urb;
    }
    if (status_urb->status) {
        ret = status_urb->status;
        goto free_urb;
    }

    if (sense_iu->iu_id != IU_ID_STATUS)
        ret = -EIO;
    else if (be16_to_cpu(sense_iu->tag) != tag)
        ret = -EIO;
    else if (sense_iu->status != 0)
        ret = -EIO;
    else
        ret = 0;

free_urb:
    if (status_urb)
        usb_free_urb(status_urb);
    if (data_in_urb)
        usb_free_urb(data_in_urb);
out:
    kfree(sense_iu);
    kfree(cmd_iu);
    mutex_unlock(&dev->lock);
    return ret;
}

