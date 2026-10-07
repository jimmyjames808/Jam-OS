/* The compositor's state VMO (user/services/compositor/wmsave.c,
 * <svcstate.h>): init makes it once a boot and hands it to each
 * compositor it starts (SR_STATE, SVCSTATE_SERVICE_RIGHTS), so a
 * restarted compositor finds the arrangement the dead one left (the
 * screens, the tiles, every window's place) and puts the windows that
 * come back where they were. */
#pragma once

/* Its size: the compositor's layout (svcstate's header and two tiny
 * request slots, then two copies of its description, about 12 KiB) with
 * room to spare; svcstate_open refuses a smaller VMO. Pages are committed
 * only as they are used. */
#define COMP_STATE_SIZE (64ull << 10)
