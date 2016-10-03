#ifndef __PSPAT_MAILBOX_H
#define __PSPAT_MAILBOX_H

#include <linux/kernel.h>
#include <linux/prefetch.h>

#define START_NEW_CACHELINE	____cacheline_aligned_in_smp

struct pspat_mailbox {
	/* shared (constant) fields */
	unsigned long		size_mask;
	unsigned long		size_shift;
	unsigned long		line_size;
	unsigned long		line_mask;

	/* producer fields */
	START_NEW_CACHELINE
	unsigned long		prod_pi;
	unsigned long		prod_ci;

	/* consumer fields */
	START_NEW_CACHELINE
	unsigned long		cons_pi;
	unsigned long		cons_ci;

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
 * Returns 0 on success, -ENOMEM on failure.
 */
static inline int pspat_mb_insert(struct pspat_mailbox *m, void *v)
{
	uintptr_t *h = &m->q[m->prod_pi & m->size_mask];

	if (unlikely(m->prod_pi == m->prod_ci)) {
		if (*h)
			return -ENOMEM;
		m->prod_ci += m->line_size;
		prefetch(h + m->line_size);
	}
	*h = (uintptr_t)v | ((m->prod_pi >> m->size_shift) & 0x1);
	m->prod_pi++;
	return 0;
}

static inline int __pspat_mb_empty(struct pspat_mailbox *m, uintptr_t v)
{
	return (!v) || ((v ^ (m->cons_ci >> m->size_shift)) & 0x1);
}

/**
 * pspat_mb_empty - test for an empty mailbox
 * @m: the mailbox to test
 *
 * Returns non-zero if the mailbox is empty
 */
static inline int pspat_mb_empty(struct pspat_mailbox *m)
{
	uintptr_t v = m->q[m->cons_ci & m->size_mask];

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
	uintptr_t v = m->q[m->cons_ci & m->size_mask];

	if (__pspat_mb_empty(m, v))
		return NULL;

	m->cons_ci++;
	return (void *)(v & ~0x1);
}


/**
 * pspat_mb_clear - clear the previously extracted entries
 * @m: the mailbox to be cleared
 *
 */
static inline void pspat_mb_clear(struct pspat_mailbox *m)
{
	unsigned long s = m->cons_ci & ~m->line_mask;

	for ( ; (m->cons_pi & ~m->line_mask) != s; m->cons_pi += m->line_size) {
		m->q[m->cons_pi & m->size_mask] = 0;
	}
}


#endif /* __PSPAT_MAILBOX_H */
