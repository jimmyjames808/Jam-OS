/* jamjar: bin/jamjar, the music player's window (docs/history/MUSIC-GUI.md):
 * the library, now playing, the controls and the jam, with the keys and
 * the mouse; q quits it and the music plays on. It is started as `run`
 * starts a program, plus a duplicate of the shell's end of the player's
 * channel (SR_USER + 4), which is what lets it play. */
#include "sh.h"

SH_CMD(jamjar)
{
    const char *args[20] = { "jamjar" };
    int n = 1;
    for (int i = 1; i < argc && n < 19; i++)
        args[n++] = argv[i];
    args[n] = NULL;
    return sh_run_program_music(n, (char **)args);
}
