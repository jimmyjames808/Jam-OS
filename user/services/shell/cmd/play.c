/* play: a sound file in the headphones. The shell checks the arguments
 * and opens the file; bin/play (user/apps/play) decodes and plays it,
 * holding only that file and the sound output (its list: /svc/audio), so
 * a crafted file on someone's stick reaches nothing else of the shell's.
 * Its lines are the shell's (play: <file>: <format>, play: stats: ...);
 * Ctrl+C asks it to stop, and it fades out and says where.
 *   play [-s] [-v dB | -n] <file.wav|file.mp3>
 * -v: this file's volume (its mixer stream's), at most 0; -s: how it went;
 * -n: decode only, as fast as it goes, and say how fast. */
#include <audio.h>
#include "sh.h"

#define PLAY_PATH "bin/play"

/* argv's options are play's (-s, -n, -v dB); the index of the file, or 0. */
static int options(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        int cb;
        if (!strcmp(argv[i], "-v") && i + 1 < argc && audio_parse_db(argv[i + 1], &cb))
            i++;
        else if (strcmp(argv[i], "-s") && strcmp(argv[i], "-n"))
            return 0;
    }
    return argc == i + 1 ? i : 0;
}

/* The file argv[i] open to read, or false (said why). */
static bool open_file(const char *arg, struct jfile *f)
{
    char abs[SH_PATH_MAX];
    bool dir = false;
    uint64_t size = 0;
    status_t st = sh_resolve(arg, abs, sizeof(abs)) ? sh_stat(abs, &dir, &size) : ERR_NOT_FOUND;
    if (st == ERR_NOT_FOUND) {
        sh_tty("play: %s: no such file\n", arg);
        return false;
    }
    if (st == OK && dir) {
        sh_tty("play: %s: is a directory\n", arg);
        return false;
    }
    if (st == OK)
        st = file_open(abs, FS_READ, f);
    if (st != OK)
        sh_tty("play: %s: can't read it (%s)\n", arg, sh_why(st));
    return st == OK;
}

SH_CMD(play)
{
    int i = options(argc, argv);
    if (!i) {
        sh_tty("usage: play [-s] [-v dB | -n] <file.wav|file.mp3>   (-v -20: 20 dB down; at most 0;\n"
               "       -s: how it went; -n: decode only, as fast as it goes, and say how fast)\n");
        return 2;
    }
    struct jfile f;
    if (!open_file(argv[i], &f))
        return 1;
    struct spawn_handle x[4];
    file_give(&f, &x[0].h, &x[1].h);   /* to bin/play: SR_USER + 0 and + 1 */
    x[0].role = SR_USER + 0;
    x[1].role = SR_USER + 1;
    const char *args[8] = { "play" };
    int n = 1;
    for (int k = 1; k <= i && n < 7; k++)
        args[n++] = argv[k];
    args[n] = NULL;
    sh_flush();
    return sh_run_helper(PLAY_PATH, n, args, x, 2);
}
