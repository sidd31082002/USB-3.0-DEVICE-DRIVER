# UAS multi-file module → uas_driver.ko
obj-m += uas_driver.o
uas_driver-objs := uas_main.o uas_urb_complete_functions.o uas_read10.o uas_capacity.o uas_inquiry.o uas_tur.o

ccflags-y := -I$(src)/include

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
