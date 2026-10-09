/* volumes: the disks and their filesystem services (bin/fat), and the
 * mounts they make (user/services/volumes). Its protocols are
 * abi/idl/volumes.idl (<idl/volumes.h>): `volumes` on its control
 * channels, `volesp` on init's ESP channel, `disks` from devmgr, `mounts`
 * on a watch channel. What stays here is how init starts it.
 *
 * Trust: what a channel may ask is fixed by which channel it is. volumes
 * serves
 *   the CONTROL channel (its startup role VOLUMES_SR_CTL; init holds its
 *     client end and publishes it as /svc/volumes, <os.h> SVC_VOLUMES:
 *     each opener gets a control channel of its own, the svc protocol's
 *     connect; tools/checkwants.py lets only user/tests/ ask for it):
 *     `volumes`. volumes runs until every client end of init's control
 *     channel is gone (the openers' own channels and the watches don't
 *     count), or until `volumes.stop`;
 *   the ESP channel (VOLUMES_SR_ESP; init made it, holds the client end
 *     and never hands it on): `volesp`, and nothing of it on any other
 *     channel;
 *   the DISKS channel (VOLUMES_SR_DISKS, then `volumes.set_disks`): the
 *     `disks` events devmgr sends. */
#pragma once

#include <idl/volumes.h>
#include <os.h>

/* volumes' startup roles: the server ends of its control, ESP and DISKS
 * channels, and a duplicate of /svc/notify's shared channel (not under
 * `nocomp`), for the desktop's notices of sticks: "USB stick added",
 * "USB stick removed", "Jam OS stick removed" and "... is back"
 * (<notice.h>). Its fats' jobs are children of its own (SR_JOB). Its
 * words: "bootdisk=0x<id>" and "nospare" (user/services/volumes/main.c). */
#define VOLUMES_SR_CTL    (SR_USER + 0)
#define VOLUMES_SR_ESP    (SR_USER + 1)
#define VOLUMES_SR_DISKS  (SR_USER + 2)
#define VOLUMES_SR_NOTIFY (SR_USER + 3)
