/* lwIP's options for netstack (docs/M9-PLAN.md "netstack: lwIP,
 * single-threaded"): what is compiled in, and every pool's size. lwIP's
 * own defaults are in third_party/lwip/src/include/lwip/opt.h; only what
 * differs from them, or what matters enough to say out loud, is here.
 *
 * The shape: NO_SYS (lwIP's raw API, no threads, no locks: netstack's one
 * loop is the only caller), IPv4 with ARP, ICMP (echo answered) and UDP;
 * raw ICMP for `ping`. Left out: TCP and IPv6 (no M9 path needs them), IP
 * fragmentation and reassembly (nothing in M9 sends a datagram over 1472
 * bytes, and reassembly is a classic place for bugs: a fragment is
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
 * copy, a port-unreachable, later UDP sends) until the netif's output has
 * copied it to the device, which it does at once; and, with ARP_QUEUEING
 * off, one waiting packet per ARP entry (16 x 1.5 KiB). 64 KiB is room
 * for that with plenty to spare. */
#define MEM_SIZE                (64 * 1024)
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
#define LWIP_TCP                0
#define LWIP_IGMP               0
#define LWIP_DHCP               0    /* user/services/dhcp (planned) */
#define LWIP_AUTOIP             0
#define LWIP_ACD                0
#define LWIP_DNS                0    /* user/services/dns (planned) */
/* With no address yet, a datagram to the DHCP client's port 68 is taken
 * whatever its destination address (a server may answer to the address
 * it offers). Only netctl's DHCP socket can be on port 68: programs get
 * ports from 1024 up. (lwIP's own DHCP, which would set this, is out.) */
#define LWIP_IP_ACCEPT_UDP_PORT(port) ((port) == PP_NTOHS(68))
/* A socket bound to port 0 gets a random port to start from (LWIP_RAND). */
#define LWIP_RANDOMIZE_INITIAL_LOCAL_PORTS 1

/* ---- checksums: all made and all checked, in software ----------------------- */
#define CHECKSUM_GEN_IP         1
#define CHECKSUM_GEN_UDP        1
#define CHECKSUM_GEN_ICMP       1
#define CHECKSUM_CHECK_IP       1
#define CHECKSUM_CHECK_UDP      1
#define CHECKSUM_CHECK_ICMP     1

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
#define TCP_STATS               0
#define IGMP_STATS              0

/* ---- callbacks: none (netstack logs address changes itself) ----------------- */
#define LWIP_NETIF_STATUS_CALLBACK 0
#define LWIP_NETIF_LINK_CALLBACK   0
#define LWIP_NETIF_EXT_STATUS_CALLBACK 0

/* Debug messages stay off: they are per packet (port/arch/cc.h sends
 * lwIP's diagnostics to the log, rate-limited, for when they are on). */
#define LWIP_DBG_TYPES_ON       LWIP_DBG_OFF
