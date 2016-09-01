#include <linux/types.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/sch_generic.h>

#include "pspat.h"


/* Function implementing the arbiter. */
int
pspat_do_arbiter(struct pspat *arb)
{
	printk("Arbiter woken up\n");
	msleep_interruptible(1000);

	return 0;
}


int
pspat_client_handler(struct sk_buff *skb, struct Qdisc *q,
	              struct net_device *dev, struct netdev_queue *txq)
{
	int cpu;

	if (pspat_debug_xmit) {
		printk(KERN_INFO "q %p dev %p txq %p root_lock %p", q, dev, txq, qdisc_lock(q));
	}

	if (!(pspat_enable && pspat_stats)) {
		/* Not our business. */
		return -ENOTTY;
	}

	cpu = get_cpu(); /* also disables preemption */
	pspat_stats[cpu].dropped++;
	put_cpu();
	kfree_skb(skb);

	return 0;
}

