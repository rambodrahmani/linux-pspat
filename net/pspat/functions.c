#include <linux/types.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/sch_generic.h>

#include "pspat.h"

/* Pseudo-identifier for client mailboxes. It's used by pspat_cli_push()
 * to decide when to insert an entry in the CL. Way safer than the previous
 * approach, but there are still theoretical race conditions for an
 * identifier to be reused while a previous process with the same identifier
 * is still alive.
 */
static atomic_t mb_next_id = ATOMIC_INIT(0);

/* push a new packet to the client queue
 * returns -ENOBUFS if the queue is full
 */
static int
pspat_cli_push(struct pspat_queue *pq, struct sk_buff *skb)
{
	struct pspat_mailbox *m;
	int err;

	if (unlikely(current->pspat_mb == NULL)) {
		err = pspat_create_client_queue();
		if (err)
			return err;
		current->pspat_mb->identifier = atomic_inc_return(&mb_next_id);
	}
	m = current->pspat_mb;

        /* The backpressure flag set tells us that the qdisc is being overrun.
         * We return an error to propagate the overrun to the client. */
	if (unlikely(m->backpressure)) {
		m->backpressure = 0;
		if (pspat_debug_xmit) {
			printk("mailbox %p backpressure\n", m);
		}
		return -ENOBUFS;
	}

	err = pspat_mb_insert(m, skb);
	if (err)
		return err;
	/* avoid duplicate notification */
	if (pq->cli_last_mb != m->identifier) {
		smp_mb(); /* let the arbiter see the insert above */
		err = pspat_mb_insert(pq->inq, m);
		BUG_ON(err);
		pq->cli_last_mb = m->identifier;
	}

	return 0;
}

static void
pspat_cli_delete(struct pspat *arb, struct pspat_mailbox *m)
{
	int i;
	/* remove m from all the client lists current-mb pointers */
	for (i = 0; i < arb->n_queues; i++) {
		struct pspat_queue *pq = arb->queues + i;
		if (pq->arb_last_mb == m)
			pq->arb_last_mb = NULL;
	}
	/* possibily remove this mb from the ack list */
	if (!list_empty(&m->list)) {
		list_del(&m->list);
	}
	/* insert into the list of mb to be delete */
	list_add_tail(&m->list, &arb->mb_to_delete);
}

static struct pspat_mailbox *
pspat_arb_get_mb(struct pspat_queue *pq)
{
	struct pspat_mailbox *m = pq->arb_last_mb;

	if (m == NULL || pspat_mb_empty(m)) {
		m = pspat_mb_extract(pq->inq);
		if (m) {
			if (list_empty(&pq->inq->list)) {
				list_add_tail(&pq->inq->list,
						&pq->mb_to_clear);
			}
			pq->arb_last_mb = m;
			/* wait for previous updates in the new mailbox */
			smp_mb();
		}
	}
	return m;
}

/* extract skb from the local queue */
static struct sk_buff *
pspat_arb_get_skb(struct pspat *arb, struct pspat_queue *pq)
{
	struct pspat_mailbox *m;
	struct sk_buff *skb;

retry:
	/* first, get the current mailbox for this cpu */
	m = pspat_arb_get_mb(pq);
	if (m == NULL) {
		return NULL;
	}
	/* try to extract an skb from the current mailbox */
	skb = pspat_mb_extract(m);
	if (skb) {
		/* let pspat_arb_ack() see this mailbox */
		if (list_empty(&m->list)) {
			list_add_tail(&m->list, &pq->mb_to_clear);
		}
	} else  if (unlikely(m->dead)) {
		/* the client is gone, the arbiter takes
		 * responsibility in deleting the mb
		 */
		pspat_cli_delete(arb, m);
		goto retry;
	}
	return skb;
}

static inline void
pspat_arb_prefetch(struct pspat *arb, struct pspat_queue *pq)
{
	if (pq->arb_last_mb != NULL)
		pspat_mb_prefetch(pq->arb_last_mb);
}

/* mark skb as eligible for transmission on a netdev_queue, and
 * make sure this queue is part of the list of active queues */
static inline void
pspat_mark(struct list_head *active_queues, struct sk_buff *skb)
{
	struct netdev_queue *txq = skb_get_tx_queue(skb->dev, skb);

	BUG_ON(skb->next);
	if (txq->pspat_markq_tail) {
		txq->pspat_markq_tail->next = skb;
	} else {
		txq->pspat_markq_head = skb;
	}
	txq->pspat_markq_tail = skb;
	if (list_empty(&txq->pspat_active)) {
		list_add_tail(&txq->pspat_active, active_queues);
	}
}

