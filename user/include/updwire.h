/* The update protocol on the wire: UDP to the Mac's port UPDWIRE_PORT
 * (tools/update-server.py serves it; docs/M9-PLAN.md "update: a new
 * build from the Mac"). The encoder and the strict decoder are
 * user/lib/updwire.c; the fetcher's window over them is <updfetch.h>.
 *
 * Stateless: every request names a snapshot, a file, an offset and a
 * length, and its reply carries the same and the bytes, so a lost or
 * repeated datagram costs one retry and the server keeps nothing per
 * client. A snapshot is the build as it was when its manifest was asked
 * for: the server copies the files then, so a `make` running meanwhile
 * can't mix two builds; a request for the manifest with snapshot 0 makes
 * a new one. The server keeps a few recent snapshots; one it dropped
 * answers UPDWIRE_GONE and the fetcher starts again from the manifest.
 *
 * All fields little-endian (<wire.h>).
 *
 * A request, UPDWIRE_REQ_SIZE bytes:
 *     0  u32 magic UPDWIRE_MAGIC       4  u8 version UPDWIRE_VERSION
 *     5  u8  type UPDWIRE_REQUEST      6  u8 file (UPDWIRE_MANIFEST...)
 *     7  u8  reserved, 0               8  u32 snapshot (0: a new one; manifest only)
 *    12  u32 offset                   16  u16 length, 1..UPDWIRE_CHUNK_MAX
 *    18  u16 reserved, 0
 * A reply, UPDWIRE_REP_HDR bytes and then `length` bytes of the file:
 *     0  u32 magic                     4  u8 version
 *     5  u8  type UPDWIRE_REPLY        6  u8 file
 *     7  u8  status (enum updwire_status)
 *     8  u32 snapshot (never 0 in an OK reply)
 *    12  u32 offset                   16  u32 file_size: the whole file's, in this snapshot
 *    20  u16 length: 0 unless OK; offset + length <= file_size
 *    22  u16 reserved, 0
 * The datagram's size is exactly the header's plus length: anything else
 * is not a datagram of this protocol. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <os.h>

#define UPDWIRE_PORT      5022u
#define UPDWIRE_MAGIC     0x4450554au   /* "JUPD" */
#define UPDWIRE_VERSION   1u
#define UPDWIRE_CHUNK_MAX 1400u         /* bytes of a file in one reply */
#define UPDWIRE_REQ_SIZE  20u
#define UPDWIRE_REP_HDR   24u
#define UPDWIRE_REP_MAX   (UPDWIRE_REP_HDR + UPDWIRE_CHUNK_MAX)

enum { UPDWIRE_REQUEST = 1, UPDWIRE_REPLY = 2 };
/* The files: the manifest, then <update.h>'s UPDATE_KERNEL + 1 and
 * UPDATE_BOOTFS + 1. */
enum { UPDWIRE_MANIFEST, UPDWIRE_KERNEL, UPDWIRE_BOOTFS, UPDWIRE_FILES };

enum updwire_status {
    UPDWIRE_OK,        /* the bytes follow */
    UPDWIRE_GONE,      /* no such snapshot (never made, or dropped) */
    UPDWIRE_RANGE,     /* offset or length past the file's end */
    UPDWIRE_BAD,       /* a malformed request */
    UPDWIRE_STATUSES,
};

struct updwire_req {
    uint8_t  file;       /* UPDWIRE_MANIFEST... */
    uint32_t snapshot;   /* 0: a new one (manifest only) */
    uint32_t offset;     /* bytes into the file */
    uint16_t length;     /* 1..UPDWIRE_CHUNK_MAX */
};

struct updwire_rep {
    uint8_t        file;
    uint8_t        status;      /* enum updwire_status */
    uint32_t       snapshot;
    uint32_t       offset;
    uint32_t       file_size;
    uint16_t       length;      /* bytes at data */
    const uint8_t *data;        /* decode: points into the datagram; encode: read from */
};

/* A request into out (UPDWIRE_REQ_SIZE bytes). ERR_INVALID_ARGS if r
 * isn't one the decoder would take (out untouched then). */
status_t updwire_req_encode(const struct updwire_req *r, uint8_t out[UPDWIRE_REQ_SIZE]);
/* A datagram of len bytes as a request, checked field by field.
 * ERR_INVALID_ARGS: not one (wrong size, magic, version, type, file,
 * reserved bits, length; a snapshot of 0 for a file). */
status_t updwire_req_decode(const void *dgram, size_t len, struct updwire_req *out);
/* A reply into out (cap bytes); its size into *len. ERR_INVALID_ARGS: r
 * breaks the rules above; ERR_BUFFER_TOO_SMALL: cap is too small. */
status_t updwire_rep_encode(const struct updwire_rep *r, uint8_t *out, size_t cap, size_t *len);
/* A datagram of len bytes as a reply; out->data points into it.
 * ERR_INVALID_ARGS: not one. */
status_t updwire_rep_decode(const void *dgram, size_t len, struct updwire_rep *out);
