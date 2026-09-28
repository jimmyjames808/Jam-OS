/* Intrusive circular doubly-linked list. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

struct list_node {
    struct list_node *next, *prev;
};

#define LIST_INIT(name) { &(name), &(name) }

static inline void list_init(struct list_node *h) { h->next = h->prev = h; }
static inline bool list_empty(const struct list_node *h) { return h->next == h; }

static inline void list_add(struct list_node *h, struct list_node *n)
{
    n->next = h->next;
    n->prev = h;
    h->next->prev = n;
    h->next = n;
}

static inline void list_del(struct list_node *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->next = n->prev = NULL;
}

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_first(h, type, member) container_of((h)->next, type, member)

static inline void list_add_tail(struct list_node *h, struct list_node *n)
{
    list_add(h->prev, n);
}