/* move skb to the a sender queue */
static int
pspat_arb_dispatch(struct pspat *arb, struct sk_buff *skb)
{
	struct pspat_dispatcher *s = &arb->dispatchers[0];
	int err;

	err = pspat_mb_insert(s->mb, skb);
	if (err) {
		/* Drop this skb and possibly set the backpressure
		 * flag for the last client on the per-CPU queue
		 * where this skb was transmitted. */
		struct pspat_mailbox *cli_mb;
		struct pspat_queue *pq;

		BUG_ON(!skb->sender_cpu);
		pq = pspat_arb->queues + skb->sender_cpu - 1;
		cli_mb = pq->arb_last_mb;

		if (cli_mb && !cli_mb->backpressure) {
			cli_mb->backpressure = 1;
		}
		pspat_arb_dispatch_drop ++;
		kfree_skb(skb);
	}

	return err;
}

/* Zero out the used skbs in the client mailboxes and the
 * client lists. */
static void
pspat_arb_ack(struct pspat_queue *pq)
{
	struct pspat_mailbox *mb_cursor, *mb_next;

	list_for_each_entry_safe(mb_cursor, mb_next, &pq->mb_to_clear, list) {
		pspat_mb_clear(mb_cursor);
		list_del_init(&mb_cursor->list);
	}
}

/* delete all known dead mailboxes */
static void
pspat_arb_delete_dead_mbs(struct pspat *arb)
{
	struct pspat_mailbox *mb_cursor, *mb_next;

	list_for_each_entry_safe(mb_cursor, mb_next, &arb->mb_to_delete, list) {
		list_del(&mb_cursor->list);
		pspat_mb_delete(mb_cursor);
	}
}

static void
pspat_arb_drain(struct pspat *arb, struct pspat_queue *pq)
{
	struct pspat_mailbox *m = pq->arb_last_mb;
	struct sk_buff *skb;
	int dropped = 0;

	BUG_ON(!m);
	while ( (skb = pspat_arb_get_skb(arb, pq)) ) {
		kfree_skb(skb);
		dropped++;
	}

	if (!m->backpressure) {
		m->backpressure = 1;
	}

	if (unlikely(pspat_debug_xmit)) {
		printk("PSPAT drained mailbox %s [%d skbs]\n", m->name, dropped);
	}
	pspat_arb_backpressure_drop += dropped;
}

/* Flush the markq associated to a device transmit queue. Returns 0 if all the
 * packets in the markq were transmitted. A non-zero return code means that the
 * markq has not been emptied. */
static inline int
pspat_txq_flush(struct netdev_queue *txq)
{
	struct net_device *dev = txq->dev;
	int ret = NETDEV_TX_BUSY;
	struct sk_buff *skb;

	/* Validate all the skbs in the markq. Some (or all) the skbs may be
	 * dropped. The function may modify the markq head/tail pointers. */
	txq->pspat_markq_head = validate_xmit_skb_list(txq->pspat_markq_head,
						dev, &txq->pspat_markq_tail);
	/* Append the markq to the validq (handling the case where the validq
	 * was empty and/or the markq is empty) and reset the markq. */
	if (txq->pspat_validq_head == NULL) {
		txq->pspat_validq_head = txq->pspat_markq_head;
		txq->pspat_validq_tail = txq->pspat_markq_tail;
	} else if (txq->pspat_markq_head) {
		txq->pspat_validq_tail->next = txq->pspat_markq_head;
		txq->pspat_validq_tail = txq->pspat_markq_tail;
	}
	txq->pspat_markq_head = txq->pspat_markq_tail = NULL;
	skb = txq->pspat_validq_head;

	HARD_TX_LOCK(dev, txq, smp_processor_id());
	if (!netif_xmit_frozen_or_stopped(txq)) {
		skb = dev_hard_start_xmit(skb, dev, txq, &ret);
	}
	HARD_TX_UNLOCK(dev, txq);

	/* The skb pointer here is NULL if all packets were transmitted.
	 * Otherwise it points to a list of packets to be transmitted. */
	txq->pspat_validq_head = skb;
	if (!skb) {
		/* All packets were transmitted, we can just reset
		 * the validq tail (head was reset above). */
		BUG_ON(!dev_xmit_complete(ret));
		txq->pspat_validq_tail = NULL;
		return 0;
	}

	return 1;
}

