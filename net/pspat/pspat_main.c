// SPDX-License-Identifier: GPL-2.0+

#include <linux/types.h>
#include <linux/module.h>
#include <linux/aio.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <net/sch_generic.h>
#include <linux/rcupdate.h>
#include <linux/kthread.h>

#include "pspat.h"

DEFINE_MUTEX(pspat_glock);
struct pspat *pspat_arb;	/* RCU-dereferenced */
static struct pspat *arbp;	/* For internal usage */

int pspat_enable __read_mostly;
int pspat_debug_xmit __read_mostly;
int pspat_xmit_mode __read_mostly = PSPAT_XMIT_MODE_ARB;
int pspat_single_txq __read_mostly = 1;	/* use only one hw queue */
int pspat_tc_bypass __read_mostly;
u64 pspat_rate __read_mostly = 40000000000;	// 40Gb/s
u64 pspat_arb_interval_ns __read_mostly = 1000;
u32 pspat_arb_qdisc_batch __read_mostly = 512;
u32 pspat_dispatch_batch __read_mostly = 256;
u32 pspat_dispatch_sleep_us __read_mostly;
u64 pspat_arb_tc_enq_drop;
u64 pspat_arb_backpressure_drop;
u64 pspat_arb_tc_deq;
u64 pspat_arb_dispatch_drop;
u64 pspat_dispatch_deq;
u64 pspat_arb_loop_avg_ns;
u64 pspat_arb_loop_max_ns;
u64 pspat_arb_loop_avg_reqs;
u64 pspat_mailbox_entries = 512;
u64 pspat_mailbox_line_size = 128;
u64 *pspat_rounds;		/* currently unused */
static int pspat_zero;
static int pspat_one = 1;
static int pspat_two = 2;
static unsigned long pspat_ulongzero;
static unsigned long pspat_ulongone = 1UL;
static unsigned long pspat_ulongmax = (unsigned long)-1;
static struct ctl_table_header *pspat_sysctl_hdr;
static unsigned long pspat_pages;

static int
pspat_enable_proc_handler(struct ctl_table *table, int write,
				void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write || !pspat_enable || !arbp) {
		return ret;
	}

	wake_up_process(arbp->arb_task);
	wake_up_process(arbp->snd_task);

	return 0;
}

static int
pspat_xmit_mode_proc_handler(struct ctl_table *table, int write,
				void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (ret || !write || !pspat_enable || !arbp
	    || pspat_xmit_mode != PSPAT_XMIT_MODE_DISPATCH) {
		return ret;
	}

	wake_up_process(arbp->snd_task);

	return 0;
}

