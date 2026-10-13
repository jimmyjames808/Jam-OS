/* itreecheck: the kernel's interval tree (kernel/lib/itree.c over
 * kernel/lib/avl.c, the same files) on the Mac, against a plain array of
 * the same ranges searched one by one. `make check` runs it.
 *
 * Each round inserts, erases and moves ranges at random (narrow and wide
 * ones, many starting at the same place, some reaching the top of the
 * 64-bit range), checking the tree after every step (itree_check: the AVL
 * links and balance, the order, every max_end), and asks random questions
 * [lo, hi), from empty and single-number ones to everything: the search
 * (itree_first, then itree_next) must return exactly the ranges that
 * overlap, each once, in order of first. Then it empties the tree. Exit 0
 * when all of it holds; otherwise the first difference and exit 1. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <jam/itree.h>

struct ent {
    struct itree_node node;
    bool              in;     /* in the tree */
    bool              seen;   /* returned by the current search */
};

#define MAXN 600

static struct ent ents[MAXN];
static unsigned nin;
static uint64_t rng = 0x2545f4914f6cdd1dull;

static uint64_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static int fail(const char *what, unsigned step)
{
    printf("itreecheck: FAIL at step %u: %s\n", step, what);
    exit(1);
}

/* A random range: mostly small numbers (so ranges pile up and share
 * starts), sometimes huge ones up to the top of the type. */
static void random_range(uint64_t *first, uint64_t *end)
{
    unsigned kind = next_rand() % 8;
    uint64_t a, len;
    if (kind == 0) {   /* near the top: end may be UINT64_MAX */
        len = 1 + next_rand() % 1000;
        a = UINT64_MAX - len - next_rand() % 1000;
    } else if (kind == 1) {   /* wide */
        a = next_rand() % 4096;
        len = 1 + next_rand() % (1ull << 40);
    } else {
        a = next_rand() % (kind == 2 ? 16 : 4096);
        len = 1 + next_rand() % (kind == 3 ? 512 : 16);
    }
    *first = a;
    *end = a + len;
}

static void query(const struct itree *t, uint64_t lo, uint64_t hi, unsigned step)
{
    for (unsigned i = 0; i < MAXN; i++)
        ents[i].seen = false;
    uint64_t last = 0;
    unsigned found = 0;
    for (struct itree_node *n = itree_first(t, lo, hi); n; n = itree_next(n, lo, hi)) {
        struct ent *e = container_of(n, struct ent, node);
        if (!e->in || e->seen)
            fail("a range returned twice, or one not in the tree", step);
        if (!(n->first < hi && n->end > lo))
            fail("a range returned that doesn't overlap", step);
        if (found && n->first < last)
            fail("returned out of order", step);
        e->seen = true;
        last = n->first;
        found++;
    }
    for (unsigned i = 0; i < MAXN; i++) {
        const struct ent *e = &ents[i];
        bool overlaps = e->in && lo < hi && e->node.first < hi && e->node.end > lo;
        if (overlaps != e->seen)
            fail("an overlapping range not returned", step);
    }
}

static void random_query(const struct itree *t, unsigned step)
{
    uint64_t lo, hi;
    switch (next_rand() % 6) {
    case 0:   /* everything */
        lo = 0;
        hi = UINT64_MAX;
        break;
    case 1:   /* empty */
        lo = hi = next_rand() % 4096;
        break;
    case 2:   /* one number */
        lo = next_rand() % 4200;
        hi = lo + 1;
        break;
    case 3:   /* at the top */
        lo = UINT64_MAX - next_rand() % 3000;
        hi = UINT64_MAX;
        break;
    default:
        random_range(&lo, &hi);
        break;
    }
    query(t, lo, hi, step);
}

static void check(const struct itree *t, unsigned step)
{
    if (itree_check(t) != (int64_t)nin)
        fail("itree_check", step);
}

static void step_once(struct itree *t, unsigned target, unsigned step)
{
    struct ent *e = &ents[next_rand() % MAXN];
    bool grow = nin < target ? next_rand() % 4 != 0 : next_rand() % 4 == 0;
    if (!e->in && grow) {
        random_range(&e->node.first, &e->node.end);
        itree_insert(t, &e->node);
        e->in = true;
        nin++;
    } else if (e->in && next_rand() % 3 == 0) {
        uint64_t first, end;
        random_range(&first, &end);
        if (next_rand() % 2 && e->node.first <= UINT64_MAX - (end - first)) {
            end = e->node.first + (end - first);   /* the end alone moves */
            first = e->node.first;
        }
        itree_move(t, &e->node, first, end);
    } else if (e->in && !grow) {
        itree_erase(t, &e->node);
        e->in = false;
        nin--;
    }
    check(t, step);
    random_query(t, step);
}

int main(void)
{
    struct itree t;
    itree_init(&t);
    if (itree_first(&t, 0, UINT64_MAX))
        fail("an empty tree found something", 0);
    unsigned step = 0;
    for (unsigned round = 0; round < 6; round++) {
        unsigned target = round % 2 ? 30 : MAXN - 50;
        for (unsigned k = 0; k < 6 * MAXN; k++, step++)
            step_once(&t, target, step);
    }
    for (unsigned i = 0; i < MAXN; i++, step++) {
        if (ents[i].in) {
            itree_erase(&t, &ents[i].node);
            ents[i].in = false;
            nin--;
        }
        check(&t, step);
        if (i % 16 == 0)
            random_query(&t, step);
    }
    if (!itree_empty(&t))
        fail("not empty", step);
    printf("itreecheck: %u steps, every search exact\n", step);
    return 0;
}
