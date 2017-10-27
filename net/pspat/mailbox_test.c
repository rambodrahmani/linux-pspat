#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

/*
 * Glue code to compile the in-kernel mailbox code in user-space.
 */
#define kfree(x) free(x)
#define kmalloc(x, y) malloc((x))
#define kzalloc(x, y) calloc(1, (x))
#define START_NEW_CACHELINE __attribute__((aligned(64)))
struct list_head {
};
/* The `const' in roundup() prevents gcc-3.3 from calling __divdi3 */
#define roundup(x, y) (					\
{							\
	const typeof(y) __y = y;			\
	(((x) + (__y - 1)) / __y) * __y;		\
}							\
)
#define INTERNODE_CACHE_BYTES	64
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#define prefetch(x)	__builtin_prefetch((x))
#define printk		printf
#define INIT_LIST_HEAD(x)
static inline __attribute__((const))
int is_power_of_2(unsigned long n)
{
	return (n != 0 && ((n & (n - 1)) == 0));
}
static unsigned long ilog2 (unsigned long x)
{
	int res = -1;
	while (x) { res++ ; x = x >> 1; }
	assert(res >= 0);
	return (unsigned long)res;
}
static inline void * ERR_PTR(long error)
{
	return (void *) error;
}
#define MAX_ERRNO	4095
#define IS_ERR_VALUE(x) unlikely((x) >= (unsigned long)-MAX_ERRNO)
static inline int IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((unsigned long)ptr);
}
/*
 * End of glue code.
 */

#include "mailbox.c"

typedef int (*testfunc_t)(struct pspat_mailbox *mb, unsigned entries,
			unsigned line_size);

#define EXPECT_TRUE(x) assert(!!(x))
#define EXPECT_FALSE(x) assert(!(x))
#define EXPECT_OK(x)   assert((x) == 0)
#define EXPECT_EQ(x, y) assert((x) == (y))

static int
test1(struct pspat_mailbox *mb, unsigned entries,
		 unsigned line_size)
{
	EXPECT_TRUE(pspat_mb_empty(mb));
	return 0;
}

static int
test2(struct pspat_mailbox *mb, unsigned entries,
		 unsigned line_size)
{
	EXPECT_TRUE(pspat_mb_empty(mb));
	EXPECT_OK(pspat_mb_insert(mb, /*value=*/mb+1));
	EXPECT_FALSE(pspat_mb_empty(mb));
	return 0;
}

static int
test3(struct pspat_mailbox *mb, unsigned entries,
		 unsigned line_size)
{
	void *v;

	EXPECT_TRUE(pspat_mb_empty(mb));
	EXPECT_OK(pspat_mb_insert(mb, /*value=*/mb+1));
	EXPECT_FALSE(pspat_mb_empty(mb));
	v = pspat_mb_extract(mb);
	EXPECT_EQ(mb+1, v);
	return 0;
}

static testfunc_t tests[] = { test1, test2, test3, NULL };

int main()
{
	const unsigned entries = 512;
	const unsigned line_size = 128;
	int i;

	for (i = 0; tests[i] != NULL; i++) {
		struct pspat_mailbox *mb;
		assert(line_size > 0 && entries >= line_size);
		mb = pspat_mb_new("test", entries, line_size);
		assert(mb);
		printf("Running test #%d ...\n", i+1);
		tests[i](mb, entries, line_size);
		printf("... [OK]\n");
		pspat_mb_delete(mb);
	}

	return 0;
}
