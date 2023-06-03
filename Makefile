KDIR ?= /home/zso/linux-6.2.1 

default:
	$(MAKE) -C $(KDIR) M=$$PWD

install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean

mrm:
	sudo rmmod dicedev

mins:
	sudo insmod dicedev.ko
	sudo chmod 777 /dev/dicedev0
	sudo chmod 777 /dev/dicedev1

