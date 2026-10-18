/* User address spaces (kernel/mm/aspace.c).
 *
 * One flat address space per process: a PML4 whose kernel half (entries
 * 256-511) is shared with every other address space, plus a set of
 * mappings of VMOs in the user half. Page-table entries are filled on
 * demand by aspace_fault. Lock order: the region lock (a sleeping mutex,
 * class "aspace") -> "vmo" -> "aspace page tables" (spinlock); a fault may
 * also wait under the region lock for a VMO's copy-on-write child being
 * made or a clone of another address space (`protecting`: vmo.c). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/status.h>

#define ASPACE_READ  (1u << 0)
#define ASPACE_WRITE (1u << 1)
/* Never together with ASPACE_WRITE, but on a JIT VMO's mapping (vmo_make_jit;
 * aspace.c's header has the argument). */
#define ASPACE_EXEC  (1u << 2)
#define ASPACE_FIXED (1u << 3)   /* map at *addr exactly (else the top of the highest gap that fits) */
/* aspace_map only: a VMO_KEEP_PAGES VMO, read-only, every entry filled at
 * map time and kept for the mapping's life (see VMAR_KEPT_ONLY in
 * <jam/abi.h>). */
#define ASPACE_KEPT_ONLY (1u << 4)
/* aspace_map only, with ASPACE_FIXED and without ASPACE_KEPT_ONLY: unmap
 * whatever is in the range and map, in one step (VMAR_REPLACE). */
#define ASPACE_REPLACE   (1u << 5)
/* aspace_map only: the mapping's clone mode, share or none (VMAR_CLONE_SHARE
 * and VMAR_CLONE_NONE in <jam/abi.h>: the default without either, the two
 * cases the kernel decides itself); not both, not with ASPACE_KEPT_ONLY. */
#define ASPACE_CLONE_SHARE (1u << 6)
#define ASPACE_CLONE_NONE  (1u << 7)
/* aspace_map only: permissions a later aspace_protect may grant (on top of
 * the ones mapped now). sys_vmar_map sets them from the VMO handle's
 * rights, so a mapping can be made RW, filled, then flipped to RX, but
 * never gain a permission the handle didn't carry. */
#define ASPACE_CAN_READ  (1u << 8)
#define ASPACE_CAN_WRITE (1u << 9)
#define ASPACE_CAN_EXEC  (1u << 10)

struct aspace;
struct job;
struct vmo;

/* A new, empty address space with one reference, charged to nobody. */
status_t aspace_create(struct aspace **out);
/* The same, charged to job (JOB_LIMIT_PAGES; NULL = nobody) for as long as
 * it exists: its PML4 now, and later every page table it makes and a page
 * per ASPACE_MAPPINGS_PER_PAGE mappings. The address space holds a job
 * reference. ERR_NO_MEMORY if the job refuses the PML4. With a job, map,
 * unmap and protect fail with ERR_NO_MEMORY, and faults with ERR_NO_MEMORY,
 * when the job refuses what they need. */
status_t aspace_create_charged(struct job *job, struct aspace **out);
#define ASPACE_MAPPINGS_PER_PAGE 16   /* charged: one page per this many mappings */
void     aspace_ref(struct aspace *as);
/* The last reference unmaps everything and frees the page tables. It must
 * not be dropped while a CPU still has the address space loaded (a
 * thread's reference covers its time on a CPU; checked, panics). It never
 * sleeps, so it may run from object teardown. */
void     aspace_unref(struct aspace *as);

