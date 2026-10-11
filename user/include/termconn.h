/* A connection console: a terminal whose screen and keyboard are a
 * connection (user/services/console/conn.c; docs/M13-PLAN.md, "The
 * terminal layer"). It is bin/console started with the argument "conn"
 * (and, as a window console's, "cols=<n>" and "rows=<n>": the terminal's
 * size until the control channel says another, 80x24 if not given; and
 * "endidle": the console ends by itself once a shell or a program has
 * been on it and none is left, its output sent). Its
 * owner (init's session service, a library's pseudo-terminal) starts it
 * with, besides the ADMIN client channels every console has (SR_USER + 0
 * to 7, the owner keeps the client ends and hands a shell its own through
 * console.new_client):
 *
 *   SR_RESOURCE        optional: a root resource with RIGHT_ROOT_KLOG
 *                      alone, for the kernel log's lines a client asks for
 *                      (console.show_log); without it there are none. No
 *                      screen and no serial port are given or used
 *   CONN_MASTER_ROLE   the master channel, the console's end: required
 *   CONN_CTL_ROLE      the control channel (`conn_ctl`, abi/idl/conn_ctl.idl),
 *                      the console's end: optional
 *
 * The master channel is a pipe of byte messages both ways, as a pipe is
 * (docs/M13-PLAN.md, "Pipes"): no framing, no handles, nothing but bytes.
 *   - Out of the console: what the remote terminal shows, in messages of
 *     at most CONN_OUT_MSG_MAX bytes, in order. The bytes are the programs'
 *     output as it was written (the remote terminal interprets the escape
 *     sequences, the colours and the cursor moves among them) with each
 *     bare "\n" sent as "\r\n"; the kernel log's lines a client asked for
 *     are sent as text, one per line, any control character in them
 *     shown as '?'. When the owner reads too slowly the console stops
 *     serving its clients (their `write`s wait) rather than drop output.
 *   - Into the console: what the remote terminal typed, a message of any
 *     size up to CHANNEL_MAX_BYTES, as xterm sends it (<termkeys.h> turns
 *     it into keys: a printable character, a control character, Enter,
 *     Backspace, Tab, the arrows, Home, End, Page Up and Down, Delete; the
 *     escape sequences of any other key are dropped), UTF-8 for text.
 *     ESC [ 200 ~ and ESC [ 201 ~ around a paste reach a client that
 *     asked for bracketed paste (ESC [ ? 2004 h) as its keys and are
 *     dropped for another.
 *   - The owner closing its end hangs the terminal up: the console's
 *     clients see their channels close (a shell exits with code 2, as when
 *     any console goes away) and the console ends with code 0. The console
 *     ending closes the owner's end the same way.
 *
 * The control channel only carries the size (`conn_ctl.resize`); closing
 * it changes nothing but that the size stays as it was. */
#pragma once

#include <os.h>

/* Startup roles of a connection console (SR_USER + 0..7 are the ADMIN
 * client channels, + 8 and + 9 the window console's init control channel
 * and notices, + 10 its /svc/wayland). */
#define CONN_MASTER_ROLE (SR_USER + 11)
#define CONN_CTL_ROLE    (SR_USER + 12)

/* The most bytes in one message the console sends on the master. */
#define CONN_OUT_MSG_MAX 4096

/* The largest terminal a connection console reports (the window console's
 * limits): a resize beyond them is reported as these. */
#define CONN_COLS_MAX 480
#define CONN_ROWS_MAX 180
