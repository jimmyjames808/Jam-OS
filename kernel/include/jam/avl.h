/* A balanced binary search tree (AVL), intrusive and augmented, for sets
 * that grow to tens of thousands of entries: an address space's mappings
 * (mm/aspace.c).
 *
 * The tree keeps no keys and calls no comparison: its user embeds a
 * struct avl_node in each entry, walks down from `root` itself to find a
 * place (each entry's left subtree holds what comes before it, its right
 * subtree what comes after), and links the new node there. So the order
 * is whatever its user says it is. Every change rebalances on the way back
 * to the root, so the height stays below 1.45 log2(n + 2): about 23 for
 * 65,536 entries.
 *
 * Augmenting: an entry may keep a summary of its whole subtree (the
 * largest free gap below it, the highest end of the intervals below it).
 * The tree calls `update` on a node whenever its children change, bottom
 * up, so the summary only has to be computed from the node itself and its
 * two children's summaries. When an entry's own data that the summary
 * reads changes, its user calls avl_propagate to bring the summaries above
 * it up to date.
 *
 * Nothing here allocates, sleeps or recurses; the tree has no lock of its
 * own (its user's lock guards it). Every operation is O(log n), except
 * avl_next and avl_prev, which are O(1) on average over a whole walk and
 * O(log n) at worst. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/list.h>   /* container_of */

struct avl_node {
    struct avl_node *parent;        /* NULL at the root */
    struct avl_node *left, *right;  /* before and after this node */
    int32_t          height;        /* of the subtree rooted here: 1 for a leaf */
};

struct avl_tree {
    struct avl_node *root;   /* NULL when empty */
    /* Recompute n's augmented summary from n and its children (either may
     * be NULL), or NULL when the tree keeps none. */
    void (*update)(struct avl_node *n);
};

static inline void avl_init(struct avl_tree *t, void (*update)(struct avl_node *n))
{
    t->root = NULL;
    t->update = update;
}

static inline bool avl_empty(const struct avl_tree *t)
{
    return t->root == NULL;
}

/* Link n (not in any tree) as the `right` (or left) child of parent, a
 * place that must be empty, or as the root of an empty tree (parent NULL),
 * then rebalance. */
void avl_link(struct avl_tree *t, struct avl_node *n, struct avl_node *parent, bool right);
/* Insert n (not in any tree) right after pos in the tree's order, or first
 * when pos is NULL. */
void avl_insert_after(struct avl_tree *t, struct avl_node *n, struct avl_node *pos);
/* Take n out of the tree and rebalance. n's fields are left undefined. */
void avl_erase(struct avl_tree *t, struct avl_node *n);
/* n's own data that its summary reads changed: recompute the summaries
 * from n up to the root. */
void avl_propagate(struct avl_tree *t, struct avl_node *n);

/* The first and last nodes in order, NULL when empty. */
struct avl_node *avl_first(const struct avl_tree *t);
struct avl_node *avl_last(const struct avl_tree *t);
/* The node after (before) n in order, NULL at the end. */
struct avl_node *avl_next(const struct avl_node *n);
struct avl_node *avl_prev(const struct avl_node *n);

/* For tests: walk the whole tree and check its links, its order (`before`
 * says whether a comes before b, NULL to skip), every height and every
 * balance. Returns the number of nodes, or -1 at the first fault found.
 * Iterative: O(n) time, no stack. */
int64_t avl_check(const struct avl_tree *t,
                  bool (*before)(const struct avl_node *a, const struct avl_node *b));