static void
pspat_txqs_flush(struct list_head *txqs)
{
	struct netdev_queue *txq, *txq_next;

	list_for_each_entry_safe(txq, txq_next, txqs, pspat_active) {
		if (pspat_txq_flush(txq) == 0) {
			list_del_init(&txq->pspat_active);
		}
	}
}

#define PSPAT_ARB_STATS_LOOPS	0x1000

/* Function implementing the arbiter. */
int
pspat_do_arbiter(struct pspat *arb)
{
	int i;
	u64 now = ktime_get_ns() << 10, picos;
	struct Qdisc *q = &arb->bypass_qdisc;
	static u64 last_pspat_rate = 0;
	static u64 picos_per_byte = 1;
	unsigned int nreqs = 0;
	/* number of empty client lists found in the last round
	 * (after a round with only empty CLs, we can safely
	 * delete the mbs in the mb_to_delete list)
	 */
	int empty_inqs = 0;

	if (unlikely(pspat_rate != last_pspat_rate)) {
		/* Avoid division in the dequeue stage below by
		 * precomputing the number of pseudo-picoseconds per byte.
		 * Recomputation is done only when needed. */
		last_pspat_rate = pspat_rate;
		picos_per_byte = (8 * (NSEC_PER_SEC << 10)) / last_pspat_rate;
	}

	rcu_read_lock_bh();

	/*
	 * bring in pending packets, arrived between pspat_next_link_idle
	 * and now (we assume they arrived at last_check)
	 */

	for (i = 0; i < arb->n_queues; i++) {
		struct pspat_queue *pq = arb->queues + i;
		struct sk_buff *to_free = NULL;
		struct sk_buff *skb;
		bool empty = true;

		if (now < pq->arb_extract_next) {
			continue;
		}
		pq->arb_extract_next = now + (pspat_arb_interval_ns << 10);

		pspat_arb_prefetch(arb, (i + 1 < arb->n_queues ? pq + 1 : arb->queues));

		while ( (skb = pspat_arb_get_skb(arb, pq)) ) {
			int rc;

			empty = false;
			++nreqs;
			if (!pspat_tc_bypass) {
				/*
				 * the client chose the txq before sending
				 * the skb to us, so we only need to recover it
				 */
				struct net_device *dev = skb->dev;
				struct netdev_queue *txq;
				BUG_ON(dev == NULL);
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
					qdisc_qstats_backlog_dec(q, skb);
					q->q.qlen--;
					j ++;
				}
				if (q->skb_bad_txq) {
					kfree_skb(q->skb_bad_txq);
					q->skb_bad_txq = NULL;
					qdisc_qstats_backlog_dec(q, skb);
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
				q->pspat_batch_limit = pspat_arb_qdisc_batch;
			}

			if (unlikely(skb->next)) {
				printk("WARNING: skb->next was not NULL\n");
				skb->next = skb->prev = NULL;
			}
			rc = q->enqueue(skb, q, &to_free) & NET_XMIT_MASK;
			if (unlikely(pspat_debug_xmit)) {
				printk("enq(%p,%p)-->%d\n", q, skb, rc);
			}
			if (unlikely(rc)) {
                                /* q->enqueue is starting to drop packets, e.g.
                                 * one internal queue in the qdisc is full. We
                                 * would like to propagate this signal to the
                                 * client, so we set the backpressure flag. We
                                 * also drain the mailbox because it may not be
                                 * anymore in the clients list. */
				pspat_arb_tc_enq_drop ++;
				pspat_arb_drain(arb, pq);
			}
		}
		if (to_free) {
			kfree_skb_list(to_free);
		}
		if (empty) {
			++empty_inqs;
		}
	}
	if (empty_inqs == arb->n_queues) {
		pspat_arb_delete_dead_mbs(arb);
	}
	for (i = 0; i < arb->n_queues; i++) {
		struct pspat_queue *pq = arb->queues + i;
		pspat_arb_ack(pq);     /* to clients */
	}
	for (q = arb->qdiscs; q; q = q->pspat_next) {
		u64 next_link_idle = q->pspat_next_link_idle;
		unsigned int ndeq = 0;

		while (next_link_idle <= now &&
			ndeq < pspat_arb_qdisc_batch)
		{
			struct sk_buff *skb = q->gso_skb;

			if (unlikely(skb)) {
				/* q->gso_skb may contain a "requeued"
				   packet which should go out first
				   (without calling ->dequeue()) */
				q->gso_skb = NULL;
				qdisc_qstats_backlog_dec(q, skb);
				q->q.qlen--;
			} else {
				skb = q->dequeue(q);
			}

			if (skb == NULL)
				break;
			ndeq++;
			if (unlikely(pspat_debug_xmit)) {
				printk("deq(%p)-->%p\n", q, skb);
			}
			next_link_idle += picos_per_byte * skb->len;

			if (pspat_single_txq) { /* possibly override txq */
				skb_set_queue_mapping(skb, 0);
			}

			switch (pspat_xmit_mode) {
			case PSPAT_XMIT_MODE_ARB:
				pspat_mark(&arb->active_txqs, skb);
				break;
			case PSPAT_XMIT_MODE_DISPATCH:
				pspat_arb_dispatch(arb, skb);
				break;
			default:
				kfree_skb(skb);
				break;
			}
		}
		pspat_arb_tc_deq += ndeq;

                /* If the traffic on this root qdisc is not enough to fill
                 * the link bandwidth, we need to move next_link_idle
                 * forward, in order to avoid accumulating credits. */
                if (next_link_idle <= now &&
			ndeq < pspat_arb_qdisc_batch) {
                    next_link_idle = now;
                }
		q->pspat_next_link_idle = next_link_idle;
	}

	if (pspat_xmit_mode == PSPAT_XMIT_MODE_ARB) {
		pspat_txqs_flush(&arb->active_txqs);
	}

	rcu_read_unlock_bh();

	/* Update statistics on avg/max cost of the arbiter loop and
	 * per-loop client mailbox processing. */
	picos = now - arb->last_ts;
	arb->last_ts = now;
	arb->num_picos += picos;
	arb->num_reqs += nreqs;
	arb->num_loops++;
	if (unlikely(picos > arb->max_picos)) {
		arb->max_picos = picos;
	}
	if (unlikely(arb->num_loops & PSPAT_ARB_STATS_LOOPS)) {
		pspat_arb_loop_avg_ns =
			(arb->num_picos / PSPAT_ARB_STATS_LOOPS) >> 10;
		pspat_arb_loop_max_ns = arb->max_picos >> 10;
		pspat_arb_loop_avg_reqs = arb->num_reqs / PSPAT_ARB_STATS_LOOPS;
		arb->num_loops = 0;
		arb->num_picos = 0;
		arb->max_picos = 0;
		arb->num_reqs = 0;
	}

	return 0;
}

