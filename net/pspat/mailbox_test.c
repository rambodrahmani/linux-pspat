// SPDX-License-Identifier: GPL-2.0+

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <math.h>

/*
 * Glue code to compile the in-kernel mailbox code in user-space.
 */
#define kfree(x) free(x)
#define kmalloc(x, y) malloc((x))
#define kzalloc(x, y) calloc(1, (x))
#define START_NEW_CACHELINE __aligned(size)
struct list_head {
};

/* The `const' in roundup() prevents gcc-3.3 from calling __divdi3 */
#define roundup(x, y) (				\
{						\
	const typeof(y) __y = y;		\
	(((x) + (__y - 1)) / __y) * __y;	\
}						\
)
#define INTERNODE_CACHE_BYTES   64
#define likely(x)       __builtin_expect((x), 1)
#define unlikely(x)     __builtin_expect((x), 0)
#define prefetch(x) __builtin_prefetch((x))
#define printk      printf
#define INIT_LIST_HEAD(x)

static inline __attribute__ ((const))
int is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}

static unsigned long ilog2(unsigned long x)
{
	int res = -1;

	while (x) {
		res++;
		x = x >> 1;
	}
	assert(res >= 0);
	return (unsigned long)res;
}

static inline void *ERR_PTR(long error)
{
	return (void *)error;
}

#define MAX_ERRNO   4095
#define IS_ERR_VALUE(x) unlikely((x) >= (unsigned long)-MAX_ERRNO)

static inline int IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((unsigned long)ptr);
}

#define BUG_ON(x)   assert(!(x))
/*
 * End of glue code.
 */

#include "mailbox.c"

typedef int (*testfunc_t) (struct pspat_mailbox *mb, unsigned int entries);

#define EXPECT_TRUE(x) assert(!!(x))
#define EXPECT_FALSE(x) assert(!(x))
#define EXPECT_OK(x)   assert((x) == 0)
#define EXPECT_FAIL(x)   assert((x) != 0)
#define EXPECT_EQ(x, y) assert((x) == (y))

static int mb_fill_limit(struct pspat_mailbox *mb, unsigned int limit)
{
	void *v = mb;
	int n = 0;

	while (n < limit && pspat_mb_insert(mb, /*value= */ v) == 0) {
		n++;
		v += 4;
	}
	return n;
}

static int mb_fill(struct pspat_mailbox *mb)
{
	return mb_fill_limit(mb, ~0U);
}

static int mb_drain_limit(struct pspat_mailbox *mb, unsigned int limit)
{
	int n = 0;

	while (n < limit && pspat_mb_extract(mb) != NULL) {
		n++;
	}
	return n;
}

static int mb_drain(struct pspat_mailbox *mb)
{
	return mb_drain_limit(mb, ~0U);
}

/**
 * Test function n. 1
 * Check if the mailbox is empty.
 */
static int test1(struct pspat_mailbox *mb, unsigned int entries)
{
	EXPECT_TRUE(pspat_mb_empty(mb));
	return 0;
}

/**
 * Test function n. 2
 * Insert into an empty mailbox.
 */
static int test2(struct pspat_mailbox *mb, unsigned int entries)
{
	EXPECT_TRUE(pspat_mb_empty(mb));
	EXPECT_OK(pspat_mb_insert(mb, /*value= */ mb + 1));
	EXPECT_FALSE(pspat_mb_empty(mb));
	return 0;
}

/**
 * Test function n. 3
 * Insert into an empty mailbox, extract and check it is
 * now empty.
 */
static int test3(struct pspat_mailbox *mb, unsigned int entries)
{
	void *v;

	EXPECT_OK(pspat_mb_insert(mb, /*value= */ mb + 1));
	EXPECT_FALSE(pspat_mb_empty(mb));
	v = pspat_mb_extract(mb);
	EXPECT_EQ(mb + 1, v);
	EXPECT_TRUE(pspat_mb_empty(mb));
	return 0;
}

/**
 * Test function n. 4
 * Check we can fill the mb completely, and after
 * that we cannot insert anymore.
 */
static int test4(struct pspat_mailbox *mb, unsigned int entries)
{
	int n = mb_fill(mb);
	EXPECT_EQ(n, entries - mb->line_entries);
	EXPECT_FALSE(pspat_mb_empty(mb));
	EXPECT_FAIL(pspat_mb_insert(mb, /*value= */ mb - 1));
	EXPECT_FALSE(pspat_mb_empty(mb));
	return 0;
}

/**
 * Test function n. 5
 * Fill in and drain, checking that we got back everything
 * we had inserted.
 */
static int test5(struct pspat_mailbox *mb, unsigned int entries)
{
	int maxn = entries - mb->line_entries;
	int n = mb_fill(mb);
	EXPECT_EQ(n, maxn);
	n = mb_drain(mb);
	EXPECT_EQ(n, maxn);
	EXPECT_TRUE(pspat_mb_empty(mb));
	return 0;
}

/**
 * Test function n. 6
 * Fill, drain, and check that we cannot insert anymore
 * without clearing.
 */
static int test6(struct pspat_mailbox *mb, unsigned int entries)
{
	int maxn = entries - mb->line_entries;
	int n = mb_fill(mb);
	EXPECT_EQ(n, maxn);
	n = mb_drain(mb);
	EXPECT_EQ(n, maxn);
	EXPECT_FAIL(pspat_mb_insert(mb, /*value= */ mb - 4));
	return 0;
}

/**
 * Test function n. 7
 * Fill, drain and clear. Then check that we can fill again.
 */
