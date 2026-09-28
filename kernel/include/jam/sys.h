/* The handle-level API: what become system calls in M5. Each call names
 * objects by handle values in a handle table and checks the handle's type
 * and rights. Buffers are kernel pointers until M5 adds user copies. */
#pragma once

#include <stdint.h>
#include <jam/handle.h>
#include <jam/status.h>

/* channels ------------------------------------------------------------------
 * (kernel/abi/channel_sys.c; semantics as in <jam/channel.h>) */

/* Two new endpoints with RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_SIGNAL. */
status_t sys_channel_create(struct handle_table *t, handle_t *a, handle_t *b);
/* Needs RIGHT_WRITE on h and RIGHT_TRANSFER on every sent handle. The sent
 * handles leave t only if the write succeeds; on failure they are all
 * still there under the same values. */
status_t sys_channel_write(struct handle_table *t, handle_t h, const void *bytes, uint32_t nbytes,
                           const handle_t *handles, uint32_t nhandles);
/* Needs RIGHT_READ. Received handles are inserted into t and their values
 * stored in handles[0 .. *actual_handles). */
status_t sys_channel_read(struct handle_table *t, handle_t h, void *bytes, uint32_t bytes_cap,
                          uint32_t *actual_bytes, handle_t *handles, uint32_t handles_cap,
                          uint32_t *actual_handles);
/* Needs RIGHT_READ | RIGHT_WRITE. Request handles as for sys_channel_write
 * (they stay in t if the request could not be sent), reply handles as for
 * sys_channel_read. */
status_t sys_channel_call(struct handle_table *t, handle_t h, void *wbytes, uint32_t wn,
                          const handle_t *wh, uint32_t whn, void *rbytes, uint32_t rcap,
                          uint32_t *ractual, handle_t *rh, uint32_t rhcap, uint32_t *rhactual,
                          uint64_t deadline_ns);
/* end channels */
