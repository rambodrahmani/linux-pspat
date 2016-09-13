KER=`uname -r`
CC=gcc

all: ker uthread

ker:
	make -C ../.. M=$$(pwd) modules

uthread: uthread.o

clean:
	make -C ../.. M=$$(pwd) clean
	-rm uthread
