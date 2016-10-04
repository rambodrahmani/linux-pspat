#include <linux/types.h>
#include <linux/module.h>
#include <linux/aio.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
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

#include "pspat.h"


DEFINE_MUTEX(pspat_glock);
struct pspat *pspat_arb;

int pspat_enable = 0;
int pspat_debug_xmit = 0;
int pspat_xmit_mode = 0; /* packets sent by the arbiter */
int pspat_single_txq = 1; /* use only one hw queue */
int pspat_tc_bypass = 0;
u64 pspat_rate = 40000000000; // 40Gb/s
s64 pspat_arb_interval_ns = 1000;
u32 pspat_arb_batch_limit = 40;
u32 pspat_qdisc_batch_limit = 40;
u64 pspat_arb_tc_enq_drop = 0;
u64 pspat_arb_tc_deq = 0;
u64 pspat_xmit_ok = 0;
u64 pspat_mailbox_size = 512;
u64 pspat_mailbox_line_size = 128;
u64 *pspat_rounds;
static int pspat_zero = 0;
static int pspat_one = 1;
static int pspat_two = 2;
static unsigned long pspat_ulongzero = 0UL;
static unsigned long pspat_ulongone = 1UL;
static unsigned long pspat_ulongmax = (unsigned long)-1;
static struct ctl_table_header *pspat_sysctl_hdr;
static unsigned long pspat_pages;

static struct ctl_table pspat_static_ctl[] = {
	{
		.procname	= "cpu",
		.mode		= 0444,
		.child		= NULL, /* created at run-time */
	},
	{
		.procname	= "rounds",
		/* .maxlen	computed at runtime */
		.mode		= 0444,
		/* .data	computed at runtime */
		.proc_handler	= &proc_doulongvec_minmax,
		.extra1		= &pspat_ulongzero,
		.extra2		= &pspat_ulongmax,
	},
	{
		.procname	= "enable",
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.data		= &pspat_enable,
		.proc_handler	= &proc_dointvec_minmax,
		.extra1		= &pspat_zero,
		.extra2		= &pspat_one,
	},
	{
		.procname	= "debug_xmit",
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.data		= &pspat_debug_xmit,
		.proc_handler	= &proc_dointvec_minmax,
		.extra1		= &pspat_zero,
		.extra2		= &pspat_one,
	},
	{
		.procname	= "xmit_mode",
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.data		= &pspat_xmit_mode,
		.proc_handler	= &proc_dointvec_minmax,
		.extra1		= &pspat_zero,
		.extra2		= &pspat_two,
	},
	{
		.procname	= "single_txq",
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.data		= &pspat_single_txq,
		.proc_handler	= &proc_dointvec_minmax,
		.extra1		= &pspat_zero,
		.extra2		= &pspat_two,
	},
	{
		.procname	= "tc_bypass",
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.data		= &pspat_tc_bypass,
		.proc_handler	= &proc_dointvec_minmax,
		.extra1		= &pspat_zero,
		.extra2		= &pspat_one,
	},
	{
		.procname	= "arb_interval_ns",
		.maxlen		= sizeof(s64),
		.mode		= 0644,
		.data		= &pspat_arb_interval_ns,
		.proc_handler	= &proc_doulongvec_minmax,
		.extra1		= &pspat_ulongzero,
		.extra2		= &pspat_ulongmax,
	},
	{
		.procname	= "arb_batch_limit",
		.maxlen		= sizeof(u32),
		.mode		= 0644,
		.data		= &pspat_arb_batch_limit,
		.proc_handler	= &proc_dointvec,
	},
	{
		.procname	= "qdisc_batch_limit",
		.maxlen		= sizeof(u32),
		.mode		= 0644,
		.data		= &pspat_qdisc_batch_limit,
		.proc_handler	= &proc_dointvec,
	},
	{
		.procname	= "rate",
		.maxlen		= sizeof(u64),
		.mode		= 0644,
		.data		= &pspat_rate,
		.proc_handler	= &proc_doulongvec_minmax,
		.extra1		= &pspat_ulongone,
		.extra2		= &pspat_ulongmax,
	},
	{
		.procname	= "arb_tc_enq_drop",
		.maxlen		= sizeof(u64),
		.mode		= 0444,
		.data		= &pspat_arb_tc_enq_drop,
		.proc_handler	= &proc_doulongvec_minmax,
		.extra1		= &pspat_ulongzero,
		.extra2		= &pspat_ulongmax,
	},
	{
		.procname	= "arb_tc_deq",
		.maxlen		= sizeof(u64),
		.mode		= 0444,
		.data		= &pspat_arb_tc_deq,
		.proc_handler	= &proc_doulongvec_minmax,
		.extra1		= &pspat_ulongzero,
		.extra2		= &pspat_ulongmax,
	},
	{
		.procname	= "xmit_ok",
		.maxlen		= sizeof(u64),
		.mode		= 0444,
		.data		= &pspat_xmit_ok,
		.proc_handler	= &proc_doulongvec_minmax,
		.extra1		= &pspat_ulongzero,
		.extra2		= &pspat_ulongmax,
	},
	{
		.procname	= "mailbox_size",
		.maxlen		= sizeof(u64),
		.mode		= 0644,
		.data		= &pspat_mailbox_size,
		.proc_handler	= &proc_doulongvec_minmax,
		.extra1		= &pspat_ulongzero,
		.extra2		= &pspat_ulongmax,
	},
	{
		.procname	= "mailbox_line_size",
		.maxlen		= sizeof(u64),
		.mode		= 0644,
		.data		= &pspat_mailbox_line_size,
		.proc_handler	= &proc_doulongvec_minmax,
		.extra1		= &pspat_ulongzero,
		.extra2		= &pspat_ulongmax,
	},
	{}
};

