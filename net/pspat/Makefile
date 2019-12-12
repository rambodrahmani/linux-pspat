################################################################################
# File: Makefile
#       Used to build mailbox_test.c as standalone.
#
# Author: Rambod Rahmani <rambodrahmani@autistici.org>
#         Created on 2019-12-12.
################################################################################

CC=gcc
CFLAGS=-Wall
LDFLAGS=-lm

all: mailbox_test

mailbox_test: mailbox_test.o

clean:
	-rm mailbox_test.o mailbox_test

test: mailbox_test
	./mailbox_test

