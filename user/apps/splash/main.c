/* splash: the boot splash. The owner's logo animation with its sound,
 * played while Jam OS starts (docs/AS-PLAN.md).
 *
 * init starts it first on a plain boot, right after the console (init's
 * splash.c), with a PROGRAM-level console channel (SR_CONSOLE), init's
 * channel (SR_USER + SPLASH_INIT_ROLE, <splash.h>) and the mixer's `audio`
 * channel (SR_AUDIO). It:
 *   1. borrows the screen and the keys (gfx_open_on: the screen stays the
 *      kernel's dark background, the video's own);
 *   2. plays bootfs's splash.mpg: the video at once (video.c), the sound
 *      as soon as the mixer answers, joined at the animation's current
 *      time (sound.c); each frame is shown when the media clock reaches
 *      it (the sound's position once it plays): late frames are dropped,
 *      none is shown early;
 *   3. a key (any key down) skips the rest: the sound fades out;
 *   4. tells init SPLASH_PLAYED (init starts the shell), holds the last
 *      frame until init says SPLASH_GO (the shell is up) or closes the
 *      channel, at most HOLD_MAX;
 *   5. fades the picture into the console's background and gives the
 *      screen back: the console draws its text.
 * Without init's channel (`run splash` from the shell) it holds the last
 * frame for a second or until a key.
 *
 * `run splash --selftest`: selftest.c. Anything that goes wrong on the way
 * ends it early (the console then draws at once); a panic draws over it
 * whatever it is doing (the kernel's). */
#include "splash_int.h"

#define HOLD_MAX   (30 * NS_PER_S)    /* a shell that never comes: give the screen back */
#define HOLD_ALONE NS_PER_S           /* run from the shell: the last frame this long */
#define FADE_STEPS 8                  /* the fade out: steps of FADE_STEP */
#define FADE_STEP  (30 * NS_PER_MS)
#define CONSOLE_BG 0x101018u          /* the console's text background (its palette's 0) */

static handle_t init_ch;   /* init's channel (0: run from the shell) */

/* Wait until media time `due`, watching the keys. True: a key was pressed. */
static bool wait_until(uint64_t due)
{
    for (;;) {
        uint64_t t = clock_now();
        if (t >= due)
            return false;
        int k = gfx_key(now() + (due - t));
        if (k != KEY_NONE)
            return true;
    }
}

/* Play the video in time with the media clock. True: a key skipped it. */
static bool play(const uint8_t *mpg, size_t len)
{
    plm_frame_t *f = video_next();
    if (!f)
        return false;
    clock_start();
    video_draw(f);
    gfx_present();
    printf("splash: first frame at %lu ms of uptime: %dx%d at %dx on %dx%d\n",
           (unsigned long)(now() / NS_PER_MS), (int)f->width, (int)f->height, video_scale(),
           scr.w, scr.h);
    sound_start(mpg, len, startup_handle(SR_AUDIO));
    double fps = video_fps();
    unsigned shown = 1, dropped = 0, n = 1;
    bool skipped = false;
    for (; !skipped && (f = video_next()); n++) {
        uint64_t due = (uint64_t)(n * (double)NS_PER_S / fps);
        uint64_t next = (uint64_t)((n + 1) * (double)NS_PER_S / fps);
        if (clock_now() >= next) {
            dropped++;   /* its time is over already: on to the next */
            continue;
        }
        video_draw(f);
        skipped = wait_until(due);
        gfx_present();
        shown++;
    }
    printf("splash: %s at %lu ms of the animation: %u frames shown, %u dropped\n",
           skipped ? "skipped by a key" : "played", (unsigned long)(clock_now() / NS_PER_MS),
           shown, dropped);
    return skipped;
}

static void tell_init(uint32_t msg)
{
    if (init_ch)
        (void)jam_channel_write(init_ch, &msg, sizeof(msg), NULL, 0);   /* gone: no matter */
}

/* Hold the last frame until init says go (or is gone), or HOLD_MAX. */
static void hold(void)
{
    if (!init_ch) {
        (void)gfx_key(now() + HOLD_ALONE);
        return;
    }
    signals_t seen = 0;
    status_t st = jam_object_wait_one(init_ch, SIG_READABLE | SIG_PEER_CLOSED, now() + HOLD_MAX,
                                      &seen);
    printf("splash: %s: giving the screen back\n",
           st == ERR_TIMED_OUT ? "no shell yet" : "the shell is up");
}

/* The picture fades into the console's background (premultiplied alpha:
 * step k of n blends 1/(n - k + 1) of the rest, so each step is an even
 * share of the way and the last is the background exactly). */
static void fade_out(void)
{
    for (int k = 1; k <= FADE_STEPS; k++) {
        uint32_t a = 255u / (uint32_t)(FADE_STEPS - k + 1);
        fill_pm(&scr.s, 0, 0, scr.w, scr.h, argb_pm(CONSOLE_BG, a));
        gfx_present();
        jam_nanosleep(now() + FADE_STEP);
    }
}

static status_t find_video(const uint8_t **mpg, size_t *len)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    status_t st = bootfs_default(&fs);
    if (st == OK)
        st = bootfs_lookup(fs, SPLASH_FILE, &data, &size);
    if (st != OK)
        return st;
    *mpg = data;
    *len = (size_t)size;
    return OK;
}

int main(int argc, char **argv)
{
    const uint8_t *mpg = NULL;
    size_t len = 0;
    init_ch = startup_handle(SR_USER + SPLASH_INIT_ROLE);
    pool_start(0);
    status_t st = find_video(&mpg, &len);
    if (has_arg(argc, argv, "--selftest"))
        return splash_selftest(st == OK ? mpg : NULL, len);
    if (st == OK)
        st = gfx_open_on(SPLASH_BG);
    if (st == OK && (st = video_open(mpg, len)) != OK)
        gfx_close();
    if (st != OK) {
        printf("splash: not playing (%s)\n", status_str(st));
        tell_init(SPLASH_PLAYED);
        return 1;
    }
    if (play(mpg, len))
        sound_stop();
    tell_init(SPLASH_PLAYED);
    hold();
    fade_out();
    sound_wait(now() + NS_PER_S);   /* the sound's tail, or its fade */
    gfx_close();
    video_close();
    return 0;
}