static int test7(struct pspat_mailbox *mb, unsigned int entries)
{
	int maxn = entries - mb->line_entries;
	int n = mb_fill(mb);
	EXPECT_EQ(n, maxn);
	n = mb_drain(mb);
	EXPECT_EQ(n, maxn);
	pspat_mb_clear(mb);
	EXPECT_TRUE(pspat_mb_empty(mb));
	n = mb_fill(mb);
	EXPECT_EQ(n, maxn);
	return 0;
}

/**
 * Test function n. 8
 * Fill, drain and clear many times. Always fill/drain until
 * running out of resources.
 */
static int test8(struct pspat_mailbox *mb, unsigned int entries)
{
	int maxn = entries - mb->line_entries;
	int arbitrary_num_cycles = entries / 3;
	int i;

	for (i = 0; i < arbitrary_num_cycles; i++) {
		int n = mb_fill(mb);
		EXPECT_EQ(n, maxn);
		n = mb_drain(mb);
		EXPECT_EQ(n, maxn);
		pspat_mb_clear(mb);
		EXPECT_TRUE(pspat_mb_empty(mb));
	}
	return 0;
}

/**
 * Test function n. 9
 * Mixed operations.
 */
static int test9(struct pspat_mailbox *mb, unsigned int entries)
{
	int maxn = entries - mb->line_entries;
	int n;

	n = mb_fill_limit(mb, maxn / 5);	/* +1/5 */
	EXPECT_EQ(n, maxn / 5);
	n = mb_drain_limit(mb, maxn / 11);	/* -1/11 */
	EXPECT_EQ(n, maxn / 11);
	n = mb_drain_limit(mb, maxn / 12);	/* -1/12 */
	EXPECT_EQ(n, maxn / 12);
	n = mb_drain_limit(mb, 1);	/* -1 */
	EXPECT_EQ(n, 1);
	n = mb_fill_limit(mb, maxn / 7);	/* +1/7 */
	EXPECT_EQ(n, maxn / 7);
	n = mb_drain_limit(mb, 2);	/* -2 */
	EXPECT_EQ(n, 2);
	n = mb_drain(mb);
	EXPECT_EQ(n, maxn / 5 + maxn / 7 - maxn / 11 - maxn / 12 - 1 - 2);
	EXPECT_TRUE(pspat_mb_empty(mb));

	return 0;
}

/**
 * Test function n. 10
 * Slowly fill the mailbox alternating insertion and extractions.
 * Check that we get the expected number of iterations.
 */
static int test10(struct pspat_mailbox *mb, unsigned int entries)
{
	int maxn = entries - mb->line_entries;
	int arbitrary_inc = 5 * maxn / 43;
	int arbitrary_dec = 4 * maxn / 43;
	int expected_iterations;
	int n_iterations;
	int track;

	assert(arbitrary_inc != arbitrary_dec);
	expected_iterations = (int)(ceil((double)(maxn - arbitrary_inc) /
					 (double)(arbitrary_inc -
						  arbitrary_dec)));

	EXPECT_TRUE(pspat_mb_empty(mb));
	for (track = 0, n_iterations = 0; track + arbitrary_inc <= maxn;
	     n_iterations++) {
		int n = mb_fill_limit(mb, arbitrary_inc);
		EXPECT_EQ(n, arbitrary_inc);
		track += n;
		n = mb_drain_limit(mb, arbitrary_dec);
		EXPECT_EQ(n, arbitrary_dec);
		track -= n;
		EXPECT_TRUE(track >= 0);
		pspat_mb_clear(mb);
	}
	EXPECT_EQ(n_iterations, expected_iterations);

	return 0;
}

/**
 * Test function n. 11
 * Insert and extract items one at a time, so many times to
 * use the ring multiple times.
 */
static int test11(struct pspat_mailbox *mb, unsigned int entries)
{
	int maxn = entries - mb->line_entries;
	int arbitrary_num_cycles = maxn * 17;
	int i;

	for (i = 0; i < arbitrary_num_cycles; i++) {
		/* Insert 1. */
		int n = mb_fill_limit(mb, 1);
		EXPECT_EQ(n, 1);
		/* Drain at least 2, checking that we get one. */
		n = mb_drain_limit(mb, 2);
		EXPECT_EQ(n, 1);
		if ((i % maxn == maxn / 3) || (i % maxn == maxn * 2 / 3)) {
			/* Clear once every maxn items, in the
			 * middle of processing, making sure we
			 * never run out of slots. */
			pspat_mb_clear(mb);
		}
	}

	return 0;
}

/**
 * Test functions array.
 */
static testfunc_t tests[] = { test1, test2, test3, test4, test5, test6,
	test7, test8, test9, test10, test11, NULL
};

/**
 * Developer harness test.
 */
int main()
{
	const unsigned int entries = 512;
	const unsigned int line_size = 128;
	int i;

	// loop through available test functions
	for (i = 0; tests[i] != NULL; i++) {
		struct pspat_mailbox *mb;

		char test_name[PSPAT_MB_NAMSZ];

		assert(entries > 0);

		snprintf(test_name, sizeof(test_name), "test-%d", i + 1);

		mb = pspat_mb_new(test_name, entries, line_size);

		assert(mb);

		EXPECT_TRUE(pspat_mb_empty(mb));

		printf("Running test #%d ...\n", i + 1);

		// run the test
		tests[i] (mb, entries);

		printf("... [OK]\n");

		pspat_mb_delete(mb);
	}

	return 0;
}
