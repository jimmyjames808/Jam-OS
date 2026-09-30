/* Starting the fat service (bin/fat, user/services/fat): the startup
 * handles it takes, for whoever spawns it (devmgr; utest's RAM-disk tests).
 *
 *   FAT_SR_BLOCK   a `block` channel (abi/idl/block.idl): the one partition
 *                  this instance may touch. If its info says read-only, fat
 *                  refuses every change (the /esp instance).
 *   FAT_SR_SERVE   the server end of the channel fat serves `fs` on
 *                  (abi/idl/fs.idl): SR_DRIVER(DR_SERVE), as a driver's.
 *                  When its client end closes, fat closes every file, syncs
 *                  and exits 0.
 *   SR_RESOURCE    optional: the root resource, used only to read the
 *                  real-time clock for file timestamps. Without it every
 *                  timestamp is 2026-01-01 00:00:00.
 *
 * argv[1], if given, names the instance in its log lines ("/data").
 * A writable partition that is blank (no boot signature in its first
 * sector) is formatted, label JAMOS-DATA; one holding anything else fat
 * can't mount is left alone.
 * Exit codes: 0 nothing left to serve (the client closed the fs channel,
 * or the disk went away); 1 the volume can't be served (a restart may
 * help: a disk error; or may not: no FAT volume and nothing to format). */
#pragma once

#include <jam/startup.h>

#define FAT_SR_BLOCK (SR_USER + 0)
#define FAT_SR_SERVE (SR_USER + 2)   /* SR_DRIVER(DR_SERVE) */
