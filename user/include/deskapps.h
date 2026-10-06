/* The desktop's apps: the one list of what the search box offers and what
 * init will start for the compositor (initctl.launch,
 * docs/G1-PLAN.md "As built: D2b"). init reads it to decide (the
 * compositor names an app, never a path: nothing else can be started that
 * way), the compositor to show the rows (user/services/compositor/menus.c).
 *
 * DESKAPPS(X) calls X(cmd, path, name, about, letter, tint) for each app,
 * in the search box's order:
 *   cmd     what the compositor asks for (initctl.launch's `app`), lower
 *           case; "terminal" is init's `terminal` (a console and a shell),
 *           not a program init launches
 *   path    the program in the boot image (NULL for the terminal)
 *   name    its row's title, and its window's (the busy cursor ends when
 *           a window with this title maps)
 *   about   the row's second line
 *   letter  its tile's letter
 *   tint    its tile's jam colour: 0 raspberry, 1 apricot, 2 blackcurrant
 * An app's program gets what the shell would give it (its own list,
 * <wants.h>) and /svc/wayland, no console. */
#pragma once

#define DESKAPPS(X)                                                        \
    X("terminal", NULL, "Terminal", "Shell", 'T', 0)                       \
    X("jamjar", "bin/jamjar", "Jamjar", "Music player", 'J', 1)

/* Desktop apps init runs at once, at most (initctl.launch). */
#define DESKAPPS_RUNNING 8
