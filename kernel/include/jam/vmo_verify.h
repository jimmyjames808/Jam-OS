/* Verified pager VMOs (kernel/object/vmo_verify.c): a program from /data
 * whose pages arrive from fat one at a time, each checked against the
 * page hashes the owner's approval wrote (docs/M12.5-PLAN.md, "Programs
 * from /data").
 *
 * vmo_make_exec_verified (abi/syscalls.def 59) copies the caller's table
 * (SHA-256 of each 4 KiB page, the last zero-padded, 32 bytes a page) into
 * a struct vmo_verify, and seals the pager VMO with it (vmo_seal_verified,
 * vmo.c). From then on a page is published only if its bytes hash to the
 * table's entry for it: pager_supply checks each page before it publishes
 * it (vmo_verify_supply, no lock held), and vmo_pager_publish checks again
 * under the VMO lock any page that wasn't (one copied before the seal).
 * A page that doesn't match is never published: pager_supply answers
 * ERR_IO and fails the requests waiting on it with ERR_BAD_STATE, which a
 * fault turns into the kill "a page doesn't match the approved program"
 * and a copy or vmo_read into ERR_IO.
 *
 * The table is fixed once made (no lock needed to read it) and belongs to
 * the VMO, which frees it with itself. Its pages are charged to the job of
 * the caller of vmo_make_exec_verified (JOB_LIMIT_PAGES), as a pin is to
 * its pinner's. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/abi.h>   /* VMO_VERIFIED_MAX, VMO_VERIFIED_HASH_BYTES */
#include <jam/status.h>

struct job;
struct vmo;
struct vmo_verify;

/* A table for a VMO of `pages` pages (at most VMO_VERIFIED_MAX
 * bytes' worth), copied from the first pages * 32 bytes of table (with
 * vmo_read: it may wait for a pager), its pages charged to job (NULL:
 * none). ERR_OUT_OF_RANGE past VMO_VERIFIED_MAX; ERR_INVALID_ARGS if
 * table is shorter; ERR_NO_MEMORY if the job refuses a page or there is
 * none; vmo_read's errors (ERR_IO for a table its pager fails). Thread
 * context, no lock held. */
status_t vmo_verify_new(struct job *job, struct vmo *table, uint64_t pages,
                        struct vmo_verify **out);
void vmo_verify_free(struct vmo_verify *vf);
/* Do the PAGE_SIZE bytes at page hash to the table's entry for page idx?
 * Never for a page past the table (a VMO grown since). Any context: no
 * lock taken. */
bool vmo_verify_match(const struct vmo_verify *vf, uint64_t idx, const void *page);
/* pager_supply's check of bytes it is about to publish as page idx of v:
 * false if v is verified and they don't match. *checked says whether they
 * were checked (v was verified when we looked), for vmo_pager_publish. */
bool vmo_verify_supply(struct vmo *v, uint64_t idx, const void *page, bool *checked);