void
pspat_shutdown(struct pspat *arb)
{
	struct netdev_queue *txq, *txq_next;
	struct Qdisc *q, **_q;
	int n;
	int i;

	/* We need to drain all the client lists and client mailboxes
	 * to discover and free up all dead client mailboxes. */
	for (i = 0, n = 0; i < arb->n_queues; i++) {
		struct pspat_queue *pq = arb->queues + i;
		struct sk_buff *skb;

		while ( (skb = pspat_arb_get_skb(arb, pq)) ) {
			kfree_skb(skb);
			n ++;
		}
	}
	printk("%s: CMs drained, found %d skbs\n", __func__, n);

	/* Also drain the validq of all the active tx queues. */
	n = 0;
	list_for_each_entry_safe(txq, txq_next, &arb->active_txqs, pspat_active) {
		/* We can't call kfree_skb_list(), because this function does
		 * not unlink the skbuffs from the list.
		 * Unlinking is important in case the refcount of some of the
		 * skbuffs does not go to zero here, that would mean possible
		 * dangling pointers. */
		while (txq->pspat_validq_head != NULL) {
			struct sk_buff *next = txq->pspat_validq_head->next;
			txq->pspat_validq_head->next = NULL;
			kfree_skb(txq->pspat_validq_head);
			txq->pspat_validq_head = next;
			n ++;
		}
		txq->pspat_validq_tail = NULL;
		list_del_init(&txq->pspat_active);
		BUG_ON(txq->pspat_markq_head != NULL ||
			txq->pspat_markq_tail != NULL);
	}
	printk("%s: Arbiter validq lists drained, found %d skbs\n", __func__, n);

	/* Return all the stolen qdiscs. */
	for (n = 0, _q = &arb->qdiscs, q = *_q; q; _q = &q->pspat_next, q = *_q) {
		spin_lock(qdisc_lock(q));
		qdisc_run_end(q);
		spin_unlock(qdisc_lock(q));
		q->pspat_owned = 0;
		*_q = NULL;
		n ++;
	}
	printk("%s: %d qdiscs released\n", __func__, n);
}

