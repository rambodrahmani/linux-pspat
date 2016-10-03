#ifndef __PSPAT_H__
#define __PSPAT_H__

#include "mailbox.h"

#define PSPAT_QLEN           512

struct pspat_queue {
	/* Input queue, written by clients, read by the arbiter. */
	struct pspat_mailbox   *inq;

	s64			arb_extract_next;
};

struct pspat {
	/* to notify arbiter, currently unused. */
	wait_queue_head_t	wqh;

	/* list of all the qdiscs that we stole from the system */
	struct Qdisc	       *qdiscs;

	struct Qdisc		bypass_qdisc;
	struct list_head	active_txqs;
	int			n_queues;
	struct pspat_queue	queues[0];
};

extern struct pspat *pspat_arb;

int pspat_do_arbiter(struct pspat *arb);

int pspat_client_handler(struct sk_buff *skb, struct Qdisc *q,
	              struct net_device *dev, struct netdev_queue *txq);
void pspat_shutdown(struct pspat *arb);

int pspat_do_sender(struct pspat *arb);

extern int pspat_enable;
extern int pspat_debug_xmit;
extern int pspat_xmit_mode;
extern int pspat_tc_bypass;
extern int pspat_single_txq;
extern u64 pspat_rate;
extern s64 pspat_arb_interval_ns;
extern u64 pspat_arb_tc_enq_drop;
extern u64 pspat_arb_tc_deq;
extern u64 pspat_xmit_ok;
extern u64 *pspat_rounds;
extern uint32_t pspat_qdisc_batch_limit;
extern struct pspat_stats *pspat_stats;

struct pspat_stats {
	unsigned long inq_drop;
} __attribute__((aligned(32)));

#endif  /* __PSPAT_H__ */
