/* Pagers: a process (fat) supplies the pages of VMOs on demand.
 *
 * A pager (OBJ_PAGER) makes pager VMOs, each under a key it chooses. A
 * pager VMO is a paged VMO whose absent pages are asked for instead of
 * zero-filled: a reader of an absent page (vmo_read; a fault, once the
 * fault path waits) queues a request in the pager, or joins the one that
 * covers the page already, and waits for it. The pager reads requests with
 * pager_next and answers with pager_supply (the kernel copies the bytes
 * into fresh pages and publishes them) or pager_op (a failure, or the VMO
 * cut off). Requests stay in the pager object until answered: a pager
 * process that dies leaves them there, and its successor takes the ones it
 * had been given again (PAGER_NEXT_AGAIN). Nobody waits forever: a request
 * not answered within the pager's answer time fails ERR_TIMED_OUT, and a
 * waiter's kill ends its wait.
 *
 * A pager VMO refuses what would put a page in it other than the pager's
 * (vmo_write, vmo_commit, writable mappings but a writable VMO's), take
 * one away (vmo_decommit, vmo_set_size), make a device or the kernel wait
 * for it (vmo_pin, vmo_map_kernel), keep it (VMAR_KEPT_ONLY) or run it
 * unchecked (vmo_seal: its code runs only through vmo_make_exec_verified,
 * which checks each page as it arrives, <jam/vmo_verify.h>).
 *
 * Writable pager VMOs (PAGER_VMO_WRITABLE, docs/WRITEMAP-PLAN.md): user
 * mappings may write them, and nothing else. Each page present is clean
 * (the disk's bytes, as far as the pager knows), dirty (stored to since
 * its last write-back began) or writing (that write-back hasn't ended;
 * dirty as well if stored to since): PAGER_PAGE_* in its cache record,
 * changed under the VMO lock. A page is mapped writable only while dirty,
 * so the first store to a clean or writing page faults (vmo_fault_map,
 * the one path that installs entries) and makes it dirty: dirty bits by
 * write-protection, not the page tables' D bits. Dirty and writing pages
 * are never evicted, and while a VMO has any it holds a reference to
 * itself (the hold): it outlives its last handle and mapping, so its
 * PAGER_REQ_GONE comes only once it is clean and PAGER_VMO_FIND finds it
 * meanwhile. pager_dirty copies dirty pages out (PEEK) and pager_write
 * copies bytes into present pages, both on page references with no lock
 * held. Write-back (pager_dirty's BEGIN, PAGER_OP_WRITTEN, the requests)
 * and the dirty limit come with WRITEMAP's WK2 and WK3.
 *
 * The page cache: a pager's VMOs hold at most its budget of pages
 * (max_pages) at once. A clean page may be dropped at any time: the kernel
 * drops the oldest supplied clean page whenever a supply needs room, or
 * memory or the job runs short, and the next access asks the pager again
 * (kernel/object/pager_cache.h). The pager drops pages itself with
 * PAGER_OP_INVALIDATE (the file changed: clean pages only) and
 * PAGER_OP_SET_SIZE (it shrank).
 *
 * Objects and lifetimes: the pager holds no reference to its VMOs: each
 * has a struct pager_vmo (pager.c) holding a reference to the pager, and
 * the pager lists the live ones by key, without a reference (a lookup
 * takes one with kobject_tryref). When a VMO linked to its key dies (no
 * handle, no mapping), the pager gets a PAGER_REQ_GONE for the key, which
 * keeps the key taken until the pager answers it (pager_op DETACH). A
 * request holds a reference to the pager; each of its waiters one to the
 * request (and to the VMO it waits on).
 *
 * Lock order: "vmo" -> "pager" (the object lock of the pager: its keys,
 * queues, requests and signals) -> "pager request" (each request's wait
 * queue) and the observers' locks (port queues, run queues). A request is
 * joined and a page published under the VMO lock, then the pager's; the
 * pager side never takes a VMO lock under its own. The cache's lock,
 * "pager cache", also nests under "vmo" only (never with "pager").
 *
 * Charges: the pager is one JOB_LIMIT_HANDLES unit of its creator's job,
 * and so is each of its VMOs' links; the VMOs (struct, tables, pages) are
 * charged to that job as any VMO, and so is each resident page's record
 * in the cache (24 message bytes); a request is charged to the job of the
 * thread that made it, as message bytes, until it is answered. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/handle.h>
#include <jam/list.h>
#include <jam/status.h>

struct job;
struct page;
struct pager;        /* embeds struct kobject first, type OBJ_PAGER */
struct pager_rec;    /* a resident page's record in the cache (pager_cache.c) */
struct pager_req;    /* one request (pager.c) */
struct pager_vmo;    /* a pager VMO's link to its pager (pager.c) */
struct vmo;

