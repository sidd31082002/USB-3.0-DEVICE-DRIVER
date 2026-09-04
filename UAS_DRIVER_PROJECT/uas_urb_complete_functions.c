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

void status_done_complete(struct urb *urb)
{
    struct completion *done_status = urb -> context;
    complete(done_status);
}

void data_done_complete(struct urb *urb)
{
    struct completion *done_data = urb -> context;
    complete(done_data);
}

//completion function of status
void status_complete(struct urb *urb)
{
	struct completion *done = urb->context;
	complete(done);
}
//urb->context will point at completion object(done).
//This is the URB callback. The USB core calls it when that URB is done (success, error, or unlinked).
// must not sleep much there; waking a waiter with complete() is the usual pattern.
