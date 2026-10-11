/*
20260905
Jan Mojzis
Public domain.
*/

#include "buf.h"
#include "bug.h"
#include "packet.h"
#include "packetparser.h"
#include "ssh.h"

int packet_global_request(struct buf *b) {

    long long pos = 0;
    crypto_uint8 ch, wantreply;
    crypto_uint32 requestlen;

    pos = packetparser_uint8(b->buf, b->len, pos,
                             &ch); /* byte SSH_MSG_GLOBAL_REQUEST */
    if (ch != SSH_MSG_GLOBAL_REQUEST) bug_proto();
    pos = packetparser_uint32(b->buf, b->len, pos,
                              &requestlen); /* string request name */
    pos = packetparser_skip(b->buf, b->len, pos, requestlen);
    pos = packetparser_uint8(b->buf, b->len, pos,
                             &wantreply); /* boolean want reply */
    /* Unknown request-specific data, if any, is ignored. */
    (void) pos;

    buf_purge(b);
    if (wantreply) {
        buf_putnum8(b, SSH_MSG_REQUEST_FAILURE);
        packet_put(b);
        buf_purge(b);
    }
    return 1;
}
