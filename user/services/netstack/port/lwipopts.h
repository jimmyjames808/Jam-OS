/* lwIP's options for netstack (docs/M9-PLAN.md "netstack: lwIP,
 * single-threaded"): what is compiled in, and every pool's size. lwIP's
 * own defaults are in third_party/lwip/src/include/lwip/opt.h; only what
 * differs from them, or what matters enough to say out loud, is here.
 *
 * The shape: NO_SYS (lwIP's raw API, no threads, no locks: netstack's one
 * loop is the only caller), IPv4 with ARP, ICMP (echo answered), UDP and
 * TCP; raw ICMP for `ping`. Left out: IPv6, IP fragmentation and
 * reassembly (nothing sends a datagram over 1472 bytes or a segment over
 * one frame, and reassembly is a classic place for bugs: a fragment is
 * dropped), IGMP, lwIP's DHCP and DNS (each is a process of its own that
 * holds almost nothing), lwIP's VLAN support (the NIC driver adds the tag:
 * netstack never sees one) and every API that needs threads (netconn,
 * sockets).
 *
 * Memory is lwIP's own: a heap of MEM_SIZE bytes and fixed pools, all of
 * it static in netstack's image, so nothing it receives can make it grow:
 * a full pool drops the frame (and counts it) instead. */
#pragma once

/* ---- the system: one thread, no OS layer ---------------------------------- */
#define NO_SYS                  1
#define SYS_LIGHTWEIGHT_PROT    0    /* nothing runs beside the loop: no critical sections */
#define LWIP_TIMERS             1    /* sys_check_timeouts, at the loop's deadline */
#define LWIP_NETCONN            0
#define LWIP_SOCKET             0
#define LWIP_NETIF_API          0

/* ---- memory: static, bounded ----------------------------------------------- */
#define MEM_ALIGNMENT           8
/* The heap holds what lwIP builds to send (PBUF_RAM: ARP, an echo reply's
 * copy, a port-unreachable, UDP sends) until the netif's output has
 * copied it to the device, which it does at once; with ARP_QUEUEING off,
 * one waiting packet per ARP entry (16 x 1.5 KiB); and TCP's copies of
 * the bytes it sent until the peer acks them, at most TCP_SND_BUF a
 * connection. 1 MiB is 64 KiB for the rest and 15 connections sending at
 * full speed at once; past that a connection's bytes wait in its tx ring
 * until acks free the heap (tcp.c retries), so the heap bounds lwIP's
 * memory, not the number of connections. */
#define MEM_SIZE                (1024 * 1024)
/* Received frames: one pbuf each, a whole frame (1514 bytes) in one
 * buffer, so a frame is never a chain. They live only while lwIP looks at
 * them (a frame is handled to the end before the next is read), unless a
 * socket queues one: 128 is a socket's queue (64) twice over. */
#define PBUF_POOL_SIZE          128
#define PBUF_POOL_BUFSIZE       1536
#define MEMP_NUM_PBUF           16   /* pbufs pointing at memory lwIP doesn't own (PBUF_REF) */
#define MEMP_NUM_RAW_PCB        4    /* raw ICMP for programs' pings (one is used) */
/* Programs' sockets (<net.h> NET_SOCKETS_MAX: DNS's names in flight
 * share it with netlog, update and the shell) and the DHCP client's. */
#define MEMP_NUM_UDP_PCB        33

/* ---- the link: Ethernet with ARP, no tags ---------------------------------- */
#define LWIP_ARP                1
#define LWIP_ETHERNET           1
#define ETHARP_SUPPORT_VLAN     0    /* the driver tags (VLAN 21): netstack never sees a tag */
#define ETH_PAD_SIZE            0
/* A home VLAN's /24: the router, the Mac and whoever pings us. A full
 * table recycles its oldest entry, so the size bounds memory, not peers. */
