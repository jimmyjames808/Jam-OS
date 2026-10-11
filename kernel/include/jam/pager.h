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
 * (vmo_write, vmo_commit, writable mappings), take one away (vmo_decommit,
 * vmo_set_size), make a device or the kernel wait for it (vmo_pin,
 * vmo_map_kernel), keep it (VMAR_KEPT_ONLY) or run it unchecked
 * (vmo_seal: its code runs only through vmo_make_exec_verified, which
 * checks each page as it arrives, <jam/vmo_verify.h>).
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
 * pager side never takes a VMO lock under its own.
 *
 * Charges: the pager is one JOB_LIMIT_HANDLES unit of its creator's job,
 * and so is each of its VMOs' links; the VMOs (struct, tables, pages) are
 * charged to that job as any VMO; a request is charged to the job of the
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
struct pager_req;    /* one request (pager.c) */
struct pager_vmo;    /* a pager VMO's link to its pager (pager.c) */
struct vmo;

static inline struct kobject *pager_kobject(struct pager *pg) { return (struct kobject *)pg; }
/* NULL unless obj is a pager. */
static inline struct pager *pager_from_kobject(struct kobject *obj)
{
    return obj && obj->type == OBJ_PAGER ? (struct pager *)obj : NULL;
}

/* Where pager_supply's bytes come from: kernel memory, or memory of the
 * current process (copied with copy_from_user, no lock held). */
struct pager_src {
    uint64_t addr;   /* a kernel pointer, or a user address */
    bool     user;   /* addr is a user address */
};

/* ---- the object layer (kernel/object/pager.c) ------------------------------ */

/* A pager whose VMOs may hold max_pages pages (> 0: its budget, not
 * enforced yet) and whose requests wait answer_ns at most
 * (PAGER_ANSWER_MIN_NS .. _MAX_NS), charged with its VMOs to job (NULL:
 * nothing charged). ERR_INVALID_ARGS for max_pages 0, ERR_OUT_OF_RANGE for
 * answer_ns, ERR_NO_RESOURCES / ERR_NO_MEMORY if the job refuses it. The
 * caller gets the only reference. */
status_t pager_create(struct job *job, uint64_t max_pages, uint64_t answer_ns,
                      struct pager **out);
/* A pager VMO of size bytes (rounded up; VMO_MAX_SIZE at most, else
 * ERR_OUT_OF_RANGE) under key, every page absent; the caller gets the only
 * reference. ERR_ALREADY_EXISTS if key is taken (a live VMO, or a gone one
 * whose PAGER_REQ_GONE has no answer yet); ERR_BAD_STATE once the pager's
 * last handle is gone. Thread context. */
status_t pager_create_vmo(struct pager *pg, uint64_t key, uint64_t size, struct vmo **out);
/* The oldest undelivered request into *out (zeroed first), marked
 * delivered; ERR_SHOULD_WAIT if none. PAGER_NEXT_AGAIN first puts every
 * delivered request without an answer back in the queue; any other flag
 * is ERR_INVALID_ARGS. A read request whose answer time has passed is
 * failed (ERR_TIMED_OUT) instead of delivered. */
status_t pager_next(struct pager *pg, uint32_t flags, struct pager_request *out);
/* Fill the absent pages of [offset, offset + len) of key's VMO from src,
 * page by page: each copied into a fresh page with no lock held, then
 * published if its slot is still absent (else dropped); every request
 * made whole is answered. ERR_INVALID_ARGS: offset or len not page
 * aligned, len 0, src unreadable; ERR_OUT_OF_RANGE: len past
 * PAGER_SUPPLY_MAX or the range past the VMO's size; ERR_NOT_FOUND: no
 * live VMO has key; ERR_NO_MEMORY: no page, or the job refused one (the
 * pages before it stay). A verified VMO's page that doesn't match its hash
 * is never published: the rest of the range is supplied, then the
 * requests waiting on such a page fail with ERR_BAD_STATE and the call
 * returns ERR_IO (unless it stopped for one of the errors above). Interrupts
 * on, no spinlock held. */
