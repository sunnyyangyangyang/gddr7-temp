obj-m += gddr7_temp.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod gddr7_temp.ko

unload:
	sudo rmmod gddr7_temp

read:
	cat /proc/gddr7_temp
