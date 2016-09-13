This module implements the PSPAT scheduler.

It registers the hook 'pspat_handler' in net/core/dev.c :: __dev_xmit_skb()
which in turn replaces __dev_xmkt_skb()

functions.c
	body of the arbiter

main.c
	code to register the hook, support sysctl variables and
	run the arbiter in-kernel using an ioctl().

pspat.h
	data structures including struct pspat_queue (one per client)
	and struct pspat (one per arbiter, embeds the queues)

uthread.c
	user code to talk to the driver