/* Map [vmo_off, vmo_off+len) of vmo (page-aligned, len > 0) with flags.
 * *addr: in, the address for ASPACE_FIXED; out, where it was mapped. The
 * mapping holds a VMO reference. ERR_INVALID_ARGS for W+X on a VMO that
 * isn't a JIT VMO (vmo_is_jit), W or X without R, or a bad range, ERR_OUT_OF_RANGE past the VMO's end,
 * ERR_NO_RESOURCES if it doesn't fit or would make more than
 * VMAR_MAX_OVERLAP mappings of vmo over a page here, ERR_ALREADY_BOUND if a FIXED range
 * overlaps an existing mapping. No permissions at all is allowed (a guard:
 * every access faults with ERR_ACCESS_DENIED). With ASPACE_KEPT_ONLY:
 * ERR_INVALID_ARGS unless the permissions are ASPACE_READ alone,
 * ERR_NOT_SUPPORTED unless vmo is kept (vmo_is_kept), and every entry is
 * installed before this returns (ERR_NO_MEMORY if the job refuses a page
 * table: nothing is left mapped); the CAN bits are ignored, and
 * aspace_protect refuses any change to it (ERR_ACCESS_DENIED). With
 * ASPACE_REPLACE (and ASPACE_FIXED; ERR_INVALID_ARGS otherwise, or with
 * ASPACE_KEPT_ONLY): what the range overlaps is unmapped and the new
 * mapping made under one hold of the region lock, so a fault sees the old
 * mapping or the new one; every error comes before anything changed
 * (ERR_NO_RESOURCES past MAX_MAPPINGS, counted after the replace). The
 * mapping's clone mode is decided here, for its life (ASPACE_CLONE_*;
 * ERR_INVALID_ARGS for both, or either with ASPACE_KEPT_ONLY). */
status_t aspace_map(struct aspace *as, struct vmo *vmo, uint64_t vmo_off, uint64_t len,
                    unsigned flags, uint64_t *addr);
/* Remove every mapping page in [addr, addr+len) (splitting mappings as
 * needed) and shoot the range down on the CPUs using this address space.
 * Holes are fine; ERR_NOT_FOUND if nothing at all was mapped there. */
status_t aspace_unmap(struct aspace *as, uint64_t addr, uint64_t len);
/* Change permissions of [addr, addr+len), which must be fully mapped
 * (ERR_NOT_FOUND otherwise). ERR_ACCESS_DENIED if a mapping in the range
 * may not have them (see ASPACE_CAN_*). W+X: ERR_INVALID_ARGS unless
 * every mapping in the range is a JIT VMO's. */
status_t aspace_protect(struct aspace *as, uint64_t addr, uint64_t len, unsigned flags);
/* vmar_clone's kernel half (docs/FORK-PLAN.md section 1.1; aspace.c's
 * "Clones"): a VMO whose copy's child the caller wants. `v` in (the
 * caller's reference), `child` out (a new reference, or NULL), `index` the
 * caller's own: aspace_clone sorts the array by v. */
struct aspace_clone_req {
    struct vmo *v;
    struct vmo *child;
    uint32_t    index;
};
/* Make every mapping of src again in dst, an empty address space
 * (ERR_BAD_STATE otherwise, and for src itself), at the same address with
 * the same permissions and maximum, by its clone mode: a copy maps one
 * copy-on-write child per VMO (every copy of one VMO maps the same child),
 * holding the pages src's copies map of it; share maps the same VMO; none
 * leaves the range free. src's region lock is held throughout (no map,
 * unmap, protect or fault of src runs meanwhile) and every copied byte is
 * what it was at one moment. Charged to dst's job (ERR_NO_MEMORY): the
 * children and every page they share, dst's mapping structs, and as many
 * page tables as src has over the mappings made again, a reserve dst's
 * own tables draw on first. r[i].child gets the child of r[i].v
 * (ERR_INVALID_ARGS if src doesn't map that VMO as a copy).
 * ERR_BAD_STATE if a pin or a kernel mapping covers a page to copy.
 * Nothing is made, charged or handed out on an error. May sleep. */
status_t aspace_clone(struct aspace *src, struct aspace *dst, struct aspace_clone_req *r,
                      uint32_t n);
/* How many mappings it holds now (merged neighbours count once). Takes the
 * region lock: may sleep. */
uint32_t aspace_mapping_count(struct aspace *as);
/* The mappings in address order, from the first that ends after `from`: up
 * to cap of them into out, their number returned (<jam/abi.h> map_info).
 * One view under the region lock (may sleep); each entry's `resident`
 * counts the page-table entries present, so a decommit (which clears
 * entries without the region lock) may make a count a moment old. */
