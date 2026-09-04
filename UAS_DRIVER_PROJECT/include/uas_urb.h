/* URB completion callbacks for UAS transfers */
#ifndef UAS_URB_H
#define UAS_URB_H

#include <linux/usb.h>

void status_done_complete(struct urb *urb);
void data_done_complete(struct urb *urb);
void status_complete(struct urb *urb);

#endif /* UAS_URB_H */
