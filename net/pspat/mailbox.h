#ifndef __PSPAT_MAILBOX_H
#define __PSPAT_MAILBOX_H

#include <linux/kernel.h>
#include <linux/prefetch.h>

#define START_NEW_CACHELINE	____cacheline_aligned_in_smp

struct pspat_mailbox {
	/* shared (constant) fields */
	unsigned long		entry_mask;
	unsigned long		seqbit_shift;
	unsigned long		line_entries;
	unsigned long		line_mask;

	/* shared field, written by both */
	unsigned long		backpressure;

	/* producer fields */
	START_NEW_CACHELINE
	unsigned long		prod_write;
	unsigned long		prod_check;

	/* consumer fields */
	START_NEW_CACHELINE
	unsigned long		cons_clear;
	unsigned long		cons_read;
	struct list_head	list;

	/* the queue */
	START_NEW_CACHELINE
	uintptr_t		q[0];
};

static inline size_t pspat_mb_size(unsigned long entries)
{
	return roundup(sizeof(struct pspat_mailbox) + entries * sizeof(uintptr_t),
			INTERNODE_CACHE_BYTES);
}

/**
 * pspat_mb_new - create a new mailbox
 * @entries: the number of entries
 * @line_size: the line size in bytes
 *
 * Both entries and line_size must be a power of 2.
 * Returned pointer must be checked with IS_ERR().
 */
struct pspat_mailbox *pspat_mb_new(unsigned long entries, unsigned long line_size);


/**
 * pspat_mb_init - initialize a pre-allocated mailbox
 * @m: the mailbox to be initialized
 * @entries: the number of entries
 * @line_size: the line size in bytes
 *
 * Both entries and line_size must be a power of 2.
 * Returns 0 on success, -errno on failure.
 */
int pspat_mb_init(struct pspat_mailbox *m, unsigned long entries,
		unsigned long line_size);

/**
 * pspat_mb_delete - delete a mailbox
 * @m: the mailbox to be deleted
 */
void pspat_mb_delete(struct pspat_mailbox *m);

/**
 * pspat_mb_insert - enqueue a new value
 * @m: the mailbox where to enqueue
 * @v: the value to be enqueued
 *
 * Returns 0 on success, -ENOBUFS on failure.
 */
static inline int pspat_mb_insert(struct pspat_mailbox *m, void *v)
{
	uintptr_t *h = &m->q[m->prod_write & m->entry_mask];

	if (unlikely(m->prod_write == m->prod_check)) {
		if (*h)
			return -ENOBUFS;
		m->prod_check += m->line_entries;
		prefetch(h + m->line_entries);
	}
	*h = (uintptr_t)v | ((m->prod_write >> m->seqbit_shift) & 0x1);
	m->prod_write++;
	return 0;
}

static inline int __pspat_mb_empty(struct pspat_mailbox *m, uintptr_t v)
{
	return (!v) || ((v ^ (m->cons_read >> m->seqbit_shift)) & 0x1);
}

/**
 * pspat_mb_empty - test for an empty mailbox
 * @m: the mailbox to test
 *
 * Returns non-zero if the mailbox is empty
 */
static inline int pspat_mb_empty(struct pspat_mailbox *m)
{
	uintptr_t v = m->q[m->cons_read & m->entry_mask];

	return __pspat_mb_empty(m, v);
}

/**
 * pspat_mb_extract - extract a value
 * @m: the mailbox where to extract from
 * 
 * Returns the extracted value, NULL if the mailbox
 * is empty. It does not free up any entry, use
 * pspat_mb_clear/pspat_mb_cler_all for that
 */
static inline void *pspat_mb_extract(struct pspat_mailbox *m)
{
	uintptr_t v = m->q[m->cons_read & m->entry_mask];

	if (__pspat_mb_empty(m, v))
		return NULL;

	m->cons_read++;
	return (void *)(v & ~0x1);
}


/**
 * pspat_mb_clear - clear the previously extracted entries
 * @m: the mailbox to be cleared
 *
 */
static inline void pspat_mb_clear(struct pspat_mailbox *m)
{
	unsigned long s = m->cons_read & m->line_mask;

	for ( ; (m->cons_clear & m->line_mask) != s; m->cons_clear += m->line_entries) {
		m->q[m->cons_clear & m->entry_mask] = 0;
	}
}


#endif /* __PSPAT_MAILBOX_H */
