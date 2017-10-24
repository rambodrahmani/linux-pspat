#include <linux/gfp.h>
#include <linux/slab.h>
#include "mailbox.h"

struct pspat_mailbox*
pspat_mb_new(unsigned long entries, unsigned long line_size)
{
	struct pspat_mailbox *m;
	int err;

	m = kzalloc(pspat_mb_size(entries), GFP_KERNEL);
	if (m == NULL)
		return ERR_PTR(-ENOMEM);

	err = pspat_mb_init(m, entries, line_size);
	if (err) {
		kfree(m);
		return ERR_PTR(err);
	}

	return m;
}

int
pspat_mb_init(struct pspat_mailbox *m, unsigned long entries,
		unsigned long line_size)
{
	unsigned long entries_per_line;

	if (!is_power_of_2(entries) || !is_power_of_2(line_size) ||
			entries <= 2 * line_size || line_size < sizeof(uintptr_t))
		return -EINVAL;

	entries_per_line = line_size / sizeof(uintptr_t);

	m->line_entries = entries_per_line;
	m->line_mask = ~(entries_per_line - 1);
	m->entry_mask = entries - 1;
	m->seqbit_shift = ilog2(entries);

	printk("line_entries %lu line_mask %lx entry_mask %lx seqbit_shift %lu\n",
		m->line_entries, m->line_mask, m->entry_mask, m->seqbit_shift);
	
	m->cons_clear = 0;
	m->cons_read = m->line_entries;
	m->prod_write = m->line_entries;
	m->prod_check = 2 * m->line_entries;

	INIT_LIST_HEAD(&m->list);

	return 0;
}

void
pspat_mb_delete(struct pspat_mailbox *m)
{
	kfree(m);
}
