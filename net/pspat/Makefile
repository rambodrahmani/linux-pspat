# Makefile used to build mailbox_test.c as standalone.

CC=gcc
CFLAGS=-Wall
LDFLAGS=-lm

all: mailbox_test

mailbox_test: mailbox_test.o

clean:
	-rm mailbox_test.o mailbox_test

test: mailbox_test
	./mailbox_test

