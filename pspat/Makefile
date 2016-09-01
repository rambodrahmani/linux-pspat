KER=`uname -r`
CC=gcc

obj-m += pspat_arb.o

all: ker uthread

ker:
	make -C /lib/modules/$(KER)/build M=$(PWD) modules

uthread: uthread.o

clean:
	make -C /lib/modules/$(KER)/build M=$(PWD) clean
	-rm uthread