static inline struct kobject *pager_kobject(struct pager *pg) { return (struct kobject *)pg; }
/* NULL unless obj is a pager. */
static inline struct pager *pager_from_kobject(struct kobject *obj)
{
    return obj && obj->type == OBJ_PAGER ? (struct pager *)obj : NULL;
}

/* Where pager_supply's and pager_write's bytes come from: kernel memory,
 * or memory of the current process (copied with copy_from_user, no lock
 * held). */
struct pager_src {
    uint64_t addr;   /* a kernel pointer, or a user address */
    bool     user;   /* addr is a user address */
};

/* Where pager_dirty's bytes go: the same (copied with copy_to_user). */
struct pager_dst {
    uint64_t addr;   /* a kernel pointer, or a user address */
    bool     user;   /* addr is a user address */
};

/* What each resident page's record in its pager's cache costs the
 * pager's job, as message bytes (pager_cache.c's struct pager_rec). */
#define PAGER_RECORD_BYTES 24u

/* A writable pager VMO's page's state (its cache record's): 0 is clean. */
#define PAGER_PAGE_DIRTY   1u   /* stored to since its last write-back began */
#define PAGER_PAGE_WRITING 2u   /* its write-back began and hasn't ended */

/* ---- the object layer (kernel/object/pager.c) ------------------------------ */

/* A pager whose VMOs may hold max_pages pages at once (> 0: its budget,
 * beyond which the oldest are evicted) and whose requests wait answer_ns at most
 * (PAGER_ANSWER_MIN_NS .. _MAX_NS), charged with its VMOs to job (NULL:
 * nothing charged). ERR_INVALID_ARGS for max_pages 0, ERR_OUT_OF_RANGE for
 * answer_ns, ERR_NO_RESOURCES / ERR_NO_MEMORY if the job refuses it. The
 * caller gets the only reference. */
status_t pager_create(struct job *job, uint64_t max_pages, uint64_t answer_ns,
                      struct pager **out);
/* A pager VMO of size bytes (rounded up; VMO_SMALL_MAX_SIZE at most, else
 * ERR_OUT_OF_RANGE) under key, every page absent; the caller gets the only
 * reference. ERR_ALREADY_EXISTS if key names a live VMO; a key whose VMO
 * is gone but whose PAGER_REQ_GONE has no answer yet is taken over (the
 * GONE withdrawn, delivered or not). ERR_BAD_STATE once the pager's last
 * handle is gone. Thread context. */
status_t pager_create_vmo(struct pager *pg, uint64_t key, uint64_t size, struct vmo **out);
/* pager_create_vmo with flags: PAGER_VMO_WRITABLE, a writable one
 * (ERR_INVALID_ARGS if the budget is under PAGER_WRITABLE_MIN_PAGES);
 * PAGER_VMO_FIND (size 0, no other flag), a reference to the live VMO
 * under key instead (ERR_NOT_FOUND if none: never made, detached, or
 * dying); any other flag ERR_INVALID_ARGS. */
status_t pager_create_vmo_flags(struct pager *pg, uint64_t key, uint64_t size, uint32_t flags,
                                struct vmo **out);
