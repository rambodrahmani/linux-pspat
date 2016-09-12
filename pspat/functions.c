#include <linux/types.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/sch_generic.h>

#include "pspat.h"

#define pspat_next(a)	do { (a)++; if (unlikely((a) >= PSPAT_QLEN)) (a) = 0; } while (0)

/* push a new packet to the client queue
 * returns -ENOBUFS if the queue is full
 */
static int
pspat_cli_push(struct pspat_queue *pq, struct sk_buff *skb)
{
	uint32_t tail = pq->cli_inq_tail;

	if (pq->inq[tail])
		return -ENOBUFS;
	pq->inq[tail] = skb;

	pspat_next(pq->cli_inq_tail);

	return 0;
}

static int
pspat_pending_mark(struct pspat_queue *pq)
{
	// XXX we may need a counter
	return 0;
}

/* copy new skbs from client queue to local queue */
static void
pspat_arb_fetch(struct pspat_queue *pq)
{
	uint32_t head = pq->arb_inq_head;
	uint32_t tail = pq->arb_cacheq_tail;

	/* cacheq should always be empty at this point */
	while (pq->inq[head]) {
		pq->cacheq[tail] = pq->inq[head];
		
		pspat_next(head);
		pspat_next(tail);
	}
	pq->arb_inq_head = head;
	pq->arb_cacheq_tail = tail;
}

/* extract skb from the local queue */
static struct sk_buff *
pspat_arb_get_skb(struct pspat_queue *pq)
{
	uint32_t head = pq->arb_cacheq_head;
	struct sk_buff *skb = pq->cacheq[head];
	if (skb) {
		pq->cacheq[head] = NULL;
		pspat_next(pq->arb_cacheq_head);
	}
	return skb;
}

/* locally mark skb as eligible for transmission */
static void
pspat_mark(struct sk_buff *skb)
{
	struct pspat_queue *pq = pspat_arb->queues + skb->sender_cpu - 1;
	uint32_t tail = pq->arb_markq_tail;

	pq->markq[tail] = skb;

	pspat_next(pq->arb_markq_tail);
}

static uint64_t
pspat_pkt_tsc(uint32_t rate, unsigned int len)
{
	// XXX
	return 0;
}

/* copy new skbs to the sender queue */
static void
pspat_arb_publish(struct pspat_queue *pq)
{
	uint32_t head = pq->arb_markq_head;
	uint32_t tail = pq->arb_outq_tail;

	while (pq->markq[head] && !pq->outq[tail]) {
		pq->outq[tail] = pq->markq[head];
		pq->markq[head] = NULL;
		
		pspat_next(head);
		pspat_next(tail);
	}
	pq->arb_markq_head = head;
	pq->arb_outq_tail = tail;
}

/* zero out the used skbs in the client queue */
static void
pspat_arb_ack(struct pspat_queue *pq)
{
	uint32_t ntc = pq->arb_inq_ntc;
	uint32_t head = pq->arb_inq_head;

	while (ntc != head) {
		pq->inq[ntc] = NULL;
		pspat_next(ntc);
	}
	pq->arb_inq_ntc = ntc;
}

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
		struct Qdisc **prevq, *q;

		rcu_read_lock_bh();

		/*
		 * bring in pending packets, arrived between next_link_idle
		 * and now (we assume they arrived at last_check)
		 */
		for (i = 0; i < arb->n_queues; i++) {
			struct pspat_queue *pq = arb->queues + i;
			struct sk_buff *skb;
			/*
			 * Skip clients with at least one packet/burst already
			 * in the scheduler.
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
				txq = skb_get_tx_queue(dev, skb);

				q = rcu_dereference_bh(txq->qdisc);
				rc = q->enqueue(skb, q) & NET_XMIT_MASK;
				if (unlikely(rc)) {
					/* enqueue frees the skb by itself
					 * in case of error, so we have nothing
					 * to do here
					 */
					continue;
				}
				// XXX maybe there is no need for atomicity
				if (test_and_set_bit(__QDISC_STATE_SCHED,
							&q->state)) {
					/* we have alredy scheduled this Qdisc
					 * for transmission
					 */
					continue;
				}
				
				q->next_sched = output_queue;
				output_queue = q;
			}
		}
		prevq = &output_queue;
		q = output_queue;
		while (q) {
			struct Qdisc *cq;
			while (q->pspat_next_link_idle <= now &&
			       ndeq < q->pspat_batch_limit)
			{
				struct sk_buff *skb = q->dequeue(q);
				// XXX things to do when dequeing:
				// - q->gso_skb may contain a "requeued"
				//   packet which should go out first
				//   (without calling ->dequeue())
				// - skb that come out of ->dequeue() must
				//   be "validated" (for segmentation,
				//   checksumming and so on). I think
				//   validation may be done in parallel
				//   in the sender threads.
				//   (see validate_xmit_skb_list())

				if (skb == NULL)
					break;
				pspat_mark(skb);
				q->pspat_next_link_idle +=
					pspat_pkt_tsc(q->pspat_rate, skb->len);
			}
			cq = q;
			q = q->next_sched;
			if (!qdisc_qlen(cq)) {
				// extract from the queue
				*prevq = q;
				// reset the flag
				clear_bit(__QDISC_STATE_SCHED, &cq->state);
			}
			if (q)
				prevq = &q->next_sched;
		}

		for (i = 0; i < arb->n_queues; i++) {
			struct pspat_queue *pq = arb->queues + i;
			pspat_arb_publish(pq); /* to senders */
			pspat_arb_ack(pq);     /* to clients */
		}

		rcu_read_unlock_bh();
	}

	return ndeq;
}


int
pspat_client_handler(struct sk_buff *skb, struct Qdisc *q,
	              struct net_device *dev, struct netdev_queue *txq)
{
	int cpu;
	struct pspat_queue *pq;

	if (pspat_debug_xmit) {
		printk(KERN_INFO "q %p dev %p txq %p root_lock %p", q, dev, txq, qdisc_lock(q));
	}

	if (!pspat_enable) {
		/* Not our business. */
		return -ENOTTY;
	}

	cpu = get_cpu(); /* also disables preemption */
	pq = pspat_arb->queues + cpu;
	if (pspat_cli_push(pq, skb)) {
		pspat_stats[cpu].dropped++;
		kfree_skb(skb);
	}
	put_cpu();
	return 0;
}

