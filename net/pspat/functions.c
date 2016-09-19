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
static int
pspat_arb_fetch(struct pspat_queue *pq)
{
	uint32_t head = pq->arb_inq_head;
	uint32_t tail = pq->arb_cacheq_tail;
	uint32_t head_first = head;
	int n = 0;

	/* cacheq should always be empty at this point */
	while (pq->inq[head]) {
		n++;
		pq->cacheq[tail] = pq->inq[head];
		
		pspat_next(head);
		pspat_next(tail);
		if (unlikely(head == head_first)) {
			pq->arb_inq_full = 1;
			break;
		}
	}
	pq->arb_inq_head = head;
	pq->arb_cacheq_tail = tail;

	return n;
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
pspat_mark(struct pspat *arb, struct sk_buff *skb)
{
	struct netdev_queue *txq;

	if (pspat_single_txq) {
		skb_set_queue_mapping(skb, 0);
	}
	txq = skb_get_tx_queue(skb->dev, skb);

	BUG_ON(skb->next);
	if (txq->pspat_markq_tail) {
		txq->pspat_markq_tail->next = skb;
	} else {
		txq->pspat_markq_head = skb;
	}
	txq->pspat_markq_tail = skb;
	if (list_empty(&txq->pspat_active)) {
		list_add_tail(&txq->pspat_active, &arb->active_txqs);
	}
}

static uint64_t
pspat_pkt_pico(uint64_t rate, unsigned int len)
{
	return ((8 * (NSEC_PER_SEC << 10)) * len) / pspat_rate;
}

/* copy new skbs to the sender queue */
static void
pspat_arb_publish(struct netdev_queue *txq)
{
#if 0
	uint32_t tail = pq->arb_outq_tail;
	struct sk_buff *skb, *next;

	for (skb = txq->mark_queue_head; skb; skb = next) {
		next = skb->next;
		skb->next = NULL;
		if (likely(pq->outq[tail] == NULL)) {
			pq->outq[tail] = pq->markq[head];
		} else {
			kfree_skb(pq->markq[head]);
			// XXX increment dropped counter
		}
		
		pspat_next(tail);
	}
	pq->arb_outq_tail = tail;
#endif
}

/* zero out the used skbs in the client queue */
static void
pspat_arb_ack(struct pspat_queue *pq)
{
	uint32_t ntc = pq->arb_inq_ntc;
	uint32_t head = pq->arb_inq_head;

	while (ntc != head || pq->arb_inq_full) {
		pq->inq[ntc] = NULL;
		pspat_next(ntc);
		pq->arb_inq_full = 0;
	}
	pq->arb_inq_ntc = ntc;
}

static void
pspat_arb_send(struct netdev_queue *txq)
{
	struct net_device *dev = txq->dev;
	struct sk_buff *skb = txq->pspat_markq_head;
	int ret = NETDEV_TX_BUSY;

	HARD_TX_LOCK(dev, txq, smp_processor_id());
	if (!netif_xmit_frozen_or_stopped(txq))
		skb = dev_hard_start_xmit(skb, dev, txq, &ret);
	else if (unlikely(pspat_debug_xmit))
		printk("txq stopped, drop %p\n", skb);
	HARD_TX_UNLOCK(dev, txq);

	if (!dev_xmit_complete(ret)) {
		// XXX we should requeue into the qdisc
		kfree_skb_list(skb);
	} else {
		pspat_xmit_ok ++;
	}
	txq->pspat_markq_head = txq->pspat_markq_tail = NULL;
}

/* Function implementing the arbiter. */
int
pspat_do_arbiter(struct pspat *arb)
{
	int i, notempty;
	s64 now = ktime_get_ns() << 10;
	struct Qdisc *q = &arb->bypass_qdisc;
	struct netdev_queue *txq_cursor, *txq_next;

	rcu_read_lock_bh();

	/*
	 * bring in pending packets, arrived between next_link_idle
	 * and now (we assume they arrived at last_check)
	 */
	notempty = 0;
	for (i = 0; i < arb->n_queues; i++) {
		struct pspat_queue *pq = arb->queues + i;
		struct sk_buff *skb;

		if (unlikely(pspat_debug_xmit)) {
			printk("Queue #%d: %u %u %u %u %u %u %u %u\n",
				i,
				pq->cli_inq_tail,
				pq->cli_outq_head,
				pq->arb_outq_tail,
				pq->arb_inq_head,
				pq->arb_inq_ntc,
				pq->arb_cacheq_tail,
				pq->arb_cacheq_head,
				pq->arb_inq_full
			      );
		}

		if (now < pq->arb_extract_next) {
			continue;
		}
		pq->arb_extract_next = now + (pspat_arb_interval_ns << 10);

		/* 
		 * copy the new skbs from pq to our local cache.
		 */
		notempty += !!pspat_arb_fetch(pq);

		while ( (skb = pspat_arb_get_skb(pq)) ) {
			struct net_device *dev = skb->dev;
			struct netdev_queue *txq;
			int rc;

			/* 
			 * the client chose the txq before sending
			 * the skb to us, so we only need to recover it
			 */
			BUG_ON(dev == NULL);

			if (!pspat_tc_bypass) {
				txq = skb_get_tx_queue(dev, skb);
				q = rcu_dereference_bh(txq->qdisc);
			}

			if (unlikely(!q->pspat_owned)) {
				struct sk_buff *oskb;
				int can_steal;
				int j = 0;
				/* it is the first time we see this Qdisc,
				 * let us try to steal it from the system
				 */
				spin_lock(qdisc_lock(q));
				can_steal = qdisc_run_begin(q);
				if (can_steal) {
					set_bit(__QDISC_STATE_DEACTIVATED, &q->state);
				}
				spin_unlock(qdisc_lock(q));

				if (!can_steal) {
					if (unlikely(pspat_debug_xmit)) {
						printk("Cannot steal qdisc %p \n", q);
					}
					/* qdisc already running, we have to skip it */
					kfree_skb(skb);
					continue;
				}

				if (q->gso_skb) {
					kfree_skb(q->gso_skb);
					q->gso_skb = NULL;
					q->q.qlen--;
					j ++;
				}
				while ((oskb = q->dequeue(q))) {
					kfree_skb(oskb);
					j ++;
				}
				printk("Stolen qdisc %p, drained %d skbs\n", q, j);

				/* add to the list of all the Qdiscs we serve
				 * and initialize the PSPAT-specific fields.
				 * We leave the QDISC_RUNNING bit set to trick
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
			if (unlikely(pspat_debug_xmit)) {
				printk("enq(%p,%p)-->%d\n", q, skb, rc);
			}
			if (unlikely(rc)) {
				pspat_arb_tc_enq_drop ++;
				/* enqueue frees the skb by itself
				 * in case of error, so we have nothing
				 * to do here
				 */
				continue;
			}
		}
	}
	pspat_rounds[notempty]++;
	for (i = 0; i < arb->n_queues; i++) {
		struct pspat_queue *pq = arb->queues + i;
		pspat_arb_ack(pq);     /* to clients */
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
			pspat_arb_tc_deq ++;
			if (unlikely(pspat_debug_xmit)) {
				printk("deq(%p)-->%p\n", q, skb);
			}
			q->pspat_next_link_idle +=
				pspat_pkt_pico(pspat_rate, skb->len);
			ndeq++;
			BUG_ON(!skb->sender_cpu);
			pq = pspat_arb->queues + skb->sender_cpu - 1;
			switch (pspat_xmit_mode) {
			case 0:
				skb = validate_xmit_skb_list(skb, skb->dev);
				/* fallthrough */
			case 1:
				/* validation is done in the sender threads */
				pspat_mark(arb, skb);
				break;
			default:
				kfree_skb(skb);
				break;
			}
		}
	}

	if (pspat_xmit_mode < 2) {
		list_for_each_entry_safe(txq_cursor, txq_next,
				&arb->active_txqs, pspat_active) {
			if (pspat_xmit_mode == 0)
				pspat_arb_send(txq_cursor);
			else
				pspat_arb_publish(txq_cursor);
			list_del_init(&txq_cursor->pspat_active);
		}
	}

	rcu_read_unlock_bh();

	return 0;
}

