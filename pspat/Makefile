KER=`uname -r`
CC=gcc

obj-m += pspat.o
pspat-y = main.o functions.o

all: ker uthread

ker:
	make -C .. M=$$(pwd) modules

uthread: uthread.o

clean:
	make -C .. M=$$(pwd) clean
	-rm uthread
