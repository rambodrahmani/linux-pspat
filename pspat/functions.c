#include <linux/types.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/sch_generic.h>

#include "pspat.h"

static int
pspat_pending_mark(struct pspat_queue *pq)
{
	// XXX mark skbs in the local queue
	return 0;
}

static void
pspat_arb_fetch(struct pspat_queue *pq)
{
	// XXX copy new skbs from client queue to local queue
}

static struct sk_buff *
pspat_arb_get_skb(struct pspat_queue *pq)
{
	// XXX get next skb from the local queue
	return NULL;
}

#if 0
static void
pspat_arb_publish(struct pspat_queue *pq)
{
	// XXX copy new skbs to the sender queue
}

static void
pspat_arb_ack(struct pspat_queue *pq)
{
	// XXX zero out the used skbs in the client queue
}
#endif

/* Function implementing the arbiter. */
int
pspat_do_arbiter(struct pspat *arb)
{
	int ndeq = 0;
	static struct Qdisc *output_queue;

	printk(KERN_INFO "Arbiter woken up\n");
	
	while (!need_resched()) {
		int i;
		uint64_t now = rdtsc();

		rcu_read_lock_bh();

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

			if (now < pq->arb_extract_next) {
				continue;
			}
			pq->arb_extract_next = now + pspat_arb_interval_tsc;

			/* 
			 * copy the new skbs from pq to our local cache.
			 */
			pspat_arb_fetch(pq);

			while ( (skb = pspat_arb_get_skb(pq)) ) {
				struct net_device *dev = skb->dev;
				struct netdev_queue *txq;
				struct Qdisc *q;
				int rc;

				/* 
				 * the client chose the txq before sending
				 * the skb to us, so we only need to recover it
				 */
				txq = netdev_get_tx_queue(dev, 
						skb_get_queue_mapping(skb));

				q = rcu_dereference_bh(txq->qdisc);
				rc = q->enqueue(skb, q) & NET_XMIT_MASK;
				if (unlikely(rc)) {
					/* enqueue frees the skb by itself
					 * in case of error, so we have nothing
					 * to do here
					 */
					continue;
				}
				if (test_and_set_bit(__QDISC_STATE_SCHED, &q->state)) {
					/* we have alredy scheduled this Qdisc for
					 * transmission
					 */
					continue;
				}
				
				q->next_sched = output_queue;
				output_queue = q;
			}
		}
#if 0
		ndeq = 0;
		while (arb->next_link_idle <= now && ndeq < pspat_arb_batch_limit) {
			struct sk_buff *skb = qdisc->dequeue(qdisc);

			if (skb == NULL)
				break;
			pspat_mark(skb); // recover the cpuid from the skb */
			arb->next_link_idle += pkt_tsc(arb, /* packet len? */);
			ndeq++;
		}

		if (ndeq > 0) {
			for (i = 0; i < arb->n_queues; i++) {
				struct pspat_queue *pq = arb->queues + i;
				pspat_arb_publish(pq); /* to senders */
				pspat_arb_ack(pq);     /* to clients */
			}
		}
#endif

		rcu_read_unlock_bh();
	}

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