#define ARP_TABLE_SIZE          16
#define ARP_QUEUEING            0    /* one waiting packet per unresolved address, the newest */
#define ETHARP_SUPPORT_STATIC_ENTRIES 0
#define LWIP_SINGLE_NETIF       1    /* one NIC; no loopback interface either */
#define LWIP_HAVE_LOOPIF        0
#define LWIP_NETIF_LOOPBACK     0
#define LWIP_NETIF_HOSTNAME     0

/* ---- IPv4 ------------------------------------------------------------------- */
#define LWIP_IPV4               1
#define LWIP_IPV6               0
#define IP_FORWARD              0
#define IP_REASSEMBLY           0    /* a fragment is dropped */
#define IP_FRAG                 0    /* nothing is sent bigger than the MTU */
#define IP_OPTIONS_ALLOWED      0    /* a datagram with IP options is dropped */
#define IP_DEFAULT_TTL          64
#define IP_SOF_BROADCAST        1    /* a socket sends broadcasts only if it may (DHCP's) */
#define IP_SOF_BROADCAST_RECV   1    /* ... and receives them only then */

/* ---- ICMP, raw, UDP --------------------------------------------------------- */
#define LWIP_ICMP               1    /* echo requests to our address are answered */
#define LWIP_BROADCAST_PING     0    /* not to a broadcast or multicast address */
#define LWIP_MULTICAST_PING     0
#define LWIP_RAW                1
#define LWIP_UDP                1
#define LWIP_UDPLITE            0
#define LWIP_IGMP               0
#define LWIP_DHCP               0    /* a process of its own: user/services/dhcp */
#define LWIP_AUTOIP             0
#define LWIP_ACD                0
#define LWIP_DNS                0    /* a process of its own: user/services/dns */
/* With no address yet, a datagram to the DHCP client's port 68 is taken
 * whatever its destination address (a server may answer to the address
 * it offers). Only netctl's DHCP socket can be on port 68: programs get
 * ports from 1024 up. (lwIP's own DHCP, which would set this, is out.) */
#define LWIP_IP_ACCEPT_UDP_PORT(port) ((port) == PP_NTOHS(68))
/* A socket bound to port 0 gets a random port to start from (LWIP_RAND). */
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1

/* ---- TCP (stack.c's TCP edge, tcp.c's connections) ----------------------------
 * A connection's receive buffer is its socket's rx ring, not lwIP: every
 * segment is copied into the ring as it arrives, and the window lwIP
 * announces is the ring's free room (stack.c sets each connection's
 * window to min(TCP_WND, its rx ring) before its SYN or SYN-ACK, and
 * tcp.c gives window back only as the program reads). So lwIP holds no
 * received bytes, and a slow reader shrinks its own window, nobody
 * else's. What lwIP does hold is the bytes it sent until they are acked
 * (the heap, above), and its pcbs. */
#define LWIP_TCP                1
#define TCP_MSS                 1460  /* a 1500-byte frame: never fragmented (IP_FRAG is off) */
/* The largest window without window scaling in whole segments (44 MSS,
 * what Linux announces unscaled): ~1.3 Gb/s at a LAN's 0.4 ms, ~170 Mb/s
 * over Wi-Fi's 3 ms. A connection with a smaller rx ring gets a smaller
 * one. Scaling stays off: one less option to parse from a peer. */
#define TCP_WND                 (44 * TCP_MSS)
/* Bytes a connection may have queued and unacked in lwIP's heap: as much
 * as a peer's window of the same size. tcp.c takes from the tx ring only
 * what the peer's window allows (and a segment more, for zero-window
 * probes), so a peer that stops reading holds no more than that. */
#define TCP_SND_BUF             (44 * TCP_MSS)
#define TCP_SND_QUEUELEN        (4 * TCP_SND_BUF / TCP_MSS)   /* segments a connection queues */
/* Announce a window as soon as it opened by a segment since the peer last
 * heard of it (RFC 1122's receiver rule: min(MSS, half the buffer), and a
 * ring is at least 4 KiB). lwIP's default, a quarter of TCP_WND, is most
 * of a small ring's whole window: its sender would stall. */
