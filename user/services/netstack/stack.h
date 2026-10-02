/* netstack's core: lwIP over one network interface, behind a narrow edge
 * (docs/M9-PLAN.md "netstack: lwIP, single-threaded"). stack.c is the only
 * file that sees lwIP; everything here is plain bytes and numbers, so the
 * loop (main.c), the control channel (ctl.c) and the tests
 * (user/tests/utest/netstack.c, which links these objects and drives
 * them in-process) never include an lwIP header.
 *
 * The edge, frames in and frames out, is where a device plugs in:
 * - frames out: lwIP calls the edge's `tx` with each finished Ethernet
 *   frame, untagged (the NIC driver adds the VLAN tag; netstack never
 *   sees one), 60 to 1514 bytes, padded with zeros to the minimum, no
 *   FCS. The frame is netstack's buffer: tx copies it before it returns.
 * - frames in: the device's reader calls stack_input once per received
 *   frame (untagged, no FCS), which lwIP handles to the end before it
 *   returns (an ARP or echo reply goes out through tx meanwhile).
 * - the link: stack_set_link when the device says it changed.
 * Not built yet: the netdev rings (docs/M9-PLAN.md "netdev: rings, not
 * calls"): their reader calls stack_input, their writer is tx. Until then
 * bin/netstack runs with no device (stack_no_device): link down, every
 * frame out dropped and counted.
 *
 * One thread: nothing here locks. Addresses are IPv4 addresses as
 * numbers, the first byte highest (10.2.21.5 is 0x0a021505). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

#define STACK_MTU        1500u   /* IP bytes in one frame */
#define STACK_FRAME_MIN  60u     /* an Ethernet frame without its FCS, at least */
#define STACK_FRAME_MAX  1514u   /* and at most: 14 bytes of header + the MTU */
#define STACK_MAC_LEN    6u

/* Where frames go out (above). OK if the frame was taken; any error counts
 * it as dropped (lwIP is told, and does nothing more about it). */
struct stack_edge {
    status_t (*tx)(void *ctx, const uint8_t *frame, size_t len);
    void    *ctx;
    uint8_t  mac[STACK_MAC_LEN];   /* the device's address: lwIP's source and ARP's */
};

/* The interface's address, all 0 when none is set. */
struct stack_ipv4 {
    uint32_t address;
    uint32_t mask;
    uint32_t gateway;   /* 0: none (only the subnet is reachable) */
};

/* What netctl.info answers about the interface. */
struct stack_state {
    struct stack_ipv4 ip;
    uint8_t mac[STACK_MAC_LEN];
    bool    device;     /* an edge that can send is attached (not stack_no_device) */
    bool    link;       /* the device says the link is up */
};

/* Counts since netstack started (never reset; netctl.stats). */
struct stack_counts {
    uint64_t rx_frames;       /* frames given to stack_input */
    uint64_t rx_refused;      /* of those, refused before lwIP: under 14 or over 1514 bytes, or
                               * no receive buffer free */
    uint64_t tx_frames;       /* frames the edge took */
    uint64_t tx_dropped;      /* frames the edge refused (no device, a full ring) or too long */
    uint64_t echo_replies;    /* ICMP echo replies sent (pings answered) */
    uint64_t icmp_errors;     /* ICMP errors sent (port or protocol unreachable) */
    uint64_t icmp_limited;    /* ICMP errors not sent: over STACK_ICMP_ERR_PER_S */
    /* lwIP's own counts (lwip_stats), by layer: */
    uint32_t link_dropped;    /* Ethernet: an unknown EtherType, a frame too short for its header */
    uint32_t arp_dropped;     /* ARP: malformed, or not for us */
    uint32_t ip_dropped;      /* IPv4: bad header, bad checksum, a fragment, options, not ours */
    uint32_t icmp_dropped;    /* ICMP: bad checksum or length, a type we don't answer */
    uint32_t udp_dropped;     /* UDP: bad checksum or length; no socket on the port */
    uint32_t bad_checksums;   /* IPv4 headers, ICMP and UDP together */
    /* lwIP's memory in use now: back to where it was once every frame is
     * dealt with (a leak shows here). */
    uint32_t rx_buffers_used; /* receive buffers, of lwipopts.h's PBUF_POOL_SIZE */
    uint32_t heap_used;       /* bytes of its heap, of MEM_SIZE */
};

/* ICMP errors (destination unreachable) are rate-limited, as hosts
 * usually do (RFC 1812 4.3.2.8), so a flood of datagrams to closed ports
 * gets at most this many answers a second (after a burst of as many). */
#define STACK_ICMP_ERR_PER_S 10u

/* Start lwIP (once per process) and add the interface on the edge `e`:
 * administratively up, link down, no address. ERR_BAD_STATE: already
 * started. */
status_t stack_start(const struct stack_edge *e);
/* Remove the interface: address, ARP entries and sockets' route gone.
 * lwIP itself stays initialised (stack_start may follow). */
void     stack_stop(void);
/* The edge of a netstack with no device: refuses every frame. */
extern const struct stack_edge stack_no_device;
/* Swap the edge (a device came, went, or restarted with another MAC):
 * the ARP table is flushed if the MAC changed; the address stays. */
void     stack_set_edge(const struct stack_edge *e);
/* The device says the link went up or down. */
void     stack_set_link(bool up);

/* A received frame (above). Never fails: a frame lwIP doesn't want is
 * dropped and counted. */
void     stack_input(const uint8_t *frame, size_t len);
/* Run lwIP's timers that are due (ARP's ageing); the time (ns, absolute)
 * the next one is due, DEADLINE_NEVER if none is: the loop's deadline. */
uint64_t stack_poll(void);

/* Set the address. The caller checked it (ctl.c's ipv4_valid). */
void     stack_set_ipv4(const struct stack_ipv4 *ip);
/* Remove the address and forget every ARP entry. */
void     stack_clear(void);
void     stack_get(struct stack_state *out);
void     stack_get_counts(struct stack_counts *out);

/* ---- port/sys_arch.c --------------------------------------------------------- */

/* A line to the log, "netstack: " in front, rate-limited with lwIP's
 * diagnostics (a burst of 20, then 5 a second; what is dropped is counted
 * on the next line that gets through). */
void nstack_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