/* The oldest undelivered request into *out (zeroed first), marked
 * delivered; ERR_SHOULD_WAIT if none. PAGER_NEXT_AGAIN first puts every
 * delivered request without an answer back in the queue; any other flag
 * is ERR_INVALID_ARGS. A read request whose answer time has passed is
 * failed (ERR_TIMED_OUT) instead of delivered. */
status_t pager_next(struct pager *pg, uint32_t flags, struct pager_request *out);
/* Fill the absent pages of [offset, offset + len) of key's VMO from src,
 * page by page: each copied into a fresh page with no lock held, then
 * published if its slot is still absent (else dropped), inside the
 * budget (the oldest pages evicted for it). When the call ends, every
 * request all of whose pages arrived is answered, even if some were
 * evicted since (its waiters look again). ERR_INVALID_ARGS: offset or len
 * not page aligned, len 0, src unreadable; ERR_OUT_OF_RANGE: len past
 * PAGER_SUPPLY_MAX or the range past the VMO's size; ERR_NOT_FOUND: no
 * live VMO has key; ERR_NO_MEMORY: no page even with the pager's cache
 * evicted, or the job refused one (the pages before it stay). A verified
 * VMO's page that doesn't match its hash is never published: the rest of
 * the range is supplied, then the requests waiting on such a page fail
 * with ERR_BAD_STATE and the call returns ERR_IO (unless it stopped for one
 * of the errors above). Interrupts
 * on, no spinlock held. */
status_t pager_supply(struct pager *pg, uint64_t key, uint64_t offset, const struct pager_src *src,
                      uint64_t len);
/* PAGER_OP_FAIL: fail key's requests overlapping [offset, offset + len)
 * (page-aligned, len > 0) with arg, ERR_IO or ERR_OUT_OF_RANGE.
 * PAGER_OP_INVALIDATE (page-aligned, len > 0, arg 0): drop the present
 * pages of [offset, offset + len) from the table and every mapping; the
 * next access asks again (ERR_OUT_OF_RANGE past the size). Requests are
 * left as they are. PAGER_OP_SET_SIZE (offset: the size, rounded up to
 * pages, VMO_SMALL_MAX_SIZE at most, else ERR_OUT_OF_RANGE; len and arg 0):
 * grow (the new pages absent) or shrink: the pages past the end are
 * dropped as for INVALIDATE, and requests past it are answered so their
 * waiters look again and meet the end (a bus error for a fault); one that
 * crosses it is cut there. PAGER_OP_DETACH (offset, len and arg 0): key's
 * VMO is cut off for good (its requests fail ERR_PEER_CLOSED now and
 * later, its pages are dropped from the table and every mapping, the key
 * is free), or key's PAGER_REQ_GONE is answered (the key is free).
 * ERR_NOT_FOUND: no live VMO has key (GONE: DETACH only); ERR_INVALID_ARGS:
 * any other op or argument. Interrupts on, no spinlock held (all but FAIL
 * may shoot down TLBs). */
status_t pager_op(struct pager *pg, uint64_t key, uint32_t op, uint64_t offset, uint64_t len,
                  int64_t arg);

/* pager_dirty (flags 0 or PAGER_DIRTY_PEEK; BEGIN and OLDEST are
 * ERR_NOT_SUPPORTED until write-back is built): the first run of key's
 * writable VMO's pages in [run->offset, run->offset + run->len) that are
 * dirty (PEEK: dirty or writing), copied to dst at their place in the
 * range, and *run set to where it is (len 0: none) and the age of its
 * oldest page. Nothing changes. ERR_INVALID_ARGS: offset or len not page
 * aligned, len 0, any other flag, the VMO not writable, dst unwritable
 * (some of the run copied); ERR_OUT_OF_RANGE: len past PAGER_SUPPLY_MAX;
 * ERR_NOT_FOUND: no live VMO has key. Interrupts on, no spinlock held. */
status_t pager_dirty(struct pager *pg, uint64_t key, uint32_t flags, struct pager_run *run,
                     const struct pager_dst *dst);
