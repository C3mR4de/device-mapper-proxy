obj-m += dmp.o

PWD := $(CURDIR)

all: build test clean

build:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

test: test_init test_exec test_fini

test_init:
	insmod dmp.ko
	dmsetup create zero1 --table "0 20480 zero"
	dmsetup create dmp1 --table "0 20480 dmp /dev/mapper/zero1"

test_exec:
	dd if=/dev/random of=/dev/mapper/dmp1 bs=4k count=1
	dd of=/dev/null   if=/dev/mapper/dmp1 bs=4k count=1
	cat /sys/module/dmp/stat/volumes

test_fini:
	dmsetup remove dmp1
	dmsetup remove zero1
	rmmod dmp.ko

