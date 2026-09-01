# BBB multi-file module (probe stays in usb3_driver.c)
obj-m += usb3_bbb.o   #Build a loadable module named usb3_bbb.ko
usb3_bbb-objs := usb3_driver.o bbb_transport.o scsi_cmds.o  #add the dependencies for the file to be build

# for UAS driver
obj-m += uas_driver.o

ccflags-y := -I$(src)/include    #Add /include to compiler search paths 
Add include/ to the compiler search path for headers

all:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	$(MAKE) -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
