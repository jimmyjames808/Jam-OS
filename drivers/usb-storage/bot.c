/* usb-storage: the Bulk-Only Transport (USB Mass Storage Class Bulk-Only
 * Transport 1.0).
 *
 * A command is three steps on the two bulk pipes: the 31-byte Command
 * Block Wrapper out, the data in or out (if any), the 13-byte Command
 * Status Wrapper in. The CSW echoes the CBW's tag and says passed, failed
 * or phase error.
 *
 * What can go wrong, and what the specification (5.3, 6.6, 6.7) says to do:
 *   - the data pipe STALLs (a device ends a data phase it can't fill that
 *     way, e.g. for a command it doesn't know): clear the halt, read the
 *     CSW, which then says "failed";
 *   - the CSW read STALLs: clear the halt and read it once more;
 *   - the CBW isn't taken, the CSW is not a CSW (size, signature, tag) or
 *     says phase error, or any step times out: reset recovery, which is
 *     the class request Bulk-Only Mass Storage Reset, then a clear halt on
 *     the IN pipe, then on the OUT pipe. The command is lost; the caller
 *     may run it again (scsi.c does, once, unless the command itself ran
 *     out of time).
 * Every step is one bounded usb-bus transfer, and the recovery is three
 * bounded requests, so a command always ends.
 *
 * The bulk buffer's last XFER_TAIL bytes are the transport's own: the CBW
 * and the CSW live there, so a command's data (below it) survives for a
 * second try. */
#include <idl/usb.h>
#include "storage.h"

#define CBW_SIGNATURE 0x43425355u   /* "USBC", little-endian on the wire */
#define CSW_SIGNATURE 0x53425355u   /* "USBS" */
#define CBW_LEN       31u
#define CSW_LEN       13u
#define CSW_PASSED    0
#define CSW_FAILED    1

#define STEP_MS       5000u                /* the CBW, and a recovery request */
#define CALL_MARGIN   (10 * NS_PER_S)      /* a usb-bus call may take this much longer than its
                                            * transfer: it stops the endpoint after a timeout */

#define REQ_CLASS_IFACE_OUT 0x21   /* host to device, class, interface */
#define REQ_CLASS_IFACE_IN  0xa1
#define BOT_RESET           0xff   /* Bulk-Only Mass Storage Reset */
#define BOT_GET_MAX_LUN     0xfe

