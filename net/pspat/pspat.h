/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __PSPAT_H__
#define __PSPAT_H__

#include "mailbox.h"

/* per-cpu data structure */
struct pspat_queue {
	/* Input queue, a mailbox of mailbox pointers. */
	/* written by clients, read by the arbiter. */
	struct pspat_mailbox *inq;

	/* client fields */
	START_NEW_CACHELINE u64 cli_last_mb;

	/* arbiter fields */
	START_NEW_CACHELINE u64 arb_extract_next;
	struct pspat_mailbox *arb_last_mb;
	struct list_head mb_to_clear;
};

struct pspat_dispatcher {
	struct pspat_mailbox *mb;
	struct list_head active_txqs;
};

struct pspat {
	struct task_struct *arb_task;
	struct task_struct *snd_task;

	/* list of all the qdiscs that we stole from the system */
	struct Qdisc *qdiscs;

	struct Qdisc bypass_qdisc;

	/* list of dead mailboxes to be deleted at the first
	 * safe opportunity
	 */
	struct list_head mb_to_delete;

	/* Statistics to evaluate the cost of an arbiter loop. */
	unsigned int num_loops;
	unsigned int num_reqs;
	u64 max_picos;
	u64 num_picos;
	u64 last_ts;

	/* list of all netdev_queue on which we are actively */
	/* transmitting */
	struct list_head active_txqs;

	/* mailboxes between the arbiter and the dispatchers */
	/* (used with PSPAT_XMIT_MODE_DISPATCH) */
	struct pspat_dispatcher dispatchers[1];

	/* mailboxes between clients and the arbiter */
	int n_queues;
	struct pspat_queue queues[0];
};

extern struct pspat *pspat_arb;

int pspat_do_arbiter(struct pspat *arb);

int pspat_client_handler(struct sk_buff *skb, struct Qdisc *q,
			 struct net_device *dev, struct netdev_queue *txq);
void pspat_shutdown(struct pspat *arb);

int pspat_do_dispatcher(struct pspat_dispatcher *s);

void pspat_dispatcher_shutdown(struct pspat_dispatcher *s);

int pspat_create_client_queue(void);

extern int pspat_enable;
extern int pspat_debug_xmit;
#define PSPAT_XMIT_MODE_ARB		0   /* packets sent by the arbiter */
#define PSPAT_XMIT_MODE_DISPATCH	1   /* packets sent by dispatcher */
#define PSPAT_XMIT_MODE_MAX		2   /* packets dropped by the arbiter */
extern int pspat_xmit_mode;
extern int pspat_tc_bypass;
extern int pspat_single_txq;
extern u64 pspat_rate;
extern u64 pspat_arb_interval_ns;
extern u64 pspat_arb_tc_enq_drop;
extern u64 pspat_arb_backpressure_drop;
extern u64 pspat_arb_tc_deq;
extern u64 pspat_arb_dispatch_drop;
extern u64 pspat_arb_xmit_requeue;
extern u64 pspat_dispatch_deq;
extern u64 *pspat_rounds;
extern u64 pspat_arb_loop_avg_ns;
extern u64 pspat_arb_loop_max_ns;
extern u64 pspat_arb_loop_avg_reqs;
extern uint32_t pspat_arb_qdisc_batch;
extern uint32_t pspat_dispatch_batch;
extern uint32_t pspat_dispatch_sleep_us;
extern struct pspat_stats *pspat_stats;

struct pspat_stats {
	unsigned long inq_drop;
} __aligned(32);

#endif /* __PSPAT_H__ */