void
pspat_shutdown(struct pspat *arb)
{
	struct Qdisc *q, **pq;

	for (pq = &arb->qdiscs, q = *pq; q; pq = &q->pspat_next, q = *pq) {
		spin_lock(qdisc_lock(q));
		qdisc_run_end(q);
		clear_bit(__QDISC_STATE_DEACTIVATED, &q->state);
		spin_unlock(qdisc_lock(q));
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
	struct pspat *arb = rcu_dereference(pspat_arb);

	if (unlikely(pspat_debug_xmit)) {
		printk("handler(%p,%p)\n", arb, skb);
	}

	if (!(pspat_enable && arb)) {
		/* Not our business. */
		return -ENOTTY;
	}

	qdisc_calculate_pkt_len(skb, q);
	cpu = get_cpu(); /* also disables preemption */
	pq = arb->queues + cpu;
	if (pspat_cli_push(pq, skb)) {
		pspat_stats[cpu].inq_drop++;
		kfree_skb(skb);
		rc = NET_XMIT_DROP;
	}
	put_cpu();
	if (unlikely(pspat_debug_xmit)) {
		printk("cli_push(%p) --> %d\n", skb, rc);
	}
	return rc;
}

/* Function implementing the arbiter. */
int
pspat_do_sender(struct pspat *arb)
{
	return 0;
}