static struct ctl_table pspat_root[] = {
	{
		.procname	= "pspat",
		.mode		= 0444,
		.child		= pspat_static_ctl,
	},
	{}
};

static struct ctl_table pspat_parent[] = {
	{
		.procname	= "net",
		.mode		= 0444,
		.child		= pspat_root,
	},
	{}
};

struct pspat_stats *pspat_stats;

static int
pspat_sysctl_init(void)
{
	int cpus = num_online_cpus(), i, n;
	int rc = -ENOMEM;
	struct ctl_table *t, *leaves;
	void *buf;
	char *name;
	size_t size, extra_size;

	pspat_stats = (struct pspat_stats*)get_zeroed_page(GFP_KERNEL); // XXX max 4096/32 cpus
	if (pspat_stats == NULL) {
		printk(KERN_WARNING "pspat: unable to allocate stats page");
		goto out;
	}

	size = (cpus + 1) * sizeof(u64);
	pspat_rounds = kzalloc(size, GFP_KERNEL);
	if (pspat_rounds == NULL) {
		printk(KERN_WARNING "pspat: unable to allocate rounds counter array\n");
		goto free_stats;
	}
	pspat_static_ctl[1].data = pspat_rounds;
	pspat_static_ctl[1].maxlen = size;

        extra_size = cpus * 16 /* space for the syctl names */,
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
		if (n >= extra_size) { /* truncated */
			printk(KERN_WARNING "pspat: not enough space for per-cpu sysctl names");
			goto free_leaves;
		}
		t->procname	= name;
		name += n + 1;
		extra_size -= n + 1;

		t->maxlen	= sizeof(unsigned long);
		t->mode		= 0644;
		t->data		= &pspat_stats[i].inq_drop;
		t->proc_handler	= &proc_doulongvec_minmax;
		t->extra1	= &pspat_ulongzero;
		t->extra2	= &pspat_ulongmax;
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

static void
pspat_sysctl_fini(void)
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
extern int (*pspat_handler)(struct sk_buff *, struct Qdisc *,
			    struct net_device *,
			    struct netdev_queue *);

static int
pspat_open(struct inode *inode, struct file *f)
{
	/* Do nothing, initialization is on-demand. */
	f->private_data = NULL;

	return 0;
}

static int
pspat_release(struct inode *inode, struct file *f)
{
	if (!f->private_data) {
		/* Nothing was created, nothing to destroy. */
		return 0;
	}

	mutex_lock(&pspat_glock);
	if (f->private_data == pspat_arb) {
		/* Destroy arbiter. */
		struct pspat *arb = pspat_arb;

		/* Unregister the arbiter. */
		rcu_assign_pointer(pspat_arb, NULL);
		synchronize_rcu();

		pspat_shutdown(arb);
		free_pages((unsigned long)arb, order_base_2(pspat_pages));

		f->private_data = NULL;
		printk("PSPAT arbiter destroyed\n");

	} else {
		/* Destroy transmitter. */
	}
	mutex_unlock(&pspat_glock);

	return 0;
}

static int
pspat_bypass_enqueue(struct sk_buff *skb, struct Qdisc *q)
{
	return __qdisc_enqueue_tail(skb, q, &q->q);
}

static struct sk_buff *
pspat_bypass_dequeue(struct Qdisc *q)
{
	return __qdisc_dequeue_head(q, &q->q);
}

static int
pspat_create(struct file *f, unsigned int cmd)
{
	int cpus = num_online_cpus(), i;
	struct pspat *arb;
	struct pspat_mailbox *m;
	unsigned long mb_entries, mb_line_size;
	size_t mb_size, arb_size;

	if (cmd < cpus) {
		/* Create a transmitter thread. */
		f->private_data = f;
		return 0;
	}
	
	/* get the current value of the mailbox parameters */
	mb_entries = pspat_mailbox_size;
	mb_line_size = pspat_mailbox_line_size;
	mb_size = pspat_mb_size(mb_entries);

	/* Create the arbiter on demand. */
	mutex_lock(&pspat_glock);
	if (pspat_arb) {
		mutex_unlock(&pspat_glock);
		printk("PSPAT arbiter already exists\n");

		return -EBUSY;
	}

	arb_size = roundup(sizeof(*arb) + cpus * sizeof(*arb->queues),
			INTERNODE_CACHE_BYTES);
	pspat_pages = DIV_ROUND_UP(arb_size + mb_size * cpus, PAGE_SIZE);

	arb = (struct pspat *)__get_free_pages(GFP_KERNEL, order_base_2(pspat_pages));
	if (!arb) {
		mutex_unlock(&pspat_glock);
		return -ENOMEM;
	}
	memset(arb, 0, PAGE_SIZE * pspat_pages);
	f->private_data = arb;
	arb->n_queues = cpus;

	/* initialize all mailboxes */
	m = (void *)arb + arb_size;
	for (i = 0; i < cpus; i++) {
		int err = pspat_mb_init(m, mb_entries, mb_line_size);
		if (err) {
			free_pages((unsigned long)arb, order_base_2(pspat_pages));
			mutex_unlock(&pspat_glock);
			return err;
		}
		arb->queues[i].inq = m;
		INIT_LIST_HEAD(&arb->queues[i].mb_to_clear);
		m = (void *)m + mb_size;
	}

	/* Initialize bypass qdisc. */
	arb->bypass_qdisc.enqueue = pspat_bypass_enqueue;
	arb->bypass_qdisc.dequeue = pspat_bypass_dequeue;
	skb_queue_head_init(&arb->bypass_qdisc.q);
	arb->bypass_qdisc.pspat_owned = 0;
	arb->bypass_qdisc.state = 0;
	arb->bypass_qdisc.__state = 0;

	init_waitqueue_head(&arb->wqh);

	INIT_LIST_HEAD(&arb->active_txqs);

	/* Register the arbiter. */
	rcu_assign_pointer(pspat_arb, arb);
	synchronize_rcu();
	printk("PSPAT arbiter created with %d per-core queues\n",
	       arb->n_queues);

	mutex_unlock(&pspat_glock);

	return 0;
}

int
pspat_create_client_queue(void)
{
	struct pspat_mailbox *m;

	if (current->pspat_mb)
		return 0;

	m = pspat_mb_new(pspat_mailbox_size, pspat_mailbox_line_size);
	if (m == NULL)
		return -ENOMEM;
	current->pspat_mb = m;
	return 0;
}

static long
pspat_ioctl(struct file *f, unsigned int cmd, unsigned long flags)
{
	DECLARE_WAITQUEUE(wait, current);
	bool blocking = false;

	if (f->private_data == NULL) {
		/* We need to create something on demand. */
		int ret = pspat_create(f, cmd);

		if (ret) {
			return ret;
		}
	}

	if (blocking) {
		add_wait_queue(&pspat_arb->wqh, &wait);
	}

	for (;;) {
		/* Wait for a notification or a signal. */
		if (need_resched()) {
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(1);
			current->state = TASK_RUNNING;
		}

		if (signal_pending(current)) {
			printk("Got a signal, returning to userspace\n");
			break;
		}

		if (f->private_data == pspat_arb) {
			/* Invoke the arbiter. */
			pspat_do_arbiter(pspat_arb);
		} else {
			/* Invoke the transmitter. */
			pspat_do_sender(pspat_arb);
		}
	}

	if (blocking) {
		remove_wait_queue(&pspat_arb->wqh, &wait);
	}

	return 0;
}

static const struct file_operations pspat_fops = {
	.owner          = THIS_MODULE,
	.release        = pspat_release,
	.open           = pspat_open,
	.unlocked_ioctl = pspat_ioctl,
	.llseek         = noop_llseek,
};

static struct miscdevice pspat_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pspat",
	.fops = &pspat_fops,
};

static int __init
pspat_init(void)
{
	int ret;

	ret = pspat_sysctl_init();
	if (ret) {
		printk("pspat_sysctl_init() failed\n");
		return ret;
	}

	ret = misc_register(&pspat_misc);
	if (ret) {
		printk("Failed to register rlite misc device\n");
		return ret;
	}

	return 0;
}

static void __exit
pspat_fini(void)
{
	misc_deregister(&pspat_misc);
	pspat_sysctl_fini();
}

module_init(pspat_init);
module_exit(pspat_fini);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vincenzo Maffione <v.maffione@gmail.com>");
