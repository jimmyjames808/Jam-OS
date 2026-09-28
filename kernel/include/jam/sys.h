/* Handle-level operations: what become system calls in M5. Each takes the
 * caller's handle table and handle values, checks the handle's type and
 * rights, then calls the object layer. Buffers are kernel pointers until
 * M5 adds user-copy. */
#pragma once

#include <stdint.h>
#include <jam/handle.h>
#include <jam/status.h>

/* VMOs */
/* New VMO (vmo_create flags); the handle gets
 * RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MAP. */
status_t sys_vmo_create(struct handle_table *t, uint64_t size, uint32_t flags, handle_t *out);
status_t sys_vmo_read(struct handle_table *t, handle_t h, uint64_t offset, void *buf,
                      uint64_t len);                                        /* RIGHT_READ */
status_t sys_vmo_write(struct handle_table *t, handle_t h, uint64_t offset, const void *buf,
                       uint64_t len);                                       /* RIGHT_WRITE */
status_t sys_vmo_get_size(struct handle_table *t, handle_t h, uint64_t *size);   /* any */
status_t sys_vmo_set_size(struct handle_table *t, handle_t h, uint64_t size);    /* RIGHT_WRITE */
status_t sys_vmo_commit(struct handle_table *t, handle_t h, uint64_t offset,
                        uint64_t len);                                      /* RIGHT_WRITE */
status_t sys_vmo_decommit(struct handle_table *t, handle_t h, uint64_t offset,
                          uint64_t len);                                    /* RIGHT_WRITE */
/* end VMOs */
