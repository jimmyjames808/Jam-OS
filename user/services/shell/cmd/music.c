/* music: the background music player's controls (abi/idl/music.idl; the
 * player is bin/music, a service init runs, docs/A2-PLAN.md "Music
 * player"). The shell only asks: the player plays in its own job, so the
 * shell goes on meanwhile and Ctrl+C here never reaches it.
 *   music start [folder]   default /data/music; a relative folder is the
 *                          shell's (cd); already playing: the new folder
 *   music stop | next | status | vol [dB] */
#include <idl/music.h>
#include "sh.h"

#define DEFAULT_FOLDER "/data/music"
#define SOON       (5 * NS_PER_S)
#define START_WAIT (60 * NS_PER_S)   /* reads the whole folder, then opens a stream */

static const char *mss(uint64_t ms, char *buf, size_t size)
{
    uint64_t s = ms / 1000;
    snprintf(buf, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    return buf;
}

static int start(handle_t ch, const char *arg)
{
    char abs[SH_PATH_MAX];
    bool dir = false;
    uint64_t size = 0;
    if (!sh_resolve(arg, abs, sizeof(abs)) || strlen(abs) >= FS_PATH_MAX) {
        sh_tty("music: %s: the path is too long\n", arg);
        return 1;
    }
    status_t st = sh_stat(abs, &dir, &size);
    if (st != OK) {
        sh_tty("music: %s: %s\n", arg, st == ERR_NOT_FOUND ? "no such folder" : sh_why(st));
        return 1;
    }
    if (!dir) {
        sh_tty("music: %s: not a folder (play plays one file)\n", arg);
        return 1;
    }
    uint8_t folder[FS_PATH_MAX];
    memset(folder, 0, sizeof(folder));
    memcpy(folder, abs, strlen(abs));
    uint32_t found = 0;
    st = music_start_until(ch, now() + START_WAIT, folder, &found);
    if (st == ERR_NOT_FOUND) {
        sh_tty("music: no audio output: no HD Audio driver with a path to a jack (see `hda`)\n");
        return 1;
    }
    if (st != OK) {
        sh_tty("music: %s: can't play it (%s)\n", abs, sh_why(st));
        return 1;
    }
    if (!found) {
        sh_say("music: no .mp3 or .wav files in %s\n", abs);
        return 1;
    }
    sh_say("music: playing %u track%s from %s in shuffle (music stop stops it)\n", found,
           found == 1 ? "" : "s", abs);
    return 0;
}

static int status(handle_t ch)
{
    uint8_t playing = 0, folder[256], path[256], title[128], note[128];
    uint32_t tracks = 0, bad = 0, started = 0;
    uint64_t elapsed = 0, length = 0;
    int32_t volume = 0;
    status_t st = music_status_until(ch, now() + SOON, &playing, &tracks, &bad, &started,
                                     &elapsed, &length, &volume, folder, path, title, note);
    if (st != OK) {
        sh_tty("music: the player doesn't answer (%s)\n", status_str(st));
        return 1;
    }
    folder[255] = path[255] = title[127] = note[127] = 0;
    char a[16], b[16], v[16];
    if (playing && title[0])
        sh_say("music: playing %s  %s / %s\n  %s\n", (char *)title, mss(elapsed, a, sizeof(a)),
               length ? mss(length, b, sizeof(b)) : "?", (char *)path);
    else if (playing)
        sh_say("music: playing (the first track is on its way)\n");
    else
        sh_say("music: stopped%s%s\n", note[0] ? ": " : "", (char *)note);
    if (folder[0]) {
        sh_say("  folder %s: %u track%s", (char *)folder, tracks, tracks == 1 ? "" : "s");
        if (bad)
            sh_say(" (%u not playable, skipped)", bad);
        sh_say(", %u started\n", started);
    }
    sh_say("  volume %s dB\n", sh_db(volume, v, sizeof(v)));
    return 0;
}

SH_CMD(music)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    bool ok = (!strcmp(cmd, "start") && argc <= 3) ||
              ((!strcmp(cmd, "stop") || !strcmp(cmd, "next") || !strcmp(cmd, "status")) &&
               argc == 2) ||
              (!strcmp(cmd, "vol") && argc <= 3);
    if (!ok) {
        sh_tty("usage: music start [folder] | stop | next | status | vol [dB]\n"
               "       (start: every .mp3 and .wav under the folder, default %s, in\n"
               "       shuffle until `music stop`; the shell stays free meanwhile)\n",
               DEFAULT_FOLDER);
        return 2;
    }
    handle_t ch = sh_music();
    if (!ch) {
        sh_tty("music: there is no music player (init didn't start bin/music)\n");
        return 1;
    }
    if (!strcmp(cmd, "start"))
        return start(ch, argc == 3 ? argv[2] : DEFAULT_FOLDER);
    if (!strcmp(cmd, "status") || (!strcmp(cmd, "vol") && argc == 2))
        return status(ch);
    status_t st;
    if (!strcmp(cmd, "stop")) {
        uint8_t was = 0;
        st = music_stop_until(ch, now() + SOON, &was);
        if (st == OK)
            sh_say("music: %s\n", was ? "stopped" : "not playing");
    } else if (!strcmp(cmd, "next")) {
        st = music_next_until(ch, now() + SOON);
        if (st == ERR_BAD_STATE) {
            sh_say("music: not playing\n");
            return 1;
        }
        if (st == OK)
            sh_say("music: next\n");
    } else {
        int32_t cb = 0, got = 0;
        if (!sh_parse_db(argv[2], &cb)) {
            sh_tty("usage: music vol <dB>   (e.g. music vol -10; 0 dB is the most)\n");
            return 2;
        }
        st = music_set_volume_until(ch, now() + SOON, cb, &got);
        char v[16];
        if (st == OK)
            sh_say("music: volume %s dB\n", sh_db(got, v, sizeof(v)));
    }
    if (st != OK) {
        sh_tty("music: the player doesn't answer (%s)\n", status_str(st));
        return 1;
    }
    return 0;
}
