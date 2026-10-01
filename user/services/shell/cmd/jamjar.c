/* jamjar: bin/jamjar, the music player's window (docs/history/MUSIC-GUI.md):
 * the library, now playing, the controls and the jam, with the keys and
 * the mouse; q quits it and the music plays on. It is `run jamjar`: its
 * list asks for the player (a channel of its own) and the music, read-only. */
#include "sh.h"

SH_CMD(jamjar)
{
    const char *args[20] = { "jamjar" };
    int n = 1;
    for (int i = 1; i < argc && n < 19; i++)
        args[n++] = argv[i];
    args[n] = NULL;
    return sh_run_program(n, (char **)args);
}
