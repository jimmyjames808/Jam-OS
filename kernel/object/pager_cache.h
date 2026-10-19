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
 * The lists. A resident page is on one of its cache's three lists through
 * its struct page's `node`, which is free while the page is in a VMO's
 * table (the free lists and gathers use it only before the page is
 * published and after it is taken out): `pages`, the clean ones, oldest
 * supplied (or written back) first, the evictors' list; `dirty`, the dirty
 * ones not being written back, oldest dirtied first (write-back's: the
 * age write-back and the background request start at its head); or
 * `writing`, those being written back (writing, and dirty as well if
 * stored to since), in the order their write-backs began. `node.next` is
 * NULL once it is off all three. Its struct page's `private` points at its
 * record (struct pager_rec: the VMO, the index, the state and when it was
 * dirtied, 24 bytes, charged to the pager's job as message bytes), which
 * says where an evictor (or write-back) finds it. A record lives exactly as long as its page is in its
 * table: whoever takes the page out frees it (pager_cache_remove_vlocked).
 * The state and the list a page is on change together, under the VMO lock
 * and this one.
 *
 * Counting. `resident` is the pages on the lists plus the slots reserved
 * for pages being published (reserve), and `tables` the table pages of
 * the cache's VMOs (a leaf a 2 MiB, a mid table a GiB, vmo.c's), which the
 * budget holds too: the budget is full when the two together reach
 * max_pages. A pager VMO's table is made only when a page is published
 * under it and freed when its last page leaves (vmo.c's prune_locked), so
 * a leaf always holds a page and a mid table a leaf, and a page that needs
 * new tables has room made for them before it is published (two at most:
 * a pager VMO fits the inline root). A budget under 3 holds no page at
 * all. The sum passes max_pages when more supplies publish at once than
 * the budget has clean pages to evict (all of it reserved, dirty or tables
 * of dirty pages), for a moment when a racing supply (or an eviction for
 * memory pressure, which emptied the leaf the page goes in) made tables
 * after room was made (the publisher then evicts again), or when a page an
 * evictor popped is dirtied before the evictor reaches it (the evictor
 * then leaves it, and the store counts it again, until a supply makes room
 * again). A page leaves the count when it
 * leaves the lists: taken out of its table, or popped by an evictor whose
 * reservation it becomes. That evictor takes it out of its table before it
 * publishes the page it made room for, so the pages in the tables stay
 * within the budget too; the one exception is a page of a VMO being
 * destroyed, which no evictor can reach (it goes with its VMO a moment
 * later). `ndirty`
 * counts the pages on `dirty` and `writing`, `nwriting` those on
 * `writing`. A store makes a clean page dirty only while `ndirty` is under
 * the pager's dirty limit (a quarter of the budget), checked and counted
 * in one hold of this lock, so stores to two VMOs can't pass it together:
 * three quarters of the budget always stay for clean pages and tables (a
 * quarter at least once the dirty pages' own tables are counted: two at
 * most each), and a supply finds one to evict (a popped page aside).
 *
 * Age (`dirtied`, the order of `dirty`): when a clean page's first store
 * made it dirty; for a page stored to while it was being written back,
 * when that write-back ended (it is dirty from then on, so a page stored
 * to all the time is written back once per age, not on every look); for a
 * page whose write-back was cut off (its pager died, or the copy failed),
 * its age from before, made no younger than the head of `dirty`, where it
 * goes back.
 *
 * Kept: a page whose write-back failed is clean (Q1's answer: it doesn't
 * pin the dirty limit, and may be evicted like any clean page), but its
 * bytes are the store's, not the disk's, so it is marked kept until it
 * leaves its table or is stored to again: pager_dirty's PEEK copies it
 * with the dirty ones, so a filesystem's read shows what every mapping
 * shows (one cache) until the page is evicted. A kept page is counted as
 * a clean one; its state (pager_cache_state_vlocked) is 0.
 *
 * Lock: "pager cache" (`lock`), taken under "vmo" (publishing and taking a
 * page out, making and freeing a table) and alone (reserving, evicting,
 * the tables of a VMO being destroyed), never the other way round: an
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
    uint64_t         resident;    /* pages on the lists + reserved slots */
    uint64_t         tables;      /* its VMOs' table pages (in the budget too) */
    uint64_t         ndirty;      /* pages on `dirty` and `writing` */
    uint64_t         nwriting;    /* pages on `writing` */
    struct list_node pages;       /* struct page (via node): clean pages, oldest supplied first */
    struct list_node dirty;       /* struct page (via node): dirty pages not being written
                                     back, oldest dirtied first */
    struct list_node writing;     /* struct page (via node): pages being written back */
};