/* pager_write: len bytes from src into key's VMO at offset (any byte,
 * len at most PAGER_SUPPLY_MAX, else ERR_OUT_OF_RANGE; 0 does nothing),
 * into the pages of the range present when it looks (any state, which
 * stays), copied on page references with no lock held; absent pages stay
 * absent. ERR_NOT_FOUND: no live VMO has key; ERR_OUT_OF_RANGE: past its
 * size; ERR_BAD_STATE: it is verified; ERR_INVALID_ARGS: src unreadable
 * (the pages before it written). Interrupts on, no spinlock held. */
status_t pager_write(struct pager *pg, uint64_t key, uint64_t offset, const struct pager_src *src,
                     uint64_t len);
/* Pages of its VMOs that are dirty or writing right now (tests). */
uint64_t pager_dirty_pages(struct pager *pg);

/* Publish p (a fresh page holding page idx's bytes, the caller's
 * reference, consumed whatever happens) in pg's VMO v as pager_supply
 * does for each page, inside the budget; `checked` as for
 * vmo_pager_publish. OK if published or present already, else as
 * vmo_pager_publish says. pager_supply's step, and the tests' way to
 * publish a page copied before a seal. Interrupts on, no spinlock held. */
status_t pager_publish_page(struct pager *pg, struct vmo *v, uint64_t idx, struct page *p,
                            bool checked);
/* Pages its VMOs hold right now, counted against its budget (tests). */
uint64_t pager_resident(struct pager *pg);

/* ---- requests: what a reader of an absent page does ------------------------- */

/* Wait (cancellable) until r is answered or its deadline passes: OK once
 * supplied (the pages are there: look again), ERR_CANCELED if the thread
 * was cancelled first (r stays for the pager and its other waiters), else
 * how r failed: ERR_IO or ERR_OUT_OF_RANGE (PAGER_OP_FAIL),
 * ERR_PEER_CLOSED (detached, the pager's last handle closed, the VMO
 * gone), ERR_TIMED_OUT (not answered in time: this waiter fails it for
 * everyone), ERR_BAD_STATE (a verified VMO's page that the pager
 * supplied didn't match its hash: <jam/vmo_verify.h>). No spinlock held,
 * nor any lock the pager's progress needs. */
status_t pager_req_wait(struct pager_req *r);
/* Drop the caller's reference to r (pager_req_wait's, or one from
 * vmo_page_request). */
void pager_req_put(struct pager_req *r);

/* ---- the VMO side (kernel/object/vmo.c) ------------------------------------- */

/* A pager VMO of size bytes linked to pv, charged to job, writable if
 * `writable`. pv is the VMO's from then on: its destruction hands pv back
 * (pager_vmo_gone). */
status_t vmo_create_pager(struct job *job, uint64_t size, struct pager_vmo *pv, bool writable,
                          struct vmo **out);
/* Made from a pager (fixed at creation: no lock needed). */
bool vmo_is_pager(const struct vmo *v);
/* A writable pager VMO (fixed at creation: no lock needed). */
bool vmo_pager_writable(const struct vmo *v);
/* pager_dirty's look at a writable pager VMO: the first run of pages in
 * [first, end) (clipped to the size) whose state has a bit of `want`, at
 * most `max` pages, each with a reference in pages[] (the caller drops
 * them: page_unref); *at its first page, *n its length (0: none), *dirtied
 * when its oldest page was first dirtied (uptime ns). Nothing changes. */
void vmo_pager_dirty_run(struct vmo *v, uint64_t first, uint64_t end, unsigned want,
                         uint64_t max, struct page **pages, uint64_t *at, uint64_t *n,
                         uint64_t *dirtied);
/* pager_write's look: pages[i] gets a reference to page first + i if
 * present, else NULL, for [first, end) (end - first at most max).
 * ERR_OUT_OF_RANGE: past the size (byte_end, the range's end in bytes,
 * checked against it); ERR_BAD_STATE: v is verified. */
status_t vmo_pager_present(struct vmo *v, uint64_t first, uint64_t end, uint64_t byte_end,
                           struct page **pages);
