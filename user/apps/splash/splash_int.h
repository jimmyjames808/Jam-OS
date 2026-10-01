/* splash: what its files share (user/apps/splash).
 *
 *   main.c     the flow: borrow the screen, play, tell init, hold the last
 *              frame, fade out, give the screen back; keys skip
 *   video.c    the video: pl_mpeg's frames and their place on the screen
 *              (1:1 on the PC's 2560x1440, scaled down on smaller screens)
 *   draw.c     a frame's YCbCr into the screen's pixels: up, box or bilinear
 *   sound.c    the sound on a thread of its own, and the media clock: the
 *              timer until the sound plays, then the sound's position
 *   selftest.c `run splash --selftest`: alpha blending, and the file decoded
 *   demo.c     `run splash --alpha`: the logo drawn with libfun's alpha calls
 *   plmpeg.c   pl_mpeg's implementation (third_party/pl_mpeg)
 *
 * Media time is in ns from the first frame (frame n is due at
 * n / FPS); the sound's frame f is at f / 48000 s of it (sound_decode
 * lines the two up by the file's time stamps). */
#pragma once

#define PLM_NO_STDIO
#include <fun.h>
#include <pl_mpeg.h>
#include <splash.h>

#define SPLASH_RATE 48000u   /* the sound's rate: the mixer's, so no resampling */

/* ---- video.c, draw.c ---------------------------------------------------------------- */

enum draw_mode { DRAW_UP, DRAW_BOX, DRAW_BILINEAR };

/* How the frame goes onto the screen (video.c chooses, draw.c draws). */
struct video_layout {
    int            w, h;     /* the frame */
    enum draw_mode mode;
    int            n;        /* DRAW_UP: the scale (1: pixel for pixel); DRAW_BOX: the divisor */
    int            x, y;     /* the picture's top left on the screen */
    int            ow, oh;   /* its size there */
};

/* The decoder over the file's bytes (read only; they must stay mapped),
 * laid out for the borrowed screen (scr). ERR_NOT_SUPPORTED: not an MPEG
 * program stream with a video of a sane, even size. */
status_t video_open(const uint8_t *mpg, size_t len);
/* The next frame in order, or NULL at the end. */
plm_frame_t *video_next(void);
/* Frames per second, and the length in seconds (from the file). */
double   video_fps(void);
double   video_seconds(void);
/* Draw frame f into scr.s (on the thread pool), not presented. */
void     video_draw(const plm_frame_t *f);
const struct video_layout *video_layout(void);
/* How a w x h frame goes onto the screen now (scr.w x scr.h). */
void     video_layout_for(int w, int h, struct video_layout *out);
/* The layout in words ("1:1", "2x", "1/2 (box)", "1920x1080 (bilinear)"). */
const char *video_mode(char *buf, size_t n);
/* Pixel (x, y) of f as 0xRRGGBB (its own block's chroma). */
uint32_t video_pixel(const plm_frame_t *f, int x, int y);
void     video_close(void);
/* draw.c: f into scr.s as l says. */
void     draw_frame(const plm_frame_t *f, const struct video_layout *l);

/* ---- sound.c ---------------------------------------------------------------------- */

/* Media time 0 is now. with_sound: the sound's stream is open and starts
 * at 0 too: the clock holds at 0 until it is heard (half a second at most). */
void     clock_start(bool with_sound);
/* The sound's stream: SOUND_PENDING (being decoded or opened),
 * SOUND_READY (open, waiting for the clock), SOUND_NONE (no sound). */
enum { SOUND_PENDING, SOUND_READY, SOUND_NONE };
int      sound_state(void);
/* frames from..from + 25 ms of pcm faded in (a raised cosine). */
void     sound_fade_in(int16_t *pcm, size_t frames, size_t from);
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

/* ---- demo.c ------------------------------------------------------------------------ */

int splash_alpha_demo(void);
