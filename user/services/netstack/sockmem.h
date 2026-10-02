/* netstack: a socket's ring memory (<sockring.h> "Who holds what"): the
 * VMO netstack makes and maps, and the two events, and what the shares
 * count of it. Shared by the UDP sockets (sock.c) and the TCP ones
 * (tcpsock.c).
 *
 * netstack holds every right; the program gets duplicates with the
 * header's rights (sockmem_handles). Dropping shrinks the VMO to nothing
 * before its handles close, so a program that keeps its handle holds no
 * page charged to netstack. Ring bytes count against the socket's opener
 * (SOCKRING_OPENER_BYTES), its class's share (progs_share_ok) and all
 * sockets (SOCKRING_TOTAL_BYTES). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>
#include <sockring.h>
#include "progs.h"

/* struct sockmem is in progs.h (struct sock holds one). */

/* Make, map and lay out rings of tx and rx bytes with `framing` (netstack's
 * side into *r) and the two events. On a failure what was made is in *m:
 * sockmem_drop it. */
status_t sockmem_make(struct sockmem *m, struct sockring *r, uint32_t framing, uint32_t tx,
                      uint32_t rx);
/* Undo sockmem_make: the to_stack binding on port with key (if bound),
 * the mapping, the VMO shrunk to nothing, the handles. */
void     sockmem_drop(struct sockmem *m, handle_t port, uint64_t key);
/* The program's handles (VMO, to_stack, to_prog) with <sockring.h>'s
 * rights; on a failure none is left. */
status_t sockmem_handles(const struct sockmem *m, handle_t hs[3]);
/* May a socket of opener o (NULL: none, as the DHCP socket's) of class cls
 * have rings of `bytes` more? (o's, the class's share, the total.) */
bool     sockmem_budget_ok(const struct opener *o, uint8_t cls, uint64_t bytes);
