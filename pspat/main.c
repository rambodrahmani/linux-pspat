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



static int instances = 0; /* To be protected by a lock. */

#ifdef EMULATE
static void
emu_tmr_cb(long unsigned arg)
{
	struct pspat *arb = (struct pspat *)arg;

	wake_up_interruptible(&arb->wqh);
	mod_timer(&arb->emu_tmr, jiffies + msecs_to_jiffies(1000));
}
#endif

int pspat_enable = 0;
int pspat_debug_xmit = 0;
static int pspat_zero = 0;
static int pspat_one = 1;
static unsigned long pspat_ulongzero = 0UL;
static unsigned long pspat_ulongmax = (unsigned long)-1;
static struct ctl_table_header *pspat_sysctl_hdr;

static struct ctl_table pspat_static_ctl[] = {
	{
		.procname	= "cpu",
		.mode		= 0444,
		.child		= NULL, /* created at run-time */
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

        extra_size = cpus * 16 /* space for the syctl names */,
	size = extra_size + sizeof(struct ctl_table) * (cpus + 1);
	buf = kzalloc(size, GFP_KERNEL);
	if (buf == NULL) {
		printk(KERN_WARNING "pspat: unable to allocate sysctls");
		goto free_stats;
	}
	name = buf;
	leaves = buf + extra_size;

	for (i = 0; i < cpus; i++) {
		t = leaves + i;

		n = snprintf(name, extra_size, "dropped-%d", i);
		if (n >= extra_size) { /* truncated */
			printk(KERN_WARNING "pspat: not enough space for per-cpu sysctl names");
			goto free_leaves;
		}
		t->procname	= name;
		name += n + 1;
		extra_size -= n + 1;

		t->maxlen	= sizeof(unsigned long);
		t->mode		= 0644;
		t->data		= &pspat_stats[i].dropped;
		t->proc_handler	= &proc_doulongvec_minmax;
		t->extra1	= &pspat_ulongzero;
		t->extra2	= &pspat_ulongmax;
	}
	pspat_static_ctl[0].child = leaves;
	pspat_sysctl_hdr = register_sysctl_table(pspat_root);

	return 0;

free_leaves:
	kfree(buf);
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
	struct pspat *arb;

	if (instances) {
		printk("PSPAT arbiter already exists\n");
		return -EBUSY;
	}

	arb = kzalloc(sizeof(*arb), GFP_KERNEL);
	if (!arb) {
		return -ENOMEM;
	}

	init_waitqueue_head(&arb->wqh);
	f->private_data = arb;

#ifdef EMULATE
	arb->emu_tmr.function = emu_tmr_cb;
	arb->emu_tmr.data = (long unsigned)arb;
	mod_timer(&arb->emu_tmr, jiffies + msecs_to_jiffies(1000));
#endif
	/* Register the arbiter. */
	rcu_assign_pointer(pspat_handler, pspat_client_handler);
	synchronize_rcu();

	instances ++;

	return 0;
}

static int
pspat_release(struct inode *inode, struct file *f)
{
	struct pspat *arb = (struct pspat *)f->private_data;

#ifdef EMULATE
	del_timer_sync(&arb->emu_tmr);
#endif
	/* Unregister the arbiter. */
	rcu_assign_pointer(pspat_handler, NULL);
	synchronize_rcu();

	kfree(arb);
	f->private_data = NULL;

	instances --;

	return 0;
}

static long
pspat_ioctl(struct file *f, unsigned int cmd, unsigned long flags)
{
	struct pspat *arb = (struct pspat *)f->private_data;
	DECLARE_WAITQUEUE(wait, current);

	(void) cmd;

	add_wait_queue(&arb->wqh, &wait);

	for (;;) {
		/* Wait for a notification or a signal. */
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (signal_pending(current)) {
			printk("Got a signal, returning to userspace\n");
			return 0;
		}
		current->state = TASK_RUNNING;

		/* Invoke the arbiter. */
		pspat_do_arbiter(arb);
	}

	remove_wait_queue(&arb->wqh, &wait);

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