struct map_info;
uint32_t aspace_get_maps(struct aspace *as, uint64_t from, struct map_info *out, uint32_t cap);
/* mincore's look (vmar_resident, abi/syscalls.def): for the pages of
 * [addr, addr + pages * PAGE_SIZE) from addr on, out[i] = 1 if the VMO
 * mapped there holds the page (vmo_resident; a pager VMO's cache told
 * only through a mapping whose maximum may write it), else 0. Returns how
 * many pages from addr are mapped (fewer than `pages`: a hole after them;
 * 0: none at addr), out filled that far. One view under the region lock
 * (may sleep), each VMO's table read a leaf at a time under its lock; the
 * caller keeps `pages` small (VMAR_RESIDENT_MAX) and copies out after. */
uint64_t aspace_resident(struct aspace *as, uint64_t addr, uint64_t pages, uint8_t *out);
/* Whether as has mapped a JIT VMO executable since it was made
 * (PROCESS_FLAG_JIT): aspace_map with ASPACE_EXEC, or aspace_protect
 * adding it; a mapping that only reads or writes one doesn't count.
 * Lock-free. */
bool     aspace_maps_jit(struct aspace *as);
/* aspace_fault calls on as so far (every one, resolved or not): atomic,
 * lock-free. process_info.faults. */
uint64_t aspace_fault_count(struct aspace *as);
/* The debug listing's look at another process's memory (debug_command
 * "threads", kernel/debug/threads.c), and nothing else's: the 8-byte word
 * at va (8-aligned) into *out, if its page is mapped present and user right
 * now and is ordinary RAM. No fault is taken and nothing waits: the page
 * table lock (a spinlock) is held for the read alone, which keeps the page
 * from being freed under it. False: not there (absent, never touched,
 * evicted, a device's). Any context that may take a spinlock. */
bool     aspace_peek(struct aspace *as, uint64_t va, uint64_t *out);
/* The same look: is va's page there and may it run? */
bool     aspace_peek_exec(struct aspace *as, uint64_t va);

/* Resolve a fault at addr for `access` (ASPACE_READ/WRITE/EXEC): commit
 * the VMO page if needed and install the page-table entry. OK: retry the
 * access. ERR_NOT_FOUND: nothing mapped there. ERR_ACCESS_DENIED: the
 * mapping doesn't allow that access. ERR_NO_MEMORY: couldn't commit (or a
 * pager's page went again 8 times before the retry saw it).
 * ERR_OUT_OF_RANGE: the VMO was shrunk below the mapped page.
 * A pager VMO's absent page is waited for, without the region lock, until
 * its pager answers, the request's deadline or a kill: ERR_IO (the pager
 * couldn't read it), ERR_PEER_CLOSED (the pager is gone: detached or
 * closed), ERR_TIMED_OUT (it didn't answer), ERR_BAD_STATE (a verified
 * VMO's page that didn't match its hash: <jam/vmo_verify.h>), ERR_CANCELED
 * (the thread was killed; thread_interrupt doesn't end the wait). A store
 * to a clean page of a writable pager VMO whose pager is at its dirty
 * limit waits the same way for a write-back (<jam/pager.h>), the pager's
 * answer time in all: ERR_TIMED_OUT, ERR_PEER_CLOSED, ERR_CANCELED.
 * May sleep (region lock): call with interrupts on and no spinlock held. */
status_t aspace_fault(struct aspace *as, uint64_t addr, unsigned access);

/* On this CPU, with interrupts off: stop using prev (NULL = kernel page
 * tables) and start using next (NULL = kernel page tables): CR3 and the
 * address spaces' active-CPU masks. Cheap no-op when prev == next. */
void     aspace_switch(struct aspace *prev, struct aspace *next);
/* Physical address of the PML4 (for tests and the entry path). */
uint64_t aspace_pml4(struct aspace *as);
