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

/* copy new skbs from client queue to local queue
 * There must be a call to pspat_arb_ack between
 * any two calls to pspat_arb_fetch
 */
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
pspat_mark(struct pspat_queue *pq, struct sk_buff *skb)
{
	uint32_t tail = pq->arb_markq_tail;

	BUG_ON(skb->sender_cpu == 0);

	pq->markq[tail] = skb;

	pspat_next(pq->arb_markq_tail);
}

static uint64_t
pspat_pkt_pico(uint64_t rate, unsigned int len)
{
	return ((8 * (NSEC_PER_SEC << 10)) * len) / pspat_rate;
}

/* copy new skbs to the sender queue */
static void
pspat_arb_publish(struct pspat_queue *pq)
{
	uint32_t head = pq->arb_markq_head;
	uint32_t tail = pq->arb_outq_tail;

	while (pq->markq[head]) {
		if (likely(pq->outq[tail] == NULL)) {
			pq->outq[tail] = pq->markq[head];
		} else {
			kfree_skb(pq->markq[head]);
			// XXX increment dropped counter
		}
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

static void
pspat_send(struct sk_buff *skb)
{
	struct net_device *dev = skb->dev;
	struct netdev_queue *txq;
	int ret = NETDEV_TX_BUSY;

	txq = skb_get_tx_queue(dev, skb);

	HARD_TX_LOCK(dev, txq, smp_processor_id());
	if (!netif_xmit_frozen_or_stopped(txq))
		skb = dev_hard_start_xmit(skb, dev, txq, &ret);
	HARD_TX_UNLOCK(dev, txq);

	if (ret == NETDEV_TX_BUSY) {
		// XXX we should requeue into the qdisc
		kfree_skb(skb);
	}
}

/* Function implementing the arbiter. */
int
pspat_do_arbiter(struct pspat *arb)
{
	unsigned int j = jiffies;

	// printk(KERN_INFO "Arbiter woken up\n");
	
	while (!need_resched() && jiffies < j + msecs_to_jiffies(1000)) {
		int i;
		s64 now = ktime_get_ns() << 10;
		struct Qdisc *q;

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
			if (pq->arb_pending)
				continue;

			if (now < pq->arb_extract_next) {
				continue;
			}
			pq->arb_extract_next = now + (pspat_arb_interval_ns << 10);

			/* 
			 * copy the new skbs from pq to our local cache.
			 */
			pspat_arb_fetch(pq);

			while ( (skb = pspat_arb_get_skb(pq)) ) {
				struct net_device *dev = skb->dev;
				struct netdev_queue *txq;
				int rc;

				/* 
				 * the client chose the txq before sending
				 * the skb to us, so we only need to recover it
				 */
				txq = skb_get_tx_queue(dev, skb);

				q = rcu_dereference_bh(txq->qdisc);
				if (unlikely(!q->pspat_owned)) {
					/* it is the first time we see this Qdisc,
					 * let us try to steal it from the system
					 */
					if (test_and_set_bit(__QDISC_STATE_SCHED,
								&q->state)) {
						/* already scheduled, we need to skip it */
						continue;
					}
					/* add to the list of all the Qdiscs we serve
					 * and initialize the PSPAT-specific fields.
					 * We leave __QDISC_STATE_SCHED set to trick
					 * the system into ignoring the Qdisc
					 */
					q->pspat_owned = 1;
					q->pspat_next = arb->qdiscs;
					arb->qdiscs = q;
					q->pspat_next_link_idle = now;
					/* XXX temporary workaround to set
					 * the per-Qdisc parameters
					 */
					q->pspat_batch_limit = pspat_qdisc_batch_limit;
				}
				rc = q->enqueue(skb, q) & NET_XMIT_MASK;
				if (unlikely(rc)) {
					/* enqueue frees the skb by itself
					 * in case of error, so we have nothing
					 * to do here
					 */
					continue;
				}
				pq->arb_pending++;
			}
		}
		for (q = arb->qdiscs; q; q = q->pspat_next) {
			int ndeq = 0;

			while (q->pspat_next_link_idle <= now &&
				ndeq < q->pspat_batch_limit)
		       	{
				struct pspat_queue *pq;
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
				BUG_ON(!skb->sender_cpu);
			        pq = pspat_arb->queues + skb->sender_cpu - 1;
				pq->arb_pending--;
				if (pspat_direct_xmit) {
					skb = validate_xmit_skb_list(skb, skb->dev);
					pspat_send(skb);
				} else {
					/* validation is done in the sender threads */
					pspat_mark(pq, skb);
				}
				q->pspat_next_link_idle +=
					pspat_pkt_pico(pspat_rate, skb->len);
				ndeq++;
			}
		}

		for (i = 0; i < arb->n_queues; i++) {
			struct pspat_queue *pq = arb->queues + i;
			pspat_arb_publish(pq); /* to senders */
			pspat_arb_ack(pq);     /* to clients */
		}

		rcu_read_unlock_bh();
	}

	return 0;
}

void
pspat_shutdown(struct pspat *arb)
{
	struct Qdisc *q, **pq;

	for (pq = &arb->qdiscs, q = *pq; q; pq = &q->pspat_next, q = *pq) {
		BUG_ON(!test_and_clear_bit(__QDISC_STATE_SCHED, &q->state));
		q->pspat_owned = 0;
		*pq = NULL;
	}
}

int
pspat_client_handler(struct sk_buff *skb, struct Qdisc *q,
	              struct net_device *dev, struct netdev_queue *txq)
{
	int cpu, rc = NET_XMIT_SUCCESS;
	struct pspat_queue *pq;

	if (pspat_debug_xmit) {
		printk(KERN_INFO "q %p dev %p txq %p root_lock %p", q, dev, txq, qdisc_lock(q));
	}

	if (!pspat_enable) {
		/* Not our business. */
		return -ENOTTY;
	}
	qdisc_calculate_pkt_len(skb, q);
	cpu = get_cpu(); /* also disables preemption */
	pq = pspat_arb->queues + cpu;
	if (pspat_cli_push(pq, skb)) {
		pspat_stats[cpu].dropped++;
		kfree_skb(skb);
		rc = NET_XMIT_DROP;
	}
	put_cpu();
	return rc;
}

