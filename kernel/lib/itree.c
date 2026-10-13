/* The interval tree (<jam/itree.h> has the model).
 *
 * The search, as one rule: the nodes that overlap [lo, hi) are the nodes
 * from the first one in order whose end lies above lo, up to (not
 * including) the first one whose first reaches hi, less those in between
 * whose end doesn't lie above lo. A subtree whose max_end doesn't lie
 * above lo holds none of them, so the walk never enters it; and since
 * order is by first, the walk stops for good at the first node whose first
 * reaches hi. Finding the first match goes down one path; each next match
 * is in the right subtree of the last (down one path) or above it (up one
 * path, to the first ancestor the last match lies left of). */
#include <jam/itree.h>

static struct itree_node *node_of(const struct avl_node *a)
{
    return a ? container_of(a, struct itree_node, avl) : NULL;
}

/* What n's max_end should be, from its end and its children's. */
static uint64_t max_end_of(const struct itree_node *n)
{
    const struct itree_node *l = node_of(n->avl.left), *r = node_of(n->avl.right);
    uint64_t m = n->end;
    if (l && l->max_end > m)
        m = l->max_end;
    if (r && r->max_end > m)
        m = r->max_end;
    return m;
}

static void update(struct avl_node *a)
{
    struct itree_node *n = node_of(a);
    n->max_end = max_end_of(n);
}

void itree_init(struct itree *t)
{
    avl_init(&t->avl, update);
}

void itree_insert(struct itree *t, struct itree_node *n)
{
    struct avl_node *parent = NULL;
    bool right = false;
    for (struct avl_node *a = t->avl.root; a;) {
        parent = a;
        right = n->first >= node_of(a)->first;
        a = right ? a->right : a->left;
    }
    avl_link(&t->avl, &n->avl, parent, right);
}

void itree_erase(struct itree *t, struct itree_node *n)
{
    avl_erase(&t->avl, &n->avl);
}

void itree_move(struct itree *t, struct itree_node *n, uint64_t first, uint64_t end)
{
    if (first == n->first) {
        n->end = end;   /* the order holds: only the summaries above n change */
        avl_propagate(&t->avl, &n->avl);
        return;
    }
    itree_erase(t, n);
    n->first = first;
    n->end = end;
    itree_insert(t, n);
}

/* The first node in n's subtree (whose max_end lies above lo) that
 * overlaps [lo, hi), or NULL if there is none in the whole tree past it:
 * the first node in order there whose end lies above lo, unless its first
 * reaches hi (then so does every node after it). */
static struct itree_node *subtree_search(struct itree_node *n, uint64_t lo, uint64_t hi)
{
    for (;;) {
        struct itree_node *l = node_of(n->avl.left);
        if (l && l->max_end > lo) {
            n = l;
            continue;
        }
        if (n->first >= hi)
            return NULL;
        if (n->end > lo)
            return n;
        struct itree_node *r = node_of(n->avl.right);
        if (!r || r->max_end <= lo)
            return NULL;   /* (not reached: n's subtree has an end above lo) */
        n = r;
    }
}

struct itree_node *itree_first(const struct itree *t, uint64_t lo, uint64_t hi)
{
    struct itree_node *root = node_of(t->avl.root);
    if (lo >= hi || !root || root->max_end <= lo)
        return NULL;
    return subtree_search(root, lo, hi);
}

struct itree_node *itree_next(const struct itree_node *n, uint64_t lo, uint64_t hi)
{
    const struct avl_node *a = &n->avl;
    for (;;) {
        /* After a (which may or may not overlap): its right subtree first. */
        struct itree_node *r = node_of(a->right);
        if (r && r->max_end > lo)
            return subtree_search(r, lo, hi);
        /* Then up to the first ancestor that a lies left of. */
        const struct avl_node *from;
        do {
            from = a;
            a = a->parent;
            if (!a)
                return NULL;
        } while (a->right == from);
        struct itree_node *p = node_of(a);
        if (p->first >= hi)
            return NULL;
        if (p->end > lo)
            return p;
    }
}

static bool before(const struct avl_node *a, const struct avl_node *b)
{
    return node_of(a)->first <= node_of(b)->first;
}

int64_t itree_check(const struct itree *t)
{
    int64_t count = avl_check(&t->avl, before);
    if (count < 0)
        return -1;
    for (const struct avl_node *a = avl_first(&t->avl); a; a = avl_next(a)) {
        const struct itree_node *n = node_of(a);
        if (n->first >= n->end || n->max_end != max_end_of(n))
            return -1;
    }
    return count;
}
