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

//fill a command IU for TUR
static void fill_command_for_TUR(struct command_iu *cmd_iu , u16 tag)
{
    memset(cmd_iu , 0 , sizeof(*cmd_iu));

    cmd_iu -> iu_id = IU_ID_COMMAND;
    cmd_iu -> tag = cpu_to_be16(tag);
    cmd_iu -> prio_attr = UAS_SIMPLE_TAG;
    cmd_iu -> len = 0;
	// lun already 0 
	cmd_iu -> cdb[0] = 0x00;   // TEST UNIT READY
}


//TUR command
int do_tur_cmd(struct my_uas_dev *dev)
{
    struct command_iu *cmd_iu;
    struct sense_iu *sense_iu;
    struct urb *status_urb;
    struct completion done;
    int ret;
    int actual = 0;
    u16 tag;
    unsigned int status_pipe;
    unsigned int cmd_pipe;

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

    //if tags are greater than 32 (max_streams)
    if (tag > dev->num_streams)
        tag = 1; 

    init_completion(&done);

    //fill the TUR Command  IU
    fill_command_for_TUR(cmd_iu , tag);

    status_urb = usb_alloc_urb(0 , GFP_KERNEL);  //creates a new urb for a USB driver to use
    //If the driver want to use this urb for interrupt, control, or bulk endpoints, pass ‘0’ as the number of iso packets.
    if(!status_urb)
    {
        ret = -ENOMEM;
        goto out;   
    }

    status_pipe = usb_rcvbulkpipe(dev -> udev , dev -> ep_status);

    //macro helper to initialize a bulk type urb
    //void usb_fill_bulk_urb(struct urb *urb, struct usb_device *dev, unsigned int pipe, void *transfer_buffer, int buffer_length, usb_complete_t complete_fn, void *context)
    usb_fill_bulk_urb(status_urb , dev -> udev , status_pipe , sense_iu , sizeof(*sense_iu) , status_complete , &done);
    //fill the stream id with the tag so that we can match with the response sense iu
    status_urb->stream_id = tag;

    //submit the status urb to wait for the IN bytes
    ret = usb_submit_urb(status_urb, GFP_KERNEL);
    if (ret) 
    {
        printk(KERN_ERR "my_uas_driver: status urb submit fail %d\n", ret);
        goto free_urb;
    }


    //send command IU through cmd pipe => here we can use the async usb_bulk_msg as command pipe doesnt advertise streams
    // send CIU on command pipe as it does not have stream
    cmd_pipe = usb_sndbulkpipe(dev -> udev, dev -> ep_cmd);
    ret = usb_bulk_msg(dev->udev, cmd_pipe, cmd_iu, sizeof(*cmd_iu), &actual, 5000);
    if (ret) 
    {
        printk(KERN_ERR "my_uas_driver: ciu send fail %d\n", ret);
        usb_kill_urb(status_urb);
        goto free_urb;
    }


    // sleep until status_complete runs (or timeout)
    //wait_for_completion_timeout is a helper which helps to go to sleeping state till the urb is completed 
    //when the urb is completed then it calls the completion which is checked by this helper
    if (!wait_for_completion_timeout(&done, msecs_to_jiffies(5000))) 
    {
        printk(KERN_ERR "my_uas_driver: tur status timeout\n");
        usb_kill_urb(status_urb);
        ret = -ETIMEDOUT;
        goto free_urb;
    }

    //check the status of the status IU
    if (status_urb->status) 
    {
        printk(KERN_ERR "my_uas_driver: status urb status %d\n", status_urb->status);
        ret = status_urb->status;
        goto free_urb;
    }

    printk(KERN_INFO "my_uas_driver: tur siu id=0x%x tag=%u scsi_status=0x%x\n", sense_iu->iu_id, be16_to_cpu(sense_iu->tag), sense_iu->status);


    if(sense_iu->iu_id != IU_ID_STATUS)  //check for the IU id is expected or not
    {
        ret = -EIO;
    }    
    else if(be16_to_cpu(sense_iu->tag) != tag)  //check for the correct value of tag
    {
        ret = -EIO;
    }    
    else if(sense_iu->status != 0)  //check for the status value in the sense IU
    {
        ret = -EIO;
    }
    else
    {
        ret = 0;
    }    

free_urb:
    usb_free_urb(status_urb);
out:
    kfree(sense_iu);
    kfree(cmd_iu);
    return ret;

}