int
pspat_client_handler(struct sk_buff *skb, struct Qdisc *q,
	              struct net_device *dev, struct netdev_queue *txq)
{
	int cpu, rc = NET_XMIT_SUCCESS;
	struct pspat_queue *pq;
	struct pspat *arb;

	if (!pspat_enable || (arb = rcu_dereference(pspat_arb)) == NULL) {
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

/* Called on process exit() to clean-up PSPAT mailbox, if any. */
void
exit_pspat(void)
{
	struct pspat *arb;
	struct pspat_queue *pq;
	int cpu;

	if (current->pspat_mb == NULL)
		return;

	current->pspat_mb->dead = 1;

retry:
	rcu_read_lock();
	arb = rcu_dereference(pspat_arb);
	if (arb) {
		/* If the arbiter is running, we cannot delete the mailbox
		 * by ourselves. Instead, we set the "dead" flag and insert
		 * the mailbox in the client list.
		 */
		cpu = get_cpu();
		pq = arb->queues + cpu;
		if (pspat_mb_insert(pq->inq, current->pspat_mb) == 0) {
			current->pspat_mb = NULL;
		}
		put_cpu();
	}
	rcu_read_unlock();
	if (current->pspat_mb) {
		/* the mailbox is still there */
		if (arb) {
			/* We failed to push PSPAT_LAST_SKB but the
			 * arbiter was running. We must try again.
			 */
			printk("PSPAT Try again to destroy mailbox\n");
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(100);
			goto retry;
		} else {
			/* The arbiter is not running. Since
			 * pspat_shutdown() drains everything, any
			 * new arbiter will not see this mailbox.
			 * Therefore, we can safely free it up.
			 */
			pspat_mb_delete(current->pspat_mb);
			current->pspat_mb = NULL;
		}
	}
}

/* Body of the dispatcher. */
int
pspat_do_dispatcher(struct pspat_dispatcher *s)
{
	struct pspat_mailbox *m = s->mb;
	struct sk_buff *skb;
	int ndeq = 0;

	while (ndeq < pspat_dispatch_batch && (skb = pspat_mb_extract(m)) != NULL) {
		pspat_mark(&s->active_txqs, skb);
		ndeq ++;
	}

	pspat_dispatch_deq += ndeq;
	pspat_mb_clear(m);
	pspat_txqs_flush(&s->active_txqs);

	if (unlikely(pspat_debug_xmit && ndeq)) {
		printk("PSPAT sender processed %d skbs\n", ndeq);
	}

	if (pspat_dispatch_sleep_us) {
		usleep_range(pspat_dispatch_sleep_us, pspat_dispatch_sleep_us);
	}

	return ndeq;
}

void
pspat_dispatcher_shutdown(struct pspat_dispatcher *s)
{
	struct netdev_queue *txq, *txq_next;
	struct sk_buff *skb;
	int n = 0;

	/* Drain the sender mailbox. */
	while ( (skb = pspat_mb_extract(s->mb)) ) {
		kfree_skb(skb);
		n ++;
	}
	printk("%s: Sender MB drained, found %d skbs\n", __func__, n);

	/* Also drain the validq of all the active tx queues. */
	n = 0;
	list_for_each_entry_safe(txq, txq_next, &s->active_txqs, pspat_active) {
		/* We can't call kfree_skb_list(), because this function does
		 * not unlink the skbuffs from the list.
		 * Unlinking is important in case the refcount of some of the
		 * skbuffs does not go to zero here, that would mean possible
		 * dangling pointers. */
		while (txq->pspat_validq_head != NULL) {
			struct sk_buff *next = txq->pspat_validq_head->next;
			txq->pspat_validq_head->next = NULL;
			kfree_skb(txq->pspat_validq_head);
			txq->pspat_validq_head = next;
			n ++;
		}
		txq->pspat_validq_tail = NULL;
		list_del_init(&txq->pspat_active);
		BUG_ON(txq->pspat_markq_head != NULL ||
			txq->pspat_markq_tail != NULL);
	}
	printk("%s: Sender validq lists drained, found %d skbs\n", __func__, n);
}

