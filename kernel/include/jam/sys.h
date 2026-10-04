/* The handle-level API the system calls run on. Every function takes the
 * caller's handle table and handle values, checks each handle's type and
 * rights, and calls the object layer. Pointers are kernel pointers: the
 * system calls (kernel/abi/sysc_*.c) copy user memory in and out around
 * these, and kernel tests call them directly. The exception is a
 * struct chan_bytes, which may name user memory (channels' message bytes,
 * copied once, straight into or out of the message). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/channel.h>
#include <jam/handle.h>
#include <jam/object.h>
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
/* As sys_channel_write, but handle i arrives with rights[i] (a subset
 * of its rights, else ERR_INVALID_ARGS; RIGHT_SAME: unchanged). How a
 * sender passes a handle the receiver may not pass on (without
 * RIGHT_TRANSFER / RIGHT_DUPLICATE): devmgr's driver handles. */
status_t sys_channel_write_rights(struct handle_table *t, handle_t h, const void *bytes,
                                  uint32_t nbytes, const handle_t *handles,
                                  const rights_t *rights, uint32_t nhandles);
/* Needs RIGHT_READ. Received handles are inserted into t and their values
 * stored in handles[0 .. *actual_handles). If t has no room for them the
 * read fails (ERR_NO_RESOURCES) and the message stays queued: no handle is
 * ever lost on the way in. */
status_t sys_channel_read(struct handle_table *t, handle_t h, void *bytes, uint32_t bytes_cap,
                          uint32_t *actual_bytes, handle_t *handles, uint32_t handles_cap,
                          uint32_t *actual_handles);
/* Needs RIGHT_READ | RIGHT_WRITE. Request handles as for sys_channel_write
 * (they stay in t if the request could not be sent). Slots for up to rhcap
 * reply handles are reserved before the request is sent, so a full table
 * fails the call up front (ERR_NO_RESOURCES) instead of losing the reply's
 * handles. */
status_t sys_channel_call(struct handle_table *t, handle_t h, void *wbytes, uint32_t wn,
                          const handle_t *wh, uint32_t whn, void *rbytes, uint32_t rcap,
                          uint32_t *ractual, handle_t *rh, uint32_t rhcap, uint32_t *rhactual,
                          uint64_t deadline_ns);
/* The same four with the bytes as a struct chan_bytes (<jam/channel.h>):
 * how the system calls pass user buffers, which the object layer copies
 * straight into and out of the message. */
status_t sys_channel_write_from(struct handle_table *t, handle_t h, const struct chan_bytes *b,
                                const handle_t *handles, uint32_t nhandles);
status_t sys_channel_write_rights_from(struct handle_table *t, handle_t h,
                                       const struct chan_bytes *b, const handle_t *handles,
                                       const rights_t *rights, uint32_t nhandles);
status_t sys_channel_read_into(struct handle_table *t, handle_t h, const struct chan_bytes *b,
                               uint32_t *actual_bytes, handle_t *handles, uint32_t handles_cap,
                               uint32_t *actual_handles);
status_t sys_channel_call_from(struct handle_table *t, handle_t h, const struct chan_bytes *w,
                               const handle_t *wh, uint32_t whn, const struct chan_bytes *r,
                               uint32_t *ractual, handle_t *rh, uint32_t rhcap,
                               uint32_t *rhactual, uint64_t deadline_ns);
/* One channel_reply_wait (abi/syscalls.def has the rules): the reply,
 * where the next request comes from, and the results. */
