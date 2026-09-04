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

//fill a command IU for INQUIRY command
static void fill_command_for_INQUIRY(struct command_iu *cmd_iu , u16 tag)
{
    memset(cmd_iu , 0 , sizeof(*cmd_iu));

    cmd_iu -> iu_id = IU_ID_COMMAND;
    cmd_iu -> tag = cpu_to_be16(tag);
    cmd_iu -> prio_attr = UAS_SIMPLE_TAG;
    cmd_iu -> len = 0;
	// lun already 0 
	cmd_iu -> cdb[0] = 0x12;   // INQUIRY OPCODE
    cmd_iu -> cdb[4] = 36;
}



//INQUIRY command
int do_scsi_inquiry(struct my_uas_dev *dev)
{
    struct command_iu *cmd_iu;
    struct sense_iu *sense_iu;
    struct completion done_status;
    struct completion done_data;
    struct urb *status_urb = NULL;
    struct urb *data_in_urb = NULL;
    unsigned int status_pipe;
    unsigned int cmd_pipe;
    unsigned int data_in_pipe;
    u8 *buffer;
    int ret;
    int actual = 0;
    u16 tag;

    cmd_iu = kzalloc(sizeof(*cmd_iu) , GFP_KERNEL);
    sense_iu = kzalloc(sizeof(*sense_iu) , GFP_KERNEL);
    buffer = kzalloc(36 , GFP_KERNEL);
    if(!cmd_iu || !sense_iu || !buffer)
    {
        ret = -ENOMEM;
        goto out;
    }

    //store the value in the tag variable
    tag = ++dev -> tag;
    if (tag == 0)
    {
		tag = ++dev->tag;
    }

    //if tags are greater than 32 (max_streams)
    if (tag > dev->num_streams)
        tag = 1;  

    //done to reset the wait flag for struct completion
    init_completion(&done_data);
    init_completion(&done_status);

    //fill the command IU for the INQUIRY cmd ....only diff is opcode = 0x12 and cdb[4] = 36 
    fill_command_for_INQUIRY(cmd_iu , tag);

    //ALLOC , FILL AND SUBMIT STATUS URB
    status_urb = usb_alloc_urb(0 , GFP_KERNEL);
    if(!status_urb)
    {
        ret = -ENOMEM;
        goto out;
    }

    status_pipe = usb_rcvbulkpipe(dev -> udev , dev -> ep_status);
    //void usb_fill_bulk_urb(struct urb *urb, struct usb_device *dev, unsigned int pipe, void *transfer_buffer, int buffer_length, usb_complete_t complete_fn, void *context)
    usb_fill_bulk_urb(status_urb , dev -> udev , status_pipe , sense_iu , sizeof(*sense_iu) ,  status_done_complete , &done_status);
    //fill the stream id part of status urb
    status_urb -> stream_id = tag;

    //submit status urb
    ret = usb_submit_urb(status_urb , GFP_KERNEL);
    if (ret) 
    {
        printk(KERN_ERR "my_uas_driver: status urb submit fail %d(from INQUIRY)\n", ret);
        goto free_urb;
    }

    //ALLOC , FILL AND SUBMIT DATA URB
    data_in_urb = usb_alloc_urb(0 , GFP_KERNEL);
    if(!data_in_urb)
    {
        usb_kill_urb(status_urb); 
        ret = -ENOMEM;
        goto out;
    }

    data_in_pipe = usb_rcvbulkpipe(dev -> udev , dev -> ep_data_in);
    //fill data bulk urb
    //void usb_fill_bulk_urb(struct urb *urb, struct usb_device *dev, unsigned int pipe, void *transfer_buffer, int buffer_length, usb_complete_t complete_fn, void *context)
    usb_fill_bulk_urb(data_in_urb , dev -> udev , data_in_pipe , buffer , 36 , data_done_complete , &done_data);
    //setup the stream id number
    data_in_urb -> stream_id = tag;

    //submit data_in urb
    ret = usb_submit_urb(data_in_urb , GFP_KERNEL);
    if(ret)
    {
        printk(KERN_ERR "my_uas_driver: data urb submit fail %d(from INQUIRY)\n", ret);
        goto free_urb;
    }


    //send the command IU of inquiry through command OUT pipe
    //create the pipe
    cmd_pipe = usb_sndbulkpipe(dev -> udev , dev -> ep_cmd);
    ret = usb_bulk_msg(dev -> udev , cmd_pipe , cmd_iu , sizeof(*cmd_iu) , &actual , 5000);
    //                (dev->udev , pipe_in/pipe_out , cbw/csw , len , actual length send/recv , timeout ms)
    if (ret) 
    {
        printk(KERN_ERR "my_uas_driver: cmd_iu send fail %d\n(from INQUIRY)", ret);
        usb_kill_urb(status_urb);
        usb_kill_urb(data_in_urb);
        goto free_urb;
    }


    //wait for data 
    if (!wait_for_completion_timeout(&done_data, msecs_to_jiffies(5000))) 
    {
        printk(KERN_ERR "my_uas_driver: INQUIRY DATA IN timeout\n");
        usb_kill_urb(status_urb);
        usb_kill_urb(data_in_urb);
        ret = -ETIMEDOUT;
        goto free_urb;
    }

    //wait for status
    if(!wait_for_completion_timeout(&done_status , msecs_to_jiffies(5000)))
    {
        printk(KERN_ERR "my_uas_driver: INQUIRY status timed out\n");
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

    //print the data from the INQUIRY command
    printk(KERN_INFO "my_uas_driver: INQUIRY vendor=%.8s product=%.16s\n", (char *)&buffer[8], (char *)&buffer[16]);

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


