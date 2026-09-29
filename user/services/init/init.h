/* init: what its two files share. main.c starts init (the init.cfg
 * programs, keytest); shell.c is the shell mode, where init starts and
 * supervises the console, serialin, devmgr and the shell. */
#pragma once

#include <os.h>

/* One line into the kernel's RESULTS box (and the log). */
void init_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Shell mode (shell.c). Runs for as long as the system does: returns
 * (false) only when init's own port fails. nousb: devmgr leaves the USB
 * controllers alone (the safe mode boot entry). */
bool init_shell(bool nousb);
