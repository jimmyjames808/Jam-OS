/* <serve.h>: what the shell's `serve` and the file server (bin/serve,
 * user/services/serve) share besides abi/idl/serve.idl: the limits. (The
 * `give` channel and its two messages went when `share` took the file's
 * handles as arguments: the protocols review's finding 31.) */
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
