#ifndef __PSPAT_H__
#define __PSPAT_H__

#define START_NEW_CACHELINE	____cacheline_aligned_in_smp

//#define EMULATE
#define PSPAT_QLEN           128

struct pspat_queue {
	/* Input queue, written by clients, read by the arbiter. */
	START_NEW_CACHELINE
	struct sk_buff		*inq[PSPAT_QLEN];

	/* Data structures private to the clients. */
	START_NEW_CACHELINE
	uint32_t		cli_inq_tail; /* insertion point in inq  */
	uint32_t		cli_outq_head; /* extraction point from outq */

	/* Output queue and s_head index, written by the arbiter,
	 * read by clients. */
	START_NEW_CACHELINE
	struct sk_buff		*outq[PSPAT_QLEN];

	/* Data structures private to the arbiter. */
	START_NEW_CACHELINE
	uint32_t		arb_outq_tail; /* insertion point in outq  */
	uint32_t		arb_inq_head; /* extraction point from inq */
	uint64_t		arb_extract_next;

	struct sk_buff		*qcache[64U/sizeof(struct sk_buff *)];
};

struct pspat {
	struct pspat_queue	queues[8]; /* NUM CORES */
	int			n_queues;

	wait_queue_head_t wqh;
#ifdef EMULATE
	struct timer_list	emu_tmr;
#endif
};

extern struct pspat *pspat_arb;

int pspat_do_arbiter(struct pspat *arb);

int pspat_client_handler(struct sk_buff *skb, struct Qdisc *q,
	              struct net_device *dev, struct netdev_queue *txq);

extern int pspat_enable;
extern int pspat_debug_xmit;
extern uint64_t pspat_arb_interval_tsc;
extern struct pspat_stats *pspat_stats;

struct pspat_stats {
	unsigned long dropped;
} __attribute__((aligned(32)));

#endif  /* __PSPAT_H__ */
