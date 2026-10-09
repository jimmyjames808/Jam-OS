/* <serve.h>: what the shell's `serve` and the file server (bin/serve,
 * user/services/serve) share besides abi/idl/serve.idl: the limits, and
 * the two messages on the channel `share` answers with (the file's
 * handles travel in a plain message, as IDL arguments can't carry them). */
#pragma once

#include <stdint.h>
#include <os.h>

#define SERVE_PORT_DEFAULT 8080u
#define SERVE_PORT_MIN     1u                   /* ports a file may be served on: from here (below
                                                 * 1024 with its `svc net listen low`) */
#define SERVE_SHARES       4u                   /* files served at once, each on its port */
#define SERVE_CLIENTS      24u                  /* connections at once, all files together */
#define SERVE_PER_SHARE    16u                  /* ... of them one file's */
#define SERVE_NAME_MAX     128u                 /* a name's bytes with its NUL (serve.idl's) */
#define SERVE_GIVE_WAIT    (5 * NS_PER_S)       /* for the file on `give` */
#define SERVE_GIVE_MAGIC   0x45565253u          /* "SRVE" */

/* On `give`, from the caller, with two handles: the file's `file` channel
 * (read) and its transfer buffer (file_give's). */
struct serve_give {
    uint32_t magic;      /* SERVE_GIVE_MAGIC */
    uint32_t reserved;   /* 0 */
    uint64_t size;       /* the file's size in bytes, as its opener saw it */
};

/* On `give`, the server's answer. */
struct serve_answer {
    int32_t  status;     /* OK: serving; else why not (serve.idl's share) */
    uint16_t port;       /* the port it listens on */
    uint16_t reserved;   /* 0 */
};
