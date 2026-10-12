/* avlcheck: the kernel's AVL tree (kernel/lib/avl.c, the same file) on
 * the Mac, against a sorted array doing the same thing. `make check` runs
 * it.
 *
 * Each round makes random inserts (at a random place in order, with
 * avl_insert_after) and erases (of a random entry), changes some entries'
 * values (with avl_propagate after), and checks after every step: the
 * links, heights and balance (avl_check), that an in-order walk both ways
 * gives the array, and each node's summaries (its subtree's size and
 * largest value) against a count from scratch. Then it empties the tree
 * from the front, from the back and at random. Exit 0 when all of it
 * holds; otherwise the first difference and exit 1. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <jam/avl.h>

struct ent {
    struct avl_node node;
    uint64_t        value;     /* the entry's own data the summary reads */
    uint64_t        size;      /* nodes in the subtree */
    uint64_t        max;       /* largest value in the subtree */
};

#define MAXN 1000

static struct ent *ref[MAXN];   /* the entries in tree order */
static unsigned nref;
static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint64_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static struct ent *E(const struct avl_node *n)
{
    return n ? container_of(n, struct ent, node) : NULL;
}

static void update(struct avl_node *n)
{
    struct ent *e = E(n), *l = E(n->left), *r = E(n->right);
    e->size = 1 + (l ? l->size : 0) + (r ? r->size : 0);
    e->max = e->value;
    if (l && l->max > e->max)
        e->max = l->max;
    if (r && r->max > e->max)
        e->max = r->max;
}

static int fail(const char *what, unsigned step)
{
    printf("avlcheck: FAIL at step %u: %s\n", step, what);
    exit(1);
}

/* The summaries from scratch, checked node by node (post-order). */
static void check_summary(const struct avl_node *n, uint64_t *size, uint64_t *max, unsigned step)
{
    if (!n) {
        *size = 0;
        *max = 0;
        return;
    }
    uint64_t ls, lm, rs, rm;
    check_summary(n->left, &ls, &lm, step);
    check_summary(n->right, &rs, &rm, step);
    *size = 1 + ls + rs;
    *max = E(n)->value;
    if (lm > *max)
        *max = lm;
    if (rm > *max)
        *max = rm;
    if (E(n)->size != *size || E(n)->max != *max)
        fail("a summary is stale", step);
}

static void check(struct avl_tree *t, unsigned step)
{
    if (avl_check(t, NULL) != (int64_t)nref)
        fail("avl_check", step);
    unsigned i = 0;
    for (struct avl_node *n = avl_first(t); n; n = avl_next(n), i++)
        if (i >= nref || E(n) != ref[i])
            fail("forward walk differs", step);
    if (i != nref)
        fail("forward walk short", step);
    for (struct avl_node *n = avl_last(t); n; n = avl_prev(n))
        if (i == 0 || E(n) != ref[--i])
            fail("backward walk differs", step);
    uint64_t size, max;
    check_summary(t->root, &size, &max, step);
    if (size != nref)
        fail("size", step);
    /* Height bound: 1.45 log2(n + 2). */
    int32_t h = t->root ? t->root->height : 0, bound = 0;
    for (uint64_t x = nref + 2; x > 1; x >>= 1)
        bound++;
    if (h > bound * 3 / 2 + 2)
        fail("too tall", step);
}

static void insert_at(struct avl_tree *t, unsigned pos, unsigned step)
{
    struct ent *e = calloc(1, sizeof(*e));
    e->value = next_rand() % 1000000;
    avl_insert_after(t, &e->node, pos ? &ref[pos - 1]->node : NULL);
    for (unsigned i = nref; i > pos; i--)
        ref[i] = ref[i - 1];
    ref[pos] = e;
    nref++;
    check(t, step);
}

static void erase_at(struct avl_tree *t, unsigned pos, unsigned step)
{
    struct ent *e = ref[pos];
    avl_erase(t, &e->node);
    for (unsigned i = pos; i + 1 < nref; i++)
        ref[i] = ref[i + 1];
    nref--;
    free(e);
    check(t, step);
}

int main(void)
{
    struct avl_tree t;
    avl_init(&t, update);
    unsigned step = 0;
    for (unsigned round = 0; round < 6; round++) {
        unsigned target = round % 2 ? 40 : MAXN - 1;
        /* Grow (mostly inserting), then shrink (mostly erasing). */
        for (unsigned k = 0; k < 3 * MAXN; k++, step++) {
            bool grow = nref < target ? next_rand() % 4 != 0 : next_rand() % 4 == 0;
            if (grow && nref < MAXN - 1) {
                unsigned mode = next_rand() % 3;   /* front, back, anywhere */
                unsigned pos = mode == 0 ? 0 : mode == 1 ? nref : next_rand() % (nref + 1);
                insert_at(&t, pos, step);
            } else if (nref) {
                erase_at(&t, next_rand() % nref, step);
            }
            if (nref && next_rand() % 8 == 0) {
                struct ent *e = ref[next_rand() % nref];
                e->value = next_rand() % 2000000;
                avl_propagate(&t, &e->node);
                check(&t, step);
            }
        }
    }
    /* Empty it: from the front, refill, from the back, refill, at random. */
    for (unsigned way = 0; way < 3; way++, step++) {
        while (nref < 500)
            insert_at(&t, next_rand() % (nref + 1), step);
        while (nref)
            erase_at(&t, way == 0 ? 0 : way == 1 ? nref - 1 : next_rand() % nref, step);
        if (t.root)
            fail("not empty", step);
    }
    printf("avlcheck: %u steps, all consistent\n", step);
    return 0;
}
