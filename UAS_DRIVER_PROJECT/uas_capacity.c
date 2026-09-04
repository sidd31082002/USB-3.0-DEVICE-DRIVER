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

//fill a command IU for READ capacity command
static void fill_command_for_READ_CAPACITY(struct command_iu *cmd_iu , u16 tag)
{
    memset(cmd_iu , 0 , sizeof(*cmd_iu));

    cmd_iu -> iu_id = IU_ID_COMMAND;
    cmd_iu -> tag = cpu_to_be16(tag);  
    cmd_iu -> prio_attr = UAS_SIMPLE_TAG;
    cmd_iu -> len = 0;
    cmd_iu -> cdb[0] = 0x25;
    //rest cdb bytes is kept 0 itself    

}


int do_read_capacity_command(struct my_uas_dev *dev)
{
    struct command_iu *cmd_iu;
    struct sense_iu *sense_iu;
    struct completion done_status;
    struct completion done_data;
    struct urb *status_urb;
    struct urb *data_in_urb;
    unsigned int data_in_pipe;
    unsigned int cmd_pipe;
    unsigned int status_pipe;
    int actual = 0;
    u32 last_lba, block_len;
    u64 total;
    int ret;
    u16 tag;
    u8 *buffer;

    cmd_iu = kzalloc(sizeof(*cmd_iu) , GFP_KERNEL);
    sense_iu = kzalloc(sizeof(*sense_iu) , GFP_KERNEL);

    buffer = kzalloc(8 , GFP_KERNEL);
    if(!cmd_iu || !sense_iu || !buffer)
    {
        ret = -ENOMEM;
        goto out;
    }
    
    tag = ++dev -> tag;
    if(tag == 0)
    {
        tag = ++dev -> tag;
    }

    if(tag > dev -> num_streams)
    {
        tag = 1;
    }

    //reset the wait flag
    init_completion(&done_status);
    init_completion(&done_data);

    fill_command_for_READ_CAPACITY(cmd_iu , tag);

    //ALLOC FILL and SUBMIT Status_URB
    status_urb = usb_alloc_urb(0 , GFP_KERNEL);
    if(!status_urb)
    {
        ret = -ENOMEM;
        goto out;
    }

    status_pipe = usb_rcvbulkpipe(dev -> udev , dev -> ep_status);
    usb_fill_bulk_urb(status_urb, dev->udev, status_pipe, sense_iu, sizeof(*sense_iu), status_done_complete, &done_status);   
    status_urb -> stream_id = tag;
    ret = usb_submit_urb(status_urb , GFP_KERNEL);
    if(ret)
    {
        printk(KERN_ERR "my_uas_driver: status urb submit fail %d(from READ CAPACITY)\n", ret);
        goto free_urb;
    }


    //ALLOC FILL and SUBMIT DATA_IN_URB
    data_in_urb = usb_alloc_urb(0 , GFP_KERNEL);
    if(!data_in_urb)
    {
        ret = -ENOMEM;
        goto out;
    }

    data_in_pipe = usb_rcvbulkpipe(dev -> udev , dev -> ep_data_in);
    usb_fill_bulk_urb(data_in_urb , dev -> udev , data_in_pipe , buffer , 8 , data_done_complete , &done_data);
    data_in_urb -> stream_id = tag;
    ret = usb_submit_urb(data_in_urb , GFP_KERNEL);
    if(ret)
    {
        usb_kill_urb(status_urb);
        printk(KERN_ERR "my_uas_driver: data urb submit fail %d(from READ CAPACITY)\n", ret);
        goto free_urb;
    }
    

    cmd_pipe = usb_sndbulkpipe(dev -> udev , dev -> ep_cmd);
    ret = usb_bulk_msg(dev -> udev , cmd_pipe , cmd_iu , sizeof(*cmd_iu) , &actual , 5000);
    //                (dev->udev , pipe_in/pipe_out , cbw/csw , len , actual length send/recv , timeout ms)
    if (ret) 
    {
        printk(KERN_ERR "my_uas_driver: cmd_iu send fail %d\n(from READ CAPACITY)", ret);
        usb_kill_urb(status_urb);
        usb_kill_urb(data_in_urb);
        goto free_urb;
    }


    if (!wait_for_completion_timeout(&done_data, msecs_to_jiffies(5000))) 
    {
        printk(KERN_ERR "my_uas_driver: READ CAPACITY DATA IN timeout\n");
        usb_kill_urb(status_urb);
        usb_kill_urb(data_in_urb);
        ret = -ETIMEDOUT;
        goto free_urb;
    }

    //wait for status
    if(!wait_for_completion_timeout(&done_status , msecs_to_jiffies(5000)))
    {
        printk(KERN_ERR "my_uas_driver: READ CAPACITY status timed out\n");
        usb_kill_urb(status_urb);
        usb_kill_urb(data_in_urb);
        ret = -ETIMEDOUT;
        goto free_urb;
    }

    if (data_in_urb->status) 
    {
        ret = data_in_urb->status;
        goto free_urb;
    }
    if (status_urb->status) 
    {
        ret = status_urb->status;
        goto free_urb;
    }

    last_lba = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
    block_len = (buffer[4] << 24) | (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
    total = ((u64)last_lba + 1) * (u64)block_len;
    printk(KERN_INFO "my_uas_driver: READ CAPACITY last_lba=%u block=%u total=%llu \n", last_lba, block_len, total);

    //store the read values inside the my_uas_dev pvt structure
    dev->last_lba = last_lba;
    dev->block_len = block_len;
    dev->capacity_sectors = (u64)last_lba + 1;

    
    //check the status 
    if (sense_iu->iu_id != IU_ID_STATUS)
    {
        ret = -EIO;
    }
    else if (be16_to_cpu(sense_iu->tag) != tag)
    {
        ret = -EIO;
    }
    else if (sense_iu->status != 0)
    {
        ret = -EIO;
    }
    else
    {
        ret = 0;
    }

free_urb:
    if (status_urb)
        usb_free_urb(status_urb);
    if (data_in_urb)
        usb_free_urb(data_in_urb);
out:
    kfree(buffer);
    kfree(sense_iu);
    kfree(cmd_iu);
    return ret;

}




