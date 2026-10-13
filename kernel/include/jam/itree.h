/* An interval tree: a set of ranges [first, end) of 64-bit numbers that
 * answers "which of them overlap [lo, hi)?" by visiting only those (and
 * O(log n) others for each), however many there are. A VMO's reverse map
 * (object/vmo.c): which user mappings cover a run of its pages.
 *
 * It is <jam/avl.h>'s tree ordered by `first` (ties in no particular order),
 * each node keeping the highest `end` in its subtree (`max_end`): the
 * classic augmentation (Cormen et al., "Introduction to Algorithms",
 * 14.3). A search goes down only into subtrees whose max_end lies above
 * lo, and stops at the first node whose `first` reaches hi: everything
 * after it in order starts there or later.
 *
 * Intrusive, like the AVL tree: its user embeds a struct itree_node and
 * sets first and end before inserting; a node's range changes only through
 * itree_move. Nothing allocates, sleeps or recurses; the tree has no lock
 * of its own. Insert, erase and move are O(log n); a whole search is
 * O((k + 1) log n) for k overlapping nodes at worst, and less in practice
 * (a step to the next match is usually a few links). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/avl.h>

struct itree_node {
    struct avl_node avl;
    uint64_t        first, end;   /* the range [first, end); first < end */
    uint64_t        max_end;      /* the highest end in this node's subtree */
};

struct itree {
    struct avl_tree avl;
};

void itree_init(struct itree *t);

static inline bool itree_empty(const struct itree *t)
{
    return avl_empty(&t->avl);
}

/* Add n (not in the tree) with its range as set. */
void itree_insert(struct itree *t, struct itree_node *n);
/* Take n out. Its fields but first and end are left undefined. */
void itree_erase(struct itree *t, struct itree_node *n);
/* Give n (in the tree) the range [first, end): O(log n). */
void itree_move(struct itree *t, struct itree_node *n, uint64_t first, uint64_t end);

/* The first node in order whose range overlaps [lo, hi), or NULL. */
struct itree_node *itree_first(const struct itree *t, uint64_t lo, uint64_t hi);
/* The node after n in order that overlaps [lo, hi), or NULL; n is one
 * itree_first or itree_next returned for the same [lo, hi). The tree must
 * not change between the two calls. So every overlapping node, in order:
 *     for (struct itree_node *n = itree_first(t, lo, hi); n; n = itree_next(n, lo, hi)) */
struct itree_node *itree_next(const struct itree_node *n, uint64_t lo, uint64_t hi);

/* For tests: avl_check, the order by first, every range non-empty, and
 * every max_end. Returns the number of nodes, or -1 at the first fault.
 * O(n). */
int64_t itree_check(const struct itree *t);
