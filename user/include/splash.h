/* splash: the boot splash's channel with init (bin/splash, user/apps/splash;
 * init's side is user/services/init/splash.c).
 *
 * On a plain boot init starts the splash first, right after the console,
 * with a channel of its own (the splash's SR_USER + SPLASH_INIT_ROLE).
 * The channel speaks the protocol `splashch` (abi/idl/splashch.idl): two
 * events, no handles:
 *   played  splash -> init: the animation is over (or a key skipped it);
 *           its last frame stays on the screen. init starts the shell now.
 *   go      init -> splash: the shell is ready (initctl.shell_ready): give
 *           the screen back to the console.
 * A closed channel means the same as the message the other side would
 * have sent: init gone (the splash lets go), the splash gone (init starts
 * the shell). */
#pragma once

#include <stdint.h>

#define SPLASH_INIT_ROLE 0u     /* SR_USER + this: the channel */
#define SPLASH_FILE      "splash.mpg"   /* the video, in bootfs */
#define SPLASH_BG        0x1e1a1du      /* the video's background (0xRRGGBB) */
