/* A pager's page cache (pager_cache.c), and nothing else includes this but
 * pager.c: the budget and the order pages are evicted in.
 *
 * A clean page (every page of a read-only pager VMO, and a writable one's
 * not stored to since it arrived or was written back) holds the disk's
 * bytes, so the kernel may drop it at any time and the next access asks the
 * pager again. A pager's cache holds every resident page of every VMO the
 * pager made, at most max_pages of them (the budget): a supply that would
 * pass the budget first evicts the oldest clean page; one whose page the
 * job (or the machine) can't pay for evicts the oldest clean pages until
 * it can, or until none is left (ERR_NO_MEMORY). Eviction is a decommit of
 * one page (vmo_pager_evict): zapped from every mapping under the VMO
 * lock, freed after the shootdown. No global reclaim: a pager evicts only
 * its own pages. Dirty and writing pages (<jam/pager.h>'s "Writable pager
 * VMOs") are never evicted: they leave only by a shrink, a detach, or
 * once written back, clean again.
 *
 * The lists. A resident page is on one of its cache's two lists through
 * its struct page's `node`, which is free while the page is in a VMO's
 * table (the free lists and gathers use it only before the page is
 * published and after it is taken out): `pages`, the clean ones, oldest
 * supplied first (the evictors' list), or `dirty`, the dirty and writing
 * ones, oldest dirtied first (write-back's); `node.next` is NULL once it is
 * off both. Its struct page's `private` points at its record (struct
 * pager_rec: the VMO, the index, the state and when it was dirtied, 24
 * bytes, charged to the pager's job as message bytes), which says where an
 * evictor finds it. A record lives exactly as long as its page is in its
 * table: whoever takes the page out frees it (pager_cache_remove_vlocked).
 * The state and the list a page is on change together, under the VMO lock
 * and this one.
 *
 * Counting. `resident` is the pages on the lists plus the slots reserved
 * for pages being published (pager_cache_reserve), and passes max_pages
 * when more supplies publish at once than the budget has clean pages to
 * evict (all of it reserved or dirty), or when a page an evictor popped is
 * dirtied before the evictor reaches it (the evictor then leaves it, and
 * the store counts it again). A page leaves the count when it leaves the
 * lists: taken out of its table, or popped by an evictor whose reservation
 * it becomes. That evictor takes it out of its table before it publishes
 * the page it made room for, so the pages in the tables stay within the
 * budget too; the one exception is a page of a VMO being destroyed, which
 * no evictor can reach (it goes with its VMO a moment later). `ndirty`
 * counts the pages on `dirty`.
 *
 * Lock: "pager cache" (`lock`), taken under "vmo" (publishing and taking a
 * page out) and alone (reserving, evicting), never the other way round: an
 * evictor drops it before it takes the VMO lock, holding a VMO reference
 * across. It is independent of the pager's object lock (never nested with
 * it). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/list.h>
#include <jam/spinlock.h>
#include <jam/status.h>

struct job;
struct page;
struct pager_rec;   /* a resident page's record (pager_cache.c) */
struct vmo;

struct pager_cache {
    spinlock_t       lock;        /* "pager cache": everything below */
    struct job      *job;         /* pays for the records (the pager's; no reference of its own) */
    uint64_t         max_pages;   /* the budget, > 0; fixed */
    uint64_t         resident;    /* pages on `pages` and `dirty` + reserved slots */
    uint64_t         ndirty;      /* pages on `dirty` */
    struct list_node pages;       /* struct page (via node): clean pages, oldest supplied first */
    struct list_node dirty;       /* struct page (via node): dirty and writing pages, oldest
                                     dirtied first */
};

/* An empty cache of max_pages (> 0) for a pager paid for by job. */
void pager_cache_init(struct pager_cache *c, struct job *job, uint64_t max_pages);

/* Publish p (a fresh page holding its bytes, the caller's reference,
 * consumed whatever happens; `checked` as for vmo_pager_publish) as page
 * idx of pager VMO v, inside c's budget: a reserved slot, the oldest
 * clean page evicted for it if the budget is full, and more of the oldest
 * evicted while the job or memory can't pay for p. OK if published or
 * present already (a second answer: p dropped, nothing evicted);
 * ERR_PEER_CLOSED if v is detached; ERR_OUT_OF_RANGE past its size; ERR_IO
 * if v is verified and p doesn't match; ERR_NO_MEMORY if it can't be paid
 * for with no clean page left. Interrupts on, no spinlock held. */
status_t pager_cache_publish(struct pager_cache *c, struct vmo *v, uint64_t idx, struct page *p,
                             bool checked);

/* Evict c's oldest clean page (memory pressure). false if c holds none. A
 * page of a VMO being destroyed leaves the list without being reached (its
 * VMO's destruction frees it), and one dirtied since it was popped stays
 * (vmo_pager_evict). Interrupts on, no spinlock held. */
bool pager_cache_evict_oldest(struct pager_cache *c);

/* The VMO lock held, p just published on a slot that pager_cache_publish
 * reserved, rec its record (which says where): p joins the list, newest. */
void pager_cache_add_vlocked(struct pager_cache *c, struct page *p, struct pager_rec *rec);

/* The VMO lock held (or the VMO has no reference left): p leaves its
 * table. It leaves its list and the count (unless an evictor popped it
 * already) and its record is freed. */
void pager_cache_remove_vlocked(struct pager_cache *c, struct page *p);

/* The VMO lock held, p in its table: its state (PAGER_PAGE_*, 0: clean)
 * and when it was first dirtied since it was last clean (uptime ns). */
unsigned pager_cache_state_vlocked(const struct page *p);
uint64_t pager_cache_dirtied_vlocked(const struct page *p);

/* The VMO lock held, p in its table: a store. Clean: dirty now, onto the
 * dirty list's tail stamped with the time (off the cache's list, or, if an
 * evictor popped it, counted again). Writing: writing and dirty, where it
 * is. Dirty: nothing. */
void pager_cache_store_vlocked(struct pager_cache *c, struct page *p);

/* Pages counted against the budget right now, and those dirty or writing
 * (tests and statistics). */
uint64_t pager_cache_resident(struct pager_cache *c);
uint64_t pager_cache_ndirty(struct pager_cache *c);
