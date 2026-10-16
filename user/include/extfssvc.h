/* Starting the extfs service (bin/extfs, user/services/extfs): the startup
 * handles it takes, for whoever spawns it (volumes, after M13.6's U7a;
 * utest's RAM-disk tests). The roles are fat's (<fatsvc.h>), the same
 * numbers, so a spawner treats the two services alike.
 *
 *   EXTFS_SR_BLOCK   a `block` channel (abi/idl/block.idl): the one
 *                    partition this instance may touch. If its info says
 *                    read-only, every change is refused.
 *   EXTFS_SR_SERVE   the server end of the channel extfs serves `fs` on
 *                    (abi/idl/fs.idl): SR_DRIVER(DR_SERVE), as a driver's.
 *                    When its client end closes, extfs closes every file,
 *                    closes the journal (the volume clean) and exits 0.
 *   EXTFS_SR_CTL     optional: the server end of an `fsctl` channel
 *                    (abi/idl/fsctl.idl): its holder can have extfs stop in
 *                    order (files closed, the journal closed, exit 0).
 *   EXTFS_SR_OWN     optional: the server end of a second `fs` channel, the
 *                    spawner's own, served as EXTFS_SR_SERVE's is; its close
 *                    ends nothing.
 *   EXTFS_SR_PAGER   optional: the mount's pager (abi/syscalls.def
 *                    160-164), with READ WRITE WAIT, the same one for every
 *                    instance: file.map answers VMOs made from it and
 *                    extfs supplies their pages (user/services/extfs/
 *                    pager.c). Without it file.map is ERR_NOT_SUPPORTED.
 *   SR_STATE         optional: its state VMO (<svcstate.h>),
 *                    EXTFS_STATE_SIZE bytes, made and kept by the spawner
 *                    across its restarts, with SVCSTATE_SERVICE_RIGHTS. An
 *                    instance that finds a dead one's state carries on: the
 *                    volume mounted again (the journal's replay is the
 *                    recovery a power cut needs too), its open files and
 *                    views, the request in progress finished exactly once
 *                    (user/services/extfs/adopt.c). Without it extfs makes
 *                    a state of its own.
 *   SR_KEEP          optional: its end of the keep channel (<keep.h>):
 *                    every open file's and view's handles are put with the
 *                    keeper, and a successor is handed them back.
 *
 * A warm spare (SR_STANDBY, <svcstate.h>) is promoted with the handles
 * above; extfs doesn't know it was one.
 *
 * argv[1], if given, names the instance in its log lines ("/jam"). The
 * words after it may be, in any order: EXTFS_ARG_FORMAT; for an instance
 * that replaces one that ended, EXTFS_ARG_KILLED (a deliberate kill) or
 * EXTFS_ARG_CRASHED (any other end): only a crash counts against the
 * request in progress (a request in progress at two crashes is answered
 * ERR_IO and dropped); and a test's powers (user/services/extfs/test.c).
 *
 * Formatting is off unless a word is EXTFS_ARG_FORMAT. Only then, and only
 * on a writable channel, is a blank partition (its first 4 KiB all zeros,
 * which a format cut short leaves too: ext's mkfs writes them last) made an
 * ext volume with Jam OS's feature set (ext's mkfs). Nothing else is ever
 * formatted: not a partition holding anything, not one that fails.
 *
 * Exit codes: 0 nothing left to serve (the fs channel's client closed it,
 * fsctl.stop, or the disk went away); 1 the volume can't be served and a
 * restart may help (a disk error); EXTFS_EXIT_NO_VOLUME there is no ext
 * volume extfs can serve on the partition (and nothing to format);
 * EXTFS_EXIT_TEST a test power ended it. */
#pragma once

#include <jam/startup.h>

#define EXTFS_SR_BLOCK (SR_USER + 0)
#define EXTFS_SR_SERVE (SR_USER + 2)   /* SR_DRIVER(DR_SERVE) */
#define EXTFS_SR_CTL   (SR_USER + 3)
#define EXTFS_SR_OWN   (SR_USER + 4)
#define EXTFS_SR_PAGER (SR_USER + 5)

/* SR_STATE's size: room for extfs's layout (two request slots of 76 KiB,
 * its tables) and its growth. Pages are committed only as they are used. */
#define EXTFS_STATE_SIZE (1ull << 20)

#define EXTFS_ARG_FORMAT     "format-if-blank"
#define EXTFS_ARG_KILLED     "killed"
#define EXTFS_ARG_CRASHED    "crashed"
#define EXTFS_EXIT_NO_VOLUME 2
#define EXTFS_EXIT_TEST      3