status_t pager_supply(struct pager *pg, uint64_t key, uint64_t offset, const struct pager_src *src,
                      uint64_t len);
/* PAGER_OP_FAIL: fail key's requests overlapping [offset, offset + len)
 * (page-aligned, len > 0) with arg, ERR_IO or ERR_OUT_OF_RANGE.
 * PAGER_OP_DETACH (offset, len and arg 0): key's VMO is cut off for good
 * (its requests fail ERR_PEER_CLOSED now and later, its pages are dropped
 * from the table and every mapping, the key is free), or key's
 * PAGER_REQ_GONE is answered (the key is free). INVALIDATE, SET_SIZE:
 * ERR_NOT_SUPPORTED (not built yet). ERR_NOT_FOUND: no such key;
 * ERR_INVALID_ARGS: any other op or argument. Interrupts on, no spinlock
 * held (DETACH shoots down TLBs). */
status_t pager_op(struct pager *pg, uint64_t key, uint32_t op, uint64_t offset, uint64_t len,
                  int64_t arg);

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

/* A pager VMO of size bytes linked to pv, charged to job. pv is the
 * VMO's from then on: its destruction hands pv back (pager_vmo_gone). */
status_t vmo_create_pager(struct job *job, uint64_t size, struct pager_vmo *pv, struct vmo **out);
/* Made from a pager (fixed at creation: no lock needed). */
bool vmo_is_pager(const struct vmo *v);
/* A pager VMO: its pager's koid (handle_info's related_koid); else 0. */
uint64_t vmo_pager_koid(struct vmo *v);
/* For a reader of page idx of a pager VMO, found absent: *out gets a
 * reference to the request that covers it (queued now, or one already
 * there), or NULL if the page is present after all (look again).
 * ERR_OUT_OF_RANGE past the size; ERR_PEER_CLOSED if the VMO is detached
 * or gone from its pager; ERR_NO_MEMORY if the caller's job can't pay for
 * a new request. Thread context, no spinlock held (the region lock may be
 * held: only spinlocks are taken). */
status_t vmo_page_request(struct vmo *v, uint64_t idx, struct pager_req **out);
/* Publish p (the caller's reference, consumed whatever happens) as page idx
 * of a pager VMO if that slot is absent, charged to the VMO's job, and
 * answer the requests it makes whole (any whose last reference goes is
 * put on `dead`, for pager.c to free once no lock is held). `checked`:
 * p's bytes were matched against v's table already (vmo_verify_supply);
 * a verified VMO's page that wasn't is matched here. OK if published or
 * present already; ERR_OUT_OF_RANGE past the size; ERR_PEER_CLOSED if
 * detached; ERR_IO if v is verified and p doesn't match; ERR_NO_MEMORY if
 * the job refused it. */
status_t vmo_pager_publish(struct vmo *v, uint64_t idx, struct page *p, bool checked,
                           struct list_node *dead);
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
/* Page idx of v (pv's) was just published: answer every request of pv
 * whose pages are now all present. */
void pager_arrived_vlocked(struct pager_vmo *pv, struct vmo *v, uint64_t idx,
                           struct list_node *dead);
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
/* RIGHT_WRITE on pager; flags 0. *out: PAGER_VMO_RIGHTS. */
status_t sys_pager_create_vmo(struct handle_table *t, handle_t pager, uint64_t key, uint64_t size,
                              uint32_t flags, handle_t *out);
/* RIGHT_READ on pager. */
status_t sys_pager_next(struct handle_table *t, handle_t pager, uint32_t flags,
                        struct pager_request *out);
/* RIGHT_WRITE on pager. */
status_t sys_pager_supply(struct handle_table *t, handle_t pager, uint64_t key, uint64_t offset,
                          const struct pager_src *src, uint64_t len);
/* RIGHT_WRITE on pager. */
status_t sys_pager_op(struct handle_table *t, handle_t pager, uint64_t key, uint32_t op,
                      uint64_t offset, uint64_t len, int64_t arg);
