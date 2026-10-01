/* splash: what its files share (user/apps/splash).
 *
 *   main.c     the flow: borrow the screen, play, tell init, hold the last
 *              frame, fade out, give the screen back; keys skip
 *   video.c    the video: pl_mpeg's frames, YCbCr to the screen's pixels at
 *              an integer scale, centred (2x on the PC's 2560x1440)
 *   sound.c    the sound on a thread of its own, and the media clock: the
 *              timer until the sound plays, then the sound's position
 *   selftest.c `run splash --selftest`: alpha blending, and the file decoded
 *   plmpeg.c   pl_mpeg's implementation (third_party/pl_mpeg)
 *
 * Media time is in ns from the first frame (frame n is due at
 * n / FPS); the sound's frame f is at f / 48000 s of it: the video and its
 * sound start together in the file (tools/mksplash.sh). */
#pragma once

#define PLM_NO_STDIO
#include <fun.h>
#include <pl_mpeg.h>
#include <splash.h>

#define SPLASH_RATE 48000u   /* the sound's rate: the mixer's, so no resampling */

/* ---- video.c ---------------------------------------------------------------------- */

/* The decoder over the file's bytes (read only; they must stay mapped),
 * laid out for the borrowed screen (scr). ERR_NOT_SUPPORTED: not an MPEG
 * program stream with a video. */
status_t video_open(const uint8_t *mpg, size_t len);
/* The next frame in order, or NULL at the end. */
plm_frame_t *video_next(void);
/* Frames per second (30). */
double   video_fps(void);
/* Draw frame f into scr.s (on the thread pool), not presented. */
void     video_draw(const plm_frame_t *f);
/* Where the video is on the screen, and its scale. */
struct rect video_area(void);
int      video_scale(void);
/* Pixel (x, y) of f as 0xRRGGBB (the conversion video_draw uses). */
uint32_t video_pixel(const plm_frame_t *f, int x, int y);
void     video_close(void);

/* ---- sound.c ---------------------------------------------------------------------- */

/* Media time 0 is now (the first frame is about to be shown). */
void     clock_start(void);
/* Media time now, ns: the sound's position while it plays, else the timer. */
uint64_t clock_now(void);
/* Play the file's sound on a thread of its own through the mixer channel
 * `audio` (SR_AUDIO; 0: silent): decoded whole first, then joined at the
 * media time it is at when the mixer answers. */
void     sound_start(const uint8_t *mpg, size_t len, handle_t audio);
/* Stop it (a key skipped the animation): faded out over 5 ms. */
void     sound_stop(void);
/* Wait until the sound has ended (heard to its end, stopped, or never
 * started), until deadline at most. */
void     sound_wait(uint64_t deadline);
/* The track decoded into 48 kHz stereo frames (for the self-test too):
 * *out (big_alloc'd) and its frame count; ERR_NOT_SUPPORTED: no MP2 track
 * at 48 kHz. */
status_t sound_decode(const uint8_t *mpg, size_t len, int16_t **out, size_t *frames);

/* ---- selftest.c ------------------------------------------------------------------- */

int splash_selftest(const uint8_t *mpg, size_t len);