/* An empty cache of max_pages (> 0) for a pager paid for by job. */
void pager_cache_init(struct pager_cache *c, struct job *job, uint64_t max_pages);

/* Publish p (a fresh page holding its bytes, the caller's reference,
 * consumed whatever happens; `checked` as for vmo_pager_publish) as page
 * idx of pager VMO v, inside c's budget: a reserved slot, the oldest
 * clean page evicted for it if the budget is full, more of the oldest
 * for the tables it needs, and more while the job or memory can't pay for
 * p. OK if published or present already (a second answer: p dropped,
 * nothing evicted);
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

/* The VMO lock held (or the VMO has no reference left): a table page of
 * one of c's VMOs was made (`made`) or freed. */
void pager_cache_table_vlocked(struct pager_cache *c, bool made);

/* The VMO lock held, p in its table: its state (PAGER_PAGE_*, 0: clean),
 * whether it is kept ("Kept" above), and its age (uptime ns: "Age"
 * above). */
unsigned pager_cache_state_vlocked(const struct page *p);
bool     pager_cache_kept_vlocked(const struct page *p);
uint64_t pager_cache_dirtied_vlocked(const struct page *p);

/* The VMO lock held, p in its table: a store. Clean: dirty now, onto the
 * dirty list's tail stamped with the time (off the cache's list, or, if an
 * evictor popped it, counted again), unless c has `limit` pages dirty or
 * writing already: then ERR_NO_RESOURCES and nothing changes (the store
 * waits, the dirty limit of <jam/pager.h>). Writing: writing and dirty,
 * where it is. Dirty: nothing. OK but at the limit. */
status_t pager_cache_store_vlocked(struct pager_cache *c, struct page *p, uint64_t limit);

/* The VMO lock held, p dirty and not writing: its write-back begins
 * (writing now, onto `writing`). */
void pager_cache_begin_vlocked(struct pager_cache *c, struct page *p);
/* The VMO lock held, p writing: its write-back ended (`failed`: with an
 * error). Clean (onto the clean list, newest, kept if `failed`: true),
 * or, stored to since, dirty (onto `dirty`'s tail, its age counted from
 * now: false). */
bool pager_cache_written_vlocked(struct pager_cache *c, struct page *p, bool failed);
/* The VMO lock held, p writing: its write-back won't end (the pager died,
 * or the copy out failed): dirty again, at `dirty`'s head. */
void pager_cache_unbegin_vlocked(struct pager_cache *c, struct page *p);

/* The oldest dirty page not being written back (`dirty`'s head), or with
 * `writing` the first page being written back: true, *v a reference to
 * its VMO, *idx its index and *dirtied its age; false if there is none.
 * No lock held: by the time the caller locks the VMO the page may have
 * moved on (it looks again). */
bool pager_cache_first(struct pager_cache *c, bool writing, struct vmo **v, uint64_t *idx,
                       uint64_t *dirtied);

/* Pages counted against the budget right now (the tables aside), those
 * dirty or writing, those writing, and the tables (tests and
 * statistics). */
uint64_t pager_cache_resident(struct pager_cache *c);
uint64_t pager_cache_tables(struct pager_cache *c);
uint64_t pager_cache_ndirty(struct pager_cache *c);
uint64_t pager_cache_nwriting(struct pager_cache *c);