static struct ctl_table pspat_static_ctl[] = {
	{
	 .procname = "cpu",
	 .mode = 0444,
	 .child = NULL,		/* created at run-time */
	 },
	{
	 .procname = "rounds",
	 /* .maxlen      computed at runtime */
	 .mode = 0444,
	 /* .data        computed at runtime */
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 },
	{
	 .procname = "enable",
	 .maxlen = sizeof(int),
	 .mode = 0644,
	 .data = &pspat_enable,
	 .proc_handler = &pspat_enable_proc_handler,
	 .extra1 = &pspat_zero,
	 .extra2 = &pspat_one,
	 },
	{
	 .procname = "debug_xmit",
	 .maxlen = sizeof(int),
	 .mode = 0644,
	 .data = &pspat_debug_xmit,
	 .proc_handler = &proc_dointvec_minmax,
	 .extra1 = &pspat_zero,
	 .extra2 = &pspat_one,
	 },
	{
	 .procname = "xmit_mode",
	 .maxlen = sizeof(int),
	 .mode = 0644,
	 .data = &pspat_xmit_mode,
	 .proc_handler = &pspat_xmit_mode_proc_handler,
	 .extra1 = &pspat_zero,
	 .extra2 = &pspat_two,
	 },
	{
	 .procname = "single_txq",
	 .maxlen = sizeof(int),
	 .mode = 0644,
	 .data = &pspat_single_txq,
	 .proc_handler = &proc_dointvec_minmax,
	 .extra1 = &pspat_zero,
	 .extra2 = &pspat_two,
	 },
	{
	 .procname = "tc_bypass",
	 .maxlen = sizeof(int),
	 .mode = 0644,
	 .data = &pspat_tc_bypass,
	 .proc_handler = &proc_dointvec_minmax,
	 .extra1 = &pspat_zero,
	 .extra2 = &pspat_one,
	 },
	{
	 .procname = "arb_interval_ns",
	 .maxlen = sizeof(u64),
	 .mode = 0644,
	 .data = &pspat_arb_interval_ns,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "arb_qdisc_batch",
	 .maxlen = sizeof(u32),
	 .mode = 0644,
	 .data = &pspat_arb_qdisc_batch,
	 .proc_handler = &proc_dointvec,
	 }
	,
	{
	 .procname = "dispatch_batch",
	 .maxlen = sizeof(u32),
	 .mode = 0644,
	 .data = &pspat_dispatch_batch,
	 .proc_handler = &proc_dointvec,
	 }
	,
	{
	 .procname = "dispatch_sleep_us",
	 .maxlen = sizeof(u32),
	 .mode = 0644,
	 .data = &pspat_dispatch_sleep_us,
	 .proc_handler = &proc_dointvec,
	 }
	,
	{
	 .procname = "rate",
	 .maxlen = sizeof(u64),
	 .mode = 0644,
	 .data = &pspat_rate,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongone,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "arb_tc_enq_drop",
	 .maxlen = sizeof(u64),
	 .mode = 0444,
	 .data = &pspat_arb_tc_enq_drop,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "arb_backpressure_drop",
	 .maxlen = sizeof(u64),
	 .mode = 0444,
	 .data = &pspat_arb_backpressure_drop,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "arb_tc_deq",
	 .maxlen = sizeof(u64),
	 .mode = 0444,
	 .data = &pspat_arb_tc_deq,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "arb_dispatch_drop",
	 .maxlen = sizeof(u64),
	 .mode = 0444,
	 .data = &pspat_arb_dispatch_drop,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "dispatch_deq",
	 .maxlen = sizeof(u64),
	 .mode = 0444,
	 .data = &pspat_dispatch_deq,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "arb_loop_avg_ns",
	 .maxlen = sizeof(u64),
	 .mode = 0444,
	 .data = &pspat_arb_loop_avg_ns,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "arb_loop_max_ns",
	 .maxlen = sizeof(u64),
	 .mode = 0444,
	 .data = &pspat_arb_loop_max_ns,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "arb_loop_avg_reqs",
	 .maxlen = sizeof(u64),
	 .mode = 0444,
	 .data = &pspat_arb_loop_avg_reqs,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "mailbox_entries",
	 .maxlen = sizeof(u64),
	 .mode = 0644,
	 .data = &pspat_mailbox_entries,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{
	 .procname = "mailbox_line_size",
	 .maxlen = sizeof(u64),
	 .mode = 0644,
	 .data = &pspat_mailbox_line_size,
	 .proc_handler = &proc_doulongvec_minmax,
	 .extra1 = &pspat_ulongzero,
	 .extra2 = &pspat_ulongmax,
	 }
	,
	{}
};

static struct ctl_table pspat_root[] = {
	{
	 .procname = "pspat",
	 .mode = 0444,
	 .child = pspat_static_ctl,
	 },
	{}
};

static struct ctl_table pspat_parent[] = {
	{
	 .procname = "net",
	 .mode = 0444,
	 .child = pspat_root,
	 },
	{}
};

struct pspat_stats *pspat_stats;

static int pspat_sysctl_init(void)
{
	int cpus = num_online_cpus(), i, n;
	int rc = -ENOMEM;
	struct ctl_table *t, *leaves;
	void *buf;
	char *name;
	size_t size, extra_size;

	pspat_stats = (struct pspat_stats *)get_zeroed_page(GFP_KERNEL);	// XXX max 4096/32 cpus
	if (pspat_stats == NULL) {
		printk(KERN_WARNING "pspat: unable to allocate stats page");
		goto out;
	}

	size = (cpus + 1) * sizeof(u64);
	pspat_rounds = kzalloc(size, GFP_KERNEL);
	if (pspat_rounds == NULL) {
		printk(KERN_WARNING
		       "pspat: unable to allocate rounds counter array\n");
		goto free_stats;
	}
	pspat_static_ctl[1].data = pspat_rounds;
	pspat_static_ctl[1].maxlen = size;

	extra_size = cpus * 16 /* space for the syctl names */ ,
	    size = extra_size + sizeof(struct ctl_table) * (cpus + 1);
	buf = kzalloc(size, GFP_KERNEL);
	if (buf == NULL) {
		printk(KERN_WARNING "pspat: unable to allocate sysctls");
		goto free_rounds;
	}
	name = buf;
	leaves = buf + extra_size;

	for (i = 0; i < cpus; i++) {
		t = leaves + i;

		n = snprintf(name, extra_size, "inq-drop-%d", i);
		if (n >= extra_size) {	/* truncated */
			printk(KERN_WARNING
			       "pspat: not enough space for per-cpu sysctl names");
			goto free_leaves;
		}
		t->procname = name;
		name += n + 1;
		extra_size -= n + 1;

		t->maxlen = sizeof(unsigned long);
		t->mode = 0644;
		t->data = &pspat_stats[i].inq_drop;
		t->proc_handler = &proc_doulongvec_minmax;
		t->extra1 = &pspat_ulongzero;
		t->extra2 = &pspat_ulongmax;
	}
	pspat_static_ctl[0].child = leaves;
	pspat_sysctl_hdr = register_sysctl_table(pspat_parent);

	return 0;

free_leaves:
	kfree(buf);
free_rounds:
	kfree(pspat_rounds);
free_stats:
	free_page((unsigned long)pspat_stats);
out:
	return rc;
}

static void pspat_sysctl_fini(void)
{
	if (pspat_sysctl_hdr)
		unregister_sysctl_table(pspat_sysctl_hdr);
	if (pspat_static_ctl[0].child)
		kfree(pspat_static_ctl[0].child);
	if (pspat_rounds)
		kfree(pspat_rounds);
	if (pspat_stats)
		free_page((unsigned long)pspat_stats);
}

/* Hook exported by net/core/dev.c */
extern int (*pspat_handler) (struct sk_buff *, struct Qdisc *,
			     struct net_device *, struct netdev_queue *);

static int arb_worker_func(void *data)
{
	struct pspat *arb = (struct pspat *)data;
	bool arb_registered = false;

	while (!kthread_should_stop()) {
		if (!pspat_enable) {
			if (arb_registered) {
				/* PSPAT is disabled but arbiter is still
				 * registered: we need to unregister. */
				mutex_lock(&pspat_glock);
				pspat_shutdown(arb);
				rcu_assign_pointer(pspat_arb, NULL);
				synchronize_rcu();
				mutex_unlock(&pspat_glock);
				arb_registered = false;
				printk("PSPAT arbiter unregistered\n");
			}

			set_current_state(TASK_INTERRUPTIBLE);
			schedule();

		} else {
			if (!arb_registered) {
				/* PSPAT is enabled but arbiter is not
				 * registered: we need to register. */
				mutex_lock(&pspat_glock);
				rcu_assign_pointer(pspat_arb, arb);
				synchronize_rcu();
				mutex_unlock(&pspat_glock);
				arb_registered = true;
				printk("PSPAT arbiter registered\n");
				arb->last_ts = ktime_get_ns() << 10;
				arb->num_loops = 0;
				arb->num_picos = 0;
				arb->max_picos = 0;
			}

			pspat_do_arbiter(arb);
			if (need_resched()) {
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(1);
			}
		}
	}

	return 0;
}

static int snd_worker_func(void *data)
{
	struct pspat_dispatcher *s = (struct pspat_dispatcher *)data;

	while (!kthread_should_stop()) {
		if (pspat_xmit_mode != PSPAT_XMIT_MODE_DISPATCH
		    || !pspat_enable) {
			printk("PSPAT dispatcher deactivated\n");
			pspat_dispatcher_shutdown(s);
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
			printk("PSPAT dispatcher activated\n");

		} else {
			pspat_do_dispatcher(s);
			if (need_resched()) {
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(1);
			}
		}
	}

	return 0;
}

static int pspat_destroy(void)
{
	mutex_lock(&pspat_glock);
	BUG_ON(arbp == NULL);

	/* Unregister the arbiter. */
	rcu_assign_pointer(pspat_arb, NULL);
	synchronize_rcu();

	if (arbp->arb_task) {
		kthread_stop(arbp->arb_task);
		arbp->arb_task = NULL;
	}
	if (arbp->snd_task) {
		kthread_stop(arbp->snd_task);
		arbp->snd_task = NULL;
	}

	pspat_dispatcher_shutdown(&arbp->dispatchers[0]);
	pspat_shutdown(arbp);
	free_pages((unsigned long)arbp, order_base_2(pspat_pages));
	arbp = NULL;

	printk("PSPAT arbiter destroyed\n");
	mutex_unlock(&pspat_glock);

	return 0;
}

int pspat_create_client_queue(void)
{
	struct pspat_mailbox *m;
	char name[PSPAT_MB_NAMSZ];

	if (current->pspat_mb)
		return 0;

	snprintf(name, PSPAT_MB_NAMSZ, "CM-%d", current->pid);

	m = pspat_mb_new(name, pspat_mailbox_entries, pspat_mailbox_line_size);
	if (m == NULL)
		return -ENOMEM;
	current->pspat_mb = m;
	return 0;
}

static int
pspat_bypass_enqueue(struct sk_buff *skb, struct Qdisc *q,
		     struct sk_buff **to_free)
{
	return qdisc_enqueue_tail(skb, q);
}

static struct sk_buff *pspat_bypass_dequeue(struct Qdisc *q)
{
	return qdisc_dequeue_head(q);
}

static struct lock_class_key qdisc_tx_busylock;
static struct lock_class_key qdisc_running_key;

static int pspat_create(void)
{
	int cpus = num_online_cpus(), i;
	int dispatchers = 1;
	struct pspat_mailbox *m;
	unsigned long mb_entries, mb_line_size;
	size_t mb_size, arb_size;
	int ret;

	/* get the current value of the mailbox parameters */
	mb_entries = pspat_mailbox_entries;
	mb_line_size = pspat_mailbox_line_size;
	mb_size = pspat_mb_size(mb_entries);

	mutex_lock(&pspat_glock);
	BUG_ON(arbp != NULL);

	arb_size = roundup(sizeof(*arbp) + cpus * sizeof(*arbp->queues),
			   INTERNODE_CACHE_BYTES);
	pspat_pages = DIV_ROUND_UP(arb_size + mb_size * (cpus + dispatchers),
				   PAGE_SIZE);

	arbp =
	    (struct pspat *)__get_free_pages(GFP_KERNEL,
					     order_base_2(pspat_pages));
	if (!arbp) {
		mutex_unlock(&pspat_glock);
		return -ENOMEM;
	}
	memset(arbp, 0, PAGE_SIZE * pspat_pages);
	arbp->n_queues = cpus;

	/* initialize all mailboxes */
	m = (void *)arbp + arb_size;
	for (i = 0; i < cpus; i++) {
		char name[PSPAT_MB_NAMSZ];
		snprintf(name, PSPAT_MB_NAMSZ, "CL-%d", i);
		ret = pspat_mb_init(m, name, mb_entries, mb_line_size);
		if (ret) {
			goto fail;
		}
		arbp->queues[i].inq = m;
		INIT_LIST_HEAD(&arbp->queues[i].mb_to_clear);
		m = (void *)m + mb_size;
	}
	INIT_LIST_HEAD(&arbp->mb_to_delete);
	INIT_LIST_HEAD(&arbp->active_txqs);

	for (i = 0; i < dispatchers; i++) {
		char name[PSPAT_MB_NAMSZ];
		snprintf(name, PSPAT_MB_NAMSZ, "T-%d", i);
		ret = pspat_mb_init(m, name, mb_entries, mb_line_size);
		if (ret) {
			goto fail;
		}
		arbp->dispatchers[i].mb = m;
		INIT_LIST_HEAD(&arbp->dispatchers[i].active_txqs);
		m = (void *)m + mb_size;
	}

	/* Initialize bypass qdisc. */
	arbp->bypass_qdisc.enqueue = pspat_bypass_enqueue;
	arbp->bypass_qdisc.dequeue = pspat_bypass_dequeue;
	qdisc_skb_head_init(&arbp->bypass_qdisc.q);
	spin_lock_init(&arbp->bypass_qdisc.q.lock);
	spin_lock_init(&arbp->bypass_qdisc.busylock);
	lockdep_set_class(&arbp->bypass_qdisc.busylock, &qdisc_tx_busylock);
	seqcount_init(&arbp->bypass_qdisc.running);
	lockdep_set_class(&arbp->bypass_qdisc.running, &qdisc_running_key);
	refcount_set(&arbp->bypass_qdisc.refcnt, 1);
	arbp->bypass_qdisc.pspat_owned = 0;
	arbp->bypass_qdisc.state = 0;

	arbp->arb_task = kthread_create(arb_worker_func, arbp, "pspat-arb");
	if (IS_ERR(arbp->arb_task)) {
		ret = -PTR_ERR(arbp->arb_task);
		goto fail;
	}

	arbp->snd_task = kthread_create(snd_worker_func, &arbp->dispatchers[0],
					"pspat-snd");
	if (IS_ERR(arbp->snd_task)) {
		ret = -PTR_ERR(arbp->snd_task);
		goto fail2;
	}

	printk("PSPAT arbiter created with %d per-core queues\n",
	       arbp->n_queues);

	mutex_unlock(&pspat_glock);

	wake_up_process(arbp->arb_task);
	wake_up_process(arbp->snd_task);

	return 0;
fail2:
	kthread_stop(arbp->arb_task);
	arbp->arb_task = NULL;
fail:
	free_pages((unsigned long)arbp, order_base_2(pspat_pages));
	mutex_unlock(&pspat_glock);

	return ret;
}

static int __init pspat_init(void)
{
	int ret;

	ret = pspat_sysctl_init();
	if (ret) {
		printk("pspat_sysctl_init() failed\n");
		return ret;
	}

	ret = pspat_create();
	if (ret) {
		printk("Failed to create arbiter\n");
		goto err1;
	}

	return 0;
err1:
	pspat_sysctl_fini();

	return ret;
}

static void __exit pspat_fini(void)
{
	pspat_destroy();
	pspat_sysctl_fini();
}

module_init(pspat_init);
module_exit(pspat_fini);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Giuseppe Lettieri <g.lettieri@iet.unipi.it");
MODULE_AUTHOR("Vincenzo Maffione <v.maffione@gmail.com>");
MODULE_AUTHOR("Luigi Rizzo <rizzo@iet.unipi.it>");