#define TCP_WND_UPDATE_THRESHOLD TCP_MSS
/* lwIP's pcbs: stack.h's STACK_TCP_CONNS (netstack's connections, each a
 * program's, counted and shared out by netstack) plus 128 for half-open
 * connections (a SYN answered: at most STACK_TCP_BACKLOG a listener) and
 * the closing ones netstack let go of (FIN_WAIT, LAST_ACK, TIME_WAIT).
 * Those two kinds have the lowest priority, so when the pool is full lwIP
 * recycles them (the oldest TIME_WAIT first) for a program's new
 * connection, and a flood of SYNs can never take a program's pcb. */
#define MEMP_NUM_TCP_PCB        (256 + 128)
#define MEMP_NUM_TCP_PCB_LISTEN 16    /* stack.h's STACK_TCP_LISTENERS */
#define MEMP_NUM_TCP_SEG        1024  /* queued segments, every connection's (15 full sends) */
#define TCP_LISTEN_BACKLOG      1     /* half-open and not yet accepted count against a listener */
#define TCP_DEFAULT_LISTEN_BACKLOG 16 /* stack.h's STACK_TCP_BACKLOG, the most a listener has */
/* An out-of-order segment is dropped, not queued: the peer resends it
 * (fast retransmit after three duplicate ACKs). On a LAN that costs
 * little, and nothing a peer sends can pin receive buffers. */
#define TCP_QUEUE_OOSEQ         0
#define LWIP_TCP_SACK_OUT       0
#define LWIP_WND_SCALE          0
#define LWIP_TCP_TIMESTAMPS     0
#define LWIP_TCP_KEEPALIVE      0     /* an idle connection is its program's to close */
/* TIME_WAIT is 2 x TCP_MSL: 60 s, as Linux. Their number is bounded by
 * the pool above (the oldest is recycled when a pcb is needed). */
#define TCP_MSL                 30000
#define LWIP_TCP_RTO_TIME       1000  /* the first retransmission after 1 s (RFC 6298) */
/* One slot of per-pcb data: a listener's window for its connections,
 * which stack.c sets on each new one before its SYN-ACK goes. */
#define LWIP_TCP_PCB_NUM_EXT_ARGS 1
/* Initial sequence numbers from the kernel's random source (RFC 6528):
 * lwIP's own is a counter anyone can guess, and a guessed one lets a
 * blind attacker inject into or reset a connection. */
#include <stdint.h>
uint32_t lwport_tcp_isn(void);   /* port/sys_arch.c */
#define LWIP_HOOK_TCP_ISN(local_ip, local_port, remote_ip, remote_port) lwport_tcp_isn()

/* ---- checksums: all made and all checked, in software ----------------------- */
#define CHECKSUM_GEN_IP         1
#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_GEN_ICMP       1
#define CHECKSUM_GEN_TCP        1
#define CHECKSUM_CHECK_IP       1
#define CHECKSUM_CHECK_UDP      1
#define CHECKSUM_CHECK_ICMP     1
#define CHECKSUM_CHECK_TCP      1

/* ---- the counters netctl's stats reads (lwip_stats) ------------------------- */
#define LWIP_STATS              1
#define LWIP_STATS_DISPLAY      0
#define LWIP_STATS_LARGE        1    /* 32-bit counters: 16-bit ones wrap in seconds */
#define LINK_STATS              1
#define ETHARP_STATS            1
#define IP_STATS                1
#define ICMP_STATS              1
#define UDP_STATS               1
#define MEM_STATS               1
#define MEMP_STATS              1
#define SYS_STATS               0
#define IPFRAG_STATS            0
#define TCP_STATS               1
#define IGMP_STATS              0

/* ---- callbacks: none (netstack logs address changes itself) ----------------- */
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK   0
#define LWIP_NETIF_EXT_STATUS_CALLBACK 0

/* Debug messages stay off: they are per packet (port/arch/cc.h sends
 * lwIP's diagnostics to the log, rate-limited, for when they are on). */
#define LWIP_DBG_TYPES_ON       LWIP_DBG_OFF
