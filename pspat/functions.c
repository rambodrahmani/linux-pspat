#include <linux/types.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/sch_generic.h>

#include "pspat.h"


/* Function implementing the arbiter. */
int
pspat_do_arbiter(struct pspat *arb /*, uint64_t now */)
{
	int ndeq = 0;

	printk("Arbiter woken up\n");
	
#if 0
	while (!need_resched()) {
		int i;

		/*
		 * bring in pending packets, arrived between next_link_idle and now
		 * (we assume they arrived at last_check)
		 */
		for (i = 0; i < arb->n_queues; i++) {
			struct pspat_queue *pq = arb->queues + i;
			struct sk_buff *skb;
			/*
			 * Skip clients with at least one packet/burst already in the
			 * scheduler. This is true if s_nhead != s_tail, and
			 * is a useful optimization.
			 */
			if (pspat_pending_mark(pq)) {
				continue;
			}

			if (now < pq->sch_extract_next) {
				continue;
			}
			pq->sch_extract_next = now + arb->sched_interval_tsc; // XXX sysctl?

			pspat_arb_fetch(pq); // XXX copy new skbs and zero-out

			while ( (skb = pspat_arb_get_skb(pq)) ) {
				struct Qdisc *qdisc = /* obtain from skb (see __dev_queue_xmit) */;
				rc = qdisc->enqueue(skb, qdisc) & NET_XMIT_MASK;
				/* enqueue frees the skb by itself in case of error, so we have
				 * nothing special to do here
				 */
			}
		}

		ndeq = 0;
		while (arb->next_link_idle <= now && ndeq < arb->sched_batch_limit) {
			struct sk_buff *skb = qdisc->dequeue(qdisc);

			if (skb == NULL)
				break;
			pspat_mark(skb); // recover the cpuid from the skb */
			arb->next_link_idle += pkt_tsc(arb, /* packet len? */);
			ndeq++;
		}

		if (ndeq > 0) {
			for (i = 0; i < arb->queues; i++) {
				struct pspat_queue *q = arb->queues + i;
				pspat_arb_publish(q);
			}
		}
	}

#endif
	return ndeq;
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
#if 0
	if (/* full */) {
		pspat_stats[cpu].dropped++;
		kfree_skb(skb);
	} else {
		/* save the cpu id into the skb */
		/* push into arb->queue[cpu] */
	}
#else
	pspat_stats[cpu].dropped++;
	kfree_skb(skb);
#endif
	put_cpu();
	return 0;
}