struct chan_reply_wait {
    handle_t           h;             /* reply on it; HANDLE_INVALID: no reply */
    struct chan_bytes  reply;         /* the reply's bytes */
    const handle_t    *reply_h;       /* its handles: they leave t once it is sent */
    uint32_t           reply_nh;      /* how many */
    handle_t           wait;          /* a channel end (read from it) or a port */
    struct chan_bytes  mark;          /* room for a uint64_t 1 once the reply went out
                                       * (len 0: none) */
    struct chan_bytes  req;           /* channel: room for the request */
    handle_t          *req_h;         /* channel: room for its handles' values */
    uint32_t           req_hcap;      /* channel: how many */
    bool               want_request;  /* the caller gave room for a channel's request
                                       * (any of its fields set: wrong for a port) */
    bool               want_packet;   /* the caller has room for a port packet (wrong
                                       * for a channel) */
    uint64_t           deadline_ns;   /* absolute */
    /* Results. */
    bool               replied;       /* there was a reply to send: reply_st is its status */
    status_t           reply_st;
    bool               is_port;       /* wait was a port: pkt holds its packet */
    uint32_t           req_nb;        /* channel: the request's bytes (also with
                                       * ERR_BUFFER_TOO_SMALL) */
    uint32_t           req_nh;        /* channel: its handles (ditto) */
    struct port_packet pkt;           /* port: the packet */
};
/* Needs RIGHT_WRITE on rw->h and RIGHT_READ on rw->wait. Reply handles as
 * for sys_channel_write; slots for the request's handles are reserved
 * before the reply goes out (ERR_NO_RESOURCES up front, nothing sent).
 * The reply's wake may hand this CPU straight to its caller (the direct
 * hand-off, <jam/sched.h>): the wait that follows blocks. */
status_t sys_channel_reply_wait(struct handle_table *t, struct chan_reply_wait *rw);
/* end channels */

/* ports, events, timers, waiting ---------------------------------------------
 * (kernel/abi/port_sys.c; semantics as in <jam/port.h>, <jam/event.h>,
 * <jam/timer.h>) */

struct port_packet;

/* New event: RIGHTS_BASIC | RIGHT_SIGNAL. */
status_t sys_event_create(struct handle_table *t, handle_t *out);
/* RIGHT_SIGNAL; only SIG_SIGNALED | SIG_USER_ALL. */
status_t sys_event_signal(struct handle_table *t, handle_t h, signals_t clear, signals_t set);
/* New timer: RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE. */
status_t sys_timer_create(struct handle_table *t, handle_t *out);
/* RIGHT_WRITE. */
status_t sys_timer_set(struct handle_table *t, handle_t h, uint64_t deadline_ns);
/* RIGHT_WRITE. */
status_t sys_timer_cancel(struct handle_table *t, handle_t h);
/* New port: RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE. */
status_t sys_port_create(struct handle_table *t, handle_t *out);
/* Port needs RIGHT_WRITE, the watched object (any type) RIGHT_WAIT. */
status_t sys_port_bind(struct handle_table *t, handle_t port, handle_t obj, uint64_t key,
                       signals_t mask, uint32_t flags);
/* Port needs RIGHT_WRITE; obj only has to be a valid handle. */
status_t sys_port_unbind(struct handle_table *t, handle_t port, handle_t obj, uint64_t key);
status_t sys_port_queue(struct handle_table *t, handle_t port,
                        const struct port_packet *pkt);                    /* RIGHT_WRITE */
status_t sys_port_wait(struct handle_table *t, handle_t port, uint64_t deadline_ns,
                       struct port_packet *out);                           /* RIGHT_READ */
/* RIGHT_SIGNAL, any type: set/clear the user bits (SIG_USER_ALL only; the
 * kernel owns the rest). Wakes waiters and ports like any signal change. */
status_t sys_object_signal(struct handle_table *t, handle_t h, signals_t clear, signals_t set);
/* RIGHT_WAIT, any type. */
status_t sys_object_wait_one(struct handle_table *t, handle_t h, signals_t mask,
                             uint64_t deadline_ns, signals_t *observed);
/* end ports, events, timers, waiting */

/* VMOs ------------------------------------------------------------------------
 * (kernel/abi/vmo_sys.c; semantics as in <jam/vmo.h>) */

