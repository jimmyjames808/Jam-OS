/* <serve.h>: what the shell's `serve` and the file server (bin/serve,
 * user/services/serve) share besides abi/idl/serve.idl: the limits. */
#pragma once

#include <stdint.h>
#include <idl/serve.h>   /* SERVE_NAME_MAX: a name's bytes */
#include <os.h>

#define SERVE_PORT_DEFAULT 8080u
#define SERVE_PORT_MIN     1u                   /* ports a file may be served on: from here (below
                                                 * 1024 with its `svc net listen low`) */
#define SERVE_SHARES       4u                   /* files served at once, each on its port */
#define SERVE_CLIENTS      24u                  /* connections at once, all files together */
#define SERVE_PER_SHARE    16u                  /* ... of them one file's */
