/* The AVL tree (<jam/avl.h> has the model).
 *
 * Every node keeps its subtree's height; the two children's heights differ
 * by at most one. A link or an erase changes heights only on the path from
 * the change to the root, so fix_up walks that path once, bottom up,
 * refreshing each node (height and summary) and rotating where a node's
 * children differ by two. It never stops early: the summaries above a
 * change may need refreshing even where the heights don't, and the path is
 * only O(log n) long. A rotation refreshes the two nodes it moves, the
 * lower one first. */
#include <jam/avl.h>

static int32_t height(const struct avl_node *n)
{
    return n ? n->height : 0;
}

/* n's height and summary from its children's. */
static void refresh(const struct avl_tree *t, struct avl_node *n)
{
    int32_t l = height(n->left), r = height(n->right);
    n->height = (l > r ? l : r) + 1;
    if (t->update)
        t->update(n);
}

/* Put `to` where `from` was under p (the root when p is NULL). */
static void replace_child(struct avl_tree *t, struct avl_node *p, const struct avl_node *from,
                          struct avl_node *to)
{
    if (!p)
        t->root = to;
    else if (p->left == from)
        p->left = to;
    else
        p->right = to;
    if (to)
        to->parent = p;
}

/* x's right child takes x's place, with x as its left child. Returns it. */
static struct avl_node *rotate_left(struct avl_tree *t, struct avl_node *x)
{
    struct avl_node *y = x->right;
    replace_child(t, x->parent, x, y);
    x->right = y->left;
    if (x->right)
        x->right->parent = x;
    y->left = x;
    x->parent = y;
    refresh(t, x);
    refresh(t, y);
    return y;
}

/* The mirror image: x's left child takes its place. */
static struct avl_node *rotate_right(struct avl_tree *t, struct avl_node *x)
{
    struct avl_node *y = x->left;
    replace_child(t, x->parent, x, y);
    x->left = y->right;
    if (x->left)
        x->left->parent = x;
    y->right = x;
    x->parent = y;
    refresh(t, x);
    refresh(t, y);
    return y;
}

/* n's children are balanced and refreshed: balance n (a single or double
 * rotation when its children's heights differ by two) and refresh it.
 * Returns the node now at n's place. */
static struct avl_node *balance(struct avl_tree *t, struct avl_node *n)
{
    int32_t b = height(n->left) - height(n->right);
    if (b > 1) {
        if (height(n->left->left) < height(n->left->right))
            rotate_left(t, n->left);
        return rotate_right(t, n);
    }
    if (b < -1) {
        if (height(n->right->right) < height(n->right->left))
            rotate_right(t, n->right);
        return rotate_left(t, n);
    }
    refresh(t, n);
    return n;
}

/* Balance and refresh every node from n up to the root. */
static void fix_up(struct avl_tree *t, struct avl_node *n)
{
    for (; n; n = n->parent)
        n = balance(t, n);
}

void avl_link(struct avl_tree *t, struct avl_node *n, struct avl_node *parent, bool right)
{
    n->left = n->right = NULL;
    n->height = 1;
    n->parent = parent;
    if (!parent)
        t->root = n;
    else if (right)
        parent->right = n;
    else
        parent->left = n;
    fix_up(t, n);
}

static struct avl_node *leftmost(struct avl_node *n)
{
    while (n->left)
        n = n->left;
    return n;
}

static struct avl_node *rightmost(struct avl_node *n)
{
    while (n->right)
        n = n->right;
    return n;
}

void avl_insert_after(struct avl_tree *t, struct avl_node *n, struct avl_node *pos)
{
    if (!pos) {
        if (t->root)
            avl_link(t, n, leftmost(t->root), false);
        else
            avl_link(t, n, NULL, false);
    } else if (!pos->right) {
        avl_link(t, n, pos, true);
    } else {
        avl_link(t, n, leftmost(pos->right), false);
    }
}

void avl_erase(struct avl_tree *t, struct avl_node *n)
{
    struct avl_node *from;   /* the lowest node whose subtree changed */
    if (n->left && n->right) {
        /* n's successor s (the leftmost of its right subtree, so it has no
         * left child) takes n's place. */
        struct avl_node *s = leftmost(n->right);
        if (s->parent == n) {
            from = s;
        } else {
            from = s->parent;
            replace_child(t, s->parent, s, s->right);
            s->right = n->right;
            s->right->parent = s;
        }
        replace_child(t, n->parent, n, s);
        s->left = n->left;
        s->left->parent = s;
    } else {
        from = n->parent;
        replace_child(t, n->parent, n, n->left ? n->left : n->right);
    }
    fix_up(t, from);
}

void avl_propagate(struct avl_tree *t, struct avl_node *n)
{
    if (!t->update)
        return;
    for (; n; n = n->parent)
        t->update(n);
}

struct avl_node *avl_first(const struct avl_tree *t)
{
    return t->root ? leftmost(t->root) : NULL;
}

struct avl_node *avl_last(const struct avl_tree *t)
{
    return t->root ? rightmost(t->root) : NULL;
}

struct avl_node *avl_next(const struct avl_node *n)
{
    if (n->right)
        return leftmost(n->right);
    while (n->parent && n == n->parent->right)
        n = n->parent;
    return n->parent;
}

struct avl_node *avl_prev(const struct avl_node *n)
{
    if (n->left)
        return rightmost(n->left);
    while (n->parent && n == n->parent->left)
        n = n->parent;
    return n->parent;
}

/* One node's own links, height and balance. */
static bool node_ok(const struct avl_node *n)
{
    if ((n->left && n->left->parent != n) || (n->right && n->right->parent != n))
        return false;
    int32_t l = height(n->left), r = height(n->right);
    return n->height == (l > r ? l : r) + 1 && l - r <= 1 && r - l <= 1;
}

int64_t avl_check(const struct avl_tree *t,
                  bool (*before)(const struct avl_node *a, const struct avl_node *b))
{
    if (t->root && t->root->parent)
        return -1;
    int64_t count = 0;
    const struct avl_node *last = NULL;
    /* A walk that never ends would be a cycle: no tree is this big. */
    for (const struct avl_node *n = avl_first(t); n; n = avl_next(n)) {
        if (!node_ok(n) || (before && last && !before(last, n)) || ++count > (1ll << 32))
            return -1;
        last = n;
    }
    return count;
}