/* New VMO (vmo_create flags); the handle gets
 * RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_RESIZE. VMO_CONTIGUOUS /
 * VMO_DMA32 require dma_cap to name a valid OBJ_DMA_CAP handle
 * (ERR_ACCESS_DENIED otherwise); for other flags dma_cap is ignored. */
status_t sys_vmo_create(struct handle_table *t, uint64_t size, uint32_t flags,
                        handle_t dma_cap, handle_t *out);
status_t sys_vmo_read(struct handle_table *t, handle_t h, uint64_t offset, void *buf,
                      uint64_t len);                                        /* RIGHT_READ */
status_t sys_vmo_write(struct handle_table *t, handle_t h, uint64_t offset, const void *buf,
                       uint64_t len);                                       /* RIGHT_WRITE */
status_t sys_vmo_get_size(struct handle_table *t, handle_t h, uint64_t *size);   /* any */
/* RIGHT_WRITE | RIGHT_RESIZE. */
status_t sys_vmo_set_size(struct handle_table *t, handle_t h, uint64_t size);
status_t sys_vmo_commit(struct handle_table *t, handle_t h, uint64_t offset,
                        uint64_t len);                                      /* RIGHT_WRITE */
/* vmo_make_exec (abi/syscalls.def): RIGHT_ROOT_VMEX on root; h is gone
 * once this succeeds, and if the VMO turns out shared (ERR_BAD_STATE). */
status_t sys_vmo_make_exec(struct handle_table *t, handle_t h, handle_t root, handle_t *out);
/* RIGHT_WRITE | RIGHT_RESIZE. */
status_t sys_vmo_decommit(struct handle_table *t, handle_t h, uint64_t offset, uint64_t len);
/* end VMOs */

/* VMARs (address spaces) -------------------------------------------------------
 * (kernel/abi/vmar_sys.c; semantics as in <jam/aspace.h>, <jam/vmar.h>) */

/* New vmar over an empty address space: RIGHTS_BASIC | RIGHT_READ |
 * RIGHT_WRITE. (For tests; a process gets its own with process_create.) */
status_t sys_vmar_create(struct handle_table *t, handle_t *out);
/* flags: ASPACE_READ/WRITE/EXEC/FIXED only. Needs RIGHT_WRITE on vmar and
 * RIGHT_MAP on vmo, plus RIGHT_READ / RIGHT_WRITE / RIGHT_EXEC on vmo for
 * each permission asked for. The VMO handle's READ/WRITE/EXEC rights bound
 * later protects. *addr as for aspace_map. */
status_t sys_vmar_map(struct handle_table *t, handle_t vmar, handle_t vmo, uint64_t vmo_off,
                      uint64_t len, uint32_t flags, uint64_t *addr);
status_t sys_vmar_unmap(struct handle_table *t, handle_t vmar, uint64_t addr,
                        uint64_t len);                                      /* RIGHT_WRITE */
/* RIGHT_WRITE; ERR_ACCESS_DENIED for a permission the mapping's VMO handle
 * didn't carry. */
status_t sys_vmar_protect(struct handle_table *t, handle_t vmar, uint64_t addr, uint64_t len,
                          uint32_t flags);
/* end VMARs */

/* interrupts --------------------------------------------------------------------
 * (kernel/abi/sysc_interrupt.c; semantics as in <jam/interrupt.h>) */

/* New interrupt object for MSI (flags 0, index 0) or MSI-X (IRQ_MSIX, table
 * entry index) of the function `dev` names: a RES_PCI_DEV handle with
 * RIGHT_MANAGE (devmgr's; drivers get the interrupt handle from it). The
 * handle gets RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE. */
status_t sys_interrupt_create_msi(struct handle_table *t, handle_t dev, uint32_t index,
                                  uint32_t flags, handle_t *out);
/* RIGHT_WRITE: clear SIG_INTERRUPT and unmask. */
status_t sys_interrupt_ack(struct handle_table *t, handle_t irq);
/* end interrupts */