/* A writable pager VMO's page idx's state (PAGER_PAGE_*; 0: clean or
 * absent), and its dirty and writing pages (tests). */
unsigned vmo_pager_page_state(struct vmo *v, uint64_t idx);
uint64_t vmo_pager_dirty_count(struct vmo *v);
/* A pager VMO: its pager's koid (handle_info's related_koid); else 0. */
uint64_t vmo_pager_koid(struct vmo *v);
/* For a reader of page idx of a pager VMO, found absent: *out gets a
 * reference to the request that covers it (queued now, or one already
 * there), or NULL if the page is present after all (look again).
 * ERR_OUT_OF_RANGE past the size; ERR_PEER_CLOSED if the VMO is detached
 * or gone from its pager; ERR_NO_MEMORY if the caller's job can't pay for
 * a new request. Thread context, no spinlock held (the region lock may be
 * held: only spinlocks are taken). For a copy-on-write child of a pager
 * VMO (vmo_create_child), the request is its pager VMO's for the page
 * idx takes from it; NULL also if idx doesn't take one (present, past the
 * window) or the pager VMO ends before it (it reads as zeros). */
status_t vmo_page_request(struct vmo *v, uint64_t idx, struct pager_req **out);
/* Publish p as page idx of a pager VMO if that slot is absent, charged
 * to the VMO's job, with rec its record in its pager's cache (a slot of
 * the budget reserved for it); the requests covering idx note its
 * arrival. `checked`: p's bytes were matched against v's table already
 * (vmo_verify_supply); a verified VMO's page that wasn't is matched here.
 * OK: published, p (the caller's reference) and rec taken. Nothing is
 * taken otherwise: ERR_ALREADY_EXISTS if the page is present,
 * ERR_OUT_OF_RANGE past the size, ERR_PEER_CLOSED if detached, ERR_IO if
 * v is verified and p doesn't match, ERR_NO_MEMORY if the job refused it
 * (or a table for it). */
status_t vmo_pager_publish(struct vmo *v, uint64_t idx, struct page *p, bool checked,
                           struct pager_rec *rec);
/* Evict page idx of a pager VMO if it is still p (the cache popped it):
 * out of the table, zapped from every mapping, freed after the
 * shootdown. Interrupts on, no spinlock held. */
void vmo_pager_evict(struct vmo *v, uint64_t idx, const struct page *p);
/* PAGER_OP_INVALIDATE's work on a pager VMO: its present pages in
 * [offset, offset + len) dropped as vmo_pager_evict drops one.
 * ERR_OUT_OF_RANGE past the size. Interrupts on, no spinlock held. */
status_t vmo_pager_invalidate(struct vmo *v, uint64_t offset, uint64_t len);
/* PAGER_OP_SET_SIZE's work (size page-aligned; past VMO_SMALL_MAX_SIZE
 * ERR_OUT_OF_RANGE): the size changed; shrinking cuts the pager's requests
 * at the new end (pager_cut_vlocked: any freed go on `dead`) and drops the
 * pages past it.
 * ERR_PEER_CLOSED if detached, ERR_BAD_STATE if sealed. Interrupts on, no
 * spinlock held. */
status_t vmo_pager_set_size(struct vmo *v, uint64_t size, struct list_node *dead);
/* Cut a pager VMO off its pager for good: no page is published any more,
 * and every present one is dropped (from every mapping first, freed after
 * the shootdown). Interrupts on, no spinlock held. */
void vmo_pager_detach(struct vmo *v);
/* VMO lock held (by vmo.c, around the pager's _vlocked calls below): is
 * page idx present? */
bool vmo_page_present_locked(struct vmo *v, uint64_t idx);

/* ---- the pager side, called by vmo.c with the VMO lock held ------------------ */

/* Page idx of pv's VMO is absent and so are [idx, end), end > idx: join
 * the request covering idx, or make one of [idx, end) cut short at the
 * first page another request covers, using *spare (taken: *spare = NULL)
 * and stamping its deadline. *out gets a reference. ERR_PEER_CLOSED if pv
 * is no longer linked to its key. */