static uint64_t call_deadline(uint32_t timeout_ms)
{
    return drv_clock_ns() + (uint64_t)timeout_ms * NS_PER_MS + CALL_MARGIN;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* One bulk transfer; a device that went away is remembered. */
static status_t xfer(struct disk *k, bool in, uint32_t off, uint32_t len, uint32_t timeout_ms,
                     uint32_t *actual)
{
    *actual = 0;
    status_t st = in ? usb_bulk_in_until(k->usb, call_deadline(timeout_ms), off, len, timeout_ms,
                                         actual)
                     : usb_bulk_out_until(k->usb, call_deadline(timeout_ms), off, len,
                                          timeout_ms, actual);
    if (st == ERR_PEER_CLOSED)
        k->gone = true;
    return st;
}

static status_t clear_halt(struct disk *k, uint8_t ep)
{
    status_t st = usb_clear_halt_until(k->usb, call_deadline(STEP_MS), ep);
    if (st == ERR_PEER_CLOSED)
        k->gone = true;
    return st;
}

/* Reset recovery (5.3.4). The device is ready for a CBW afterwards, its
 * pipes' toggles back at DATA0 on both sides. */
static void reset_recovery(struct disk *k, const char *why)
{
    const uint8_t none[64] = { 0 };
    k->resets++;
    status_t r = usb_control_out_until(k->usb, call_deadline(STEP_MS), REQ_CLASS_IFACE_OUT,
                                       BOT_RESET, 0, k->ifnum, 0, none);
    if (r == ERR_PEER_CLOSED) {
        k->gone = true;
        return;
    }
    status_t in = clear_halt(k, k->ep_in);
    status_t out = k->gone ? ERR_PEER_CLOSED : clear_halt(k, k->ep_out);
    if (k->resets <= 8)
        drv_log("usb-storage %04x:%04x: %s: reset recovery %u (reset %s, clear halt in %s, "
                "out %s)", k->vid, k->pid, why, k->resets, status_str(r), status_str(in),
                status_str(out));
}

/* A step failed with st: the device is gone, or the transport is reset.
 * The status bot_run returns. */
static status_t broken(struct disk *k, status_t st, const char *why)
{
    if (st == ERR_PEER_CLOSED || k->gone) {
        k->gone = true;
        return ERR_PEER_CLOSED;
    }
    reset_recovery(k, why);
    if (k->gone)
        return ERR_PEER_CLOSED;
    return st == ERR_TIMED_OUT ? ERR_TIMED_OUT : ERR_IO;
}

static status_t send_cbw(struct disk *k, const struct bot_cmd *c)
{
    uint32_t at = k->data_max;
    uint8_t *w = k->xbuf + at;
    __builtin_memset(w, 0, CBW_LEN);
    put_le32(w, CBW_SIGNATURE);
    put_le32(w + 4, ++k->tag);
    put_le32(w + 8, c->len);
    w[12] = c->in ? 0x80 : 0x00;   /* bmCBWFlags: the data phase's direction */
    w[13] = 0;                     /* LUN 0 */
    w[14] = c->cdb_len;
    __builtin_memcpy(w + 15, c->cdb, c->cdb_len);
    uint32_t n;
    status_t st = xfer(k, false, at, CBW_LEN, STEP_MS, &n);
    if (st == OK && n != CBW_LEN)
        st = ERR_IO;
    return st;
}

/* The data phase. A STALL ends it (the CSW says how the command went):
 * the halt is cleared and *moved is what is known to have moved, nothing. */
static status_t data_phase(struct disk *k, const struct bot_cmd *c, uint32_t *moved)
{
    status_t st = xfer(k, c->in, c->off, c->len, c->timeout_ms, moved);
    if (st != ERR_IO)
        return st;
    *moved = 0;
    st = clear_halt(k, c->in ? k->ep_in : k->ep_out);
    return st == ERR_PEER_CLOSED ? st : OK;
}

/* Read and check the CSW; *failed: the command failed (status 1). */
static status_t read_csw(struct disk *k, uint32_t timeout_ms, bool *failed)
{
    uint32_t at = k->data_max + 512, n;
    status_t st = xfer(k, true, at, CSW_LEN, timeout_ms, &n);
    if (st == ERR_IO) {   /* a STALL: clear it and ask once more (figure 2) */
        st = clear_halt(k, k->ep_in);
        if (st != ERR_PEER_CLOSED)
            st = xfer(k, true, at, CSW_LEN, timeout_ms, &n);
    }
    if (st != OK)
        return st;
    const uint8_t *s = k->xbuf + at;
    if (n != CSW_LEN || le32(s) != CSW_SIGNATURE || le32(s + 4) != k->tag)
        return ERR_IO;
    if (s[12] != CSW_PASSED && s[12] != CSW_FAILED)
        return ERR_IO;   /* phase error */
    *failed = s[12] == CSW_FAILED;
    return OK;
}

status_t bot_run(struct disk *k, const struct bot_cmd *c, uint32_t *moved, bool *failed)
{
    *moved = 0;
    if (k->gone)
        return ERR_PEER_CLOSED;
    status_t st = send_cbw(k, c);
    if (st != OK) {
        k->cbw_st = st;
        st = broken(k, st, "the CBW was not taken");
        return st == ERR_PEER_CLOSED ? st : ERR_BAD_STATE;
    }
    if (c->len) {
        st = data_phase(k, c, moved);
        if (st != OK)
            return broken(k, st, c->in ? "data in" : "data out");
    }
    st = read_csw(k, c->timeout_ms, failed);
    if (st != OK)
        return broken(k, st, "no valid CSW");
    return OK;
}

unsigned bot_luns(struct disk *k)
{
    uint16_t n = 0;
    static uint8_t data[1024];
    status_t st = usb_control_in_until(k->usb, call_deadline(STEP_MS), REQ_CLASS_IFACE_IN,
                                       BOT_GET_MAX_LUN, 0, k->ifnum, 1, &n, data);
    if (st == ERR_PEER_CLOSED)
        k->gone = true;
    /* A device with one unit may STALL the request (3.2). */
    return st == OK && n == 1 && data[0] < 16 ? data[0] + 1u : 1u;
}