status_t pager_join_vlocked(struct pager_vmo *pv, uint64_t idx, uint64_t end,
                            struct pager_req **spare, struct pager_req **out);
/* Page idx (p) of pv's VMO was just published with rec: the request
 * covering idx notes its arrival (answered when the supply ends), and p
 * joins the pager's cache. */
void pager_published_vlocked(struct pager_vmo *pv, uint64_t idx, struct page *p,
                             struct pager_rec *rec);
/* Page p of pv's VMO leaves its table (taken out, or the VMO destroyed:
 * then no lock is needed): it leaves the pager's cache. */
void pager_page_gone_vlocked(struct pager_vmo *pv, struct page *p);
/* Page p (present) of pv's VMO, a writable one: its state (PAGER_PAGE_*,
 * 0: clean), and when it was first dirtied since it was last clean (uptime
 * ns; meaningless while clean). */
unsigned pager_page_state_vlocked(struct pager_vmo *pv, const struct page *p);
uint64_t pager_page_dirtied_vlocked(struct pager_vmo *pv, const struct page *p);
/* A store to page p (present) of pv's VMO, a writable one: clean becomes
 * dirty (off the cache's list onto the pager's dirty list, oldest first:
 * never evicted), writing becomes writing and dirty. */
void pager_page_store_vlocked(struct pager_vmo *pv, struct page *p);
/* pv's VMO shrank to `end` pages: its requests from there on are answered
 * (OK: the waiters look again and find the end), one crossing it is cut
 * there (and answered if all its pages below arrived). */
void pager_cut_vlocked(struct pager_vmo *pv, uint64_t end, struct list_node *dead);
/* A spare request for pv's next join, charged to the current thread's job
 * (ERR_NO_MEMORY if it refuses, or no memory); pager_req_free gives back
 * one that wasn't used. */
status_t pager_req_alloc(struct pager_vmo *pv, struct pager_req **out);
void pager_req_free(struct pager_req *r);
/* pv's VMO is being destroyed (no reference left): if linked, its key
 * stays taken and the pager gets a PAGER_REQ_GONE; else pv goes. */
void pager_vmo_gone(struct pager_vmo *pv);
/* The koid of pv's pager. */
uint64_t pager_vmo_pager_koid(const struct pager_vmo *pv);

/* ---- the handle layer (kernel/abi/pager_sys.c) ------------------------------ */

/* RIGHT_ROOT_PAGER on root; flags 0. *out: PAGER_RIGHTS. The pager is
 * charged to t's job. */
status_t sys_pager_create(struct handle_table *t, handle_t root, uint64_t max_pages,
                          uint64_t answer_ns, uint32_t flags, handle_t *out);
/* RIGHT_WRITE on pager; flags as pager_create_vmo's. *out:
 * PAGER_VMO_RIGHTS, and RIGHT_WRITE for a writable VMO. */
status_t sys_pager_create_vmo(struct handle_table *t, handle_t pager, uint64_t key, uint64_t size,
                              uint32_t flags, handle_t *out);
/* RIGHT_WRITE on pager. */
status_t sys_pager_dirty(struct handle_table *t, handle_t pager, uint64_t key, uint32_t flags,
                         struct pager_run *run, const struct pager_dst *dst);
/* RIGHT_WRITE on pager. */
status_t sys_pager_write(struct handle_table *t, handle_t pager, uint64_t key, uint64_t offset,
                         const struct pager_src *src, uint64_t len);
/* RIGHT_READ on pager. */
status_t sys_pager_next(struct handle_table *t, handle_t pager, uint32_t flags,
                        struct pager_request *out);
/* RIGHT_WRITE on pager. */
status_t sys_pager_supply(struct handle_table *t, handle_t pager, uint64_t key, uint64_t offset,
                          const struct pager_src *src, uint64_t len);
/* RIGHT_WRITE on pager. */
status_t sys_pager_op(struct handle_table *t, handle_t pager, uint64_t key, uint32_t op,
                      uint64_t offset, uint64_t len, int64_t arg);
