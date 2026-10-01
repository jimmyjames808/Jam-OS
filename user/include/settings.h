/* Settings that survive a reboot (user/lib/settings.c): one plain text
 * file on the stick, /data/etc/settings, that the owner can also edit on
 * another computer. One setting a line, `key = value`; blank lines and
 * lines starting with '#' are comments and stay as they are. Keys are
 * letters, digits, '.', '_' and '-' (at most SETTINGS_KEY_MAX - 1);
 * a value is the rest of the line without the spaces around it (UTF-8,
 * no control characters, at most SETTINGS_VALUE_MAX - 1 bytes). A line
 * that is none of these is kept as it is and means nothing; if a key is
 * there twice the last one counts.
 *
 * The keys in use (each read where it is applied):
 *   timezone       the zone times are shown in (<wallclock.h>), default
 *                  Australia/Sydney; init sets the clock with it
 *   rtc            what the PC's real-time clock keeps: local (the zone's
 *                  time, as Windows sets it; the default), utc, or a
 *                  zone's name (Windows set to another zone than ours)
 *   volume         the mixer's master volume, dB (0 the most)
 *   music.folder   what `music start` plays with no folder given
 *   music.volume   the music player's volume, dB
 * The shell's vol, music and date -z write them; init reads them when
 * /data is mounted.
 *
 * Writing never leaves a half-written file where the settings were: the
 * new text goes to settings.new and is synced, then takes the old one's
 * name. A stick pulled between the two leaves only settings.new, which is
 * read in its place. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <jam/status.h>

#define SETTINGS_FILE      "/data/etc/settings"
#define SETTINGS_MAX       16384   /* a bigger file is refused (ERR_OUT_OF_RANGE) */
#define SETTINGS_KEY_MAX   32
#define SETTINGS_VALUE_MAX 240

/* The value of `key` in `file` (SETTINGS_FILE, or a test's) into out (cap
 * bytes, NUL-terminated). ERR_NOT_FOUND: no such key, or no file;
 * ERR_BUFFER_TOO_SMALL: the value doesn't fit; else the file's errors. */
status_t settings_get(const char *file, const char *key, char *out, size_t cap);
/* Set `key` to `value` in `file` (made, with its folder, if missing):
 * its line replaced (the first, others with the key dropped), or added
 * at the end. Nothing is written if it has that value already.
 * ERR_INVALID_ARGS: not a key, or not a value (see the top). */
status_t settings_set(const char *file, const char *key, const char *value);

/* The whole of `file` replaced by text (n bytes), the same safe way (its
 * folder made if missing): init's first, commented settings file. */
status_t settings_write(const char *file, const char *text, size_t n);

/* The parts, on text in memory (n bytes, not NUL-terminated). */
bool     settings_key_ok(const char *key);
bool     settings_value_ok(const char *value);
/* The value of key in text: true and out filled, or false. */
bool     settings_lookup(const char *text, size_t n, const char *key, char *out, size_t cap);
/* text with key set to value, into out (cap bytes); *out_n its length.
 * ERR_BUFFER_TOO_SMALL if it doesn't fit. */
status_t settings_edit(const char *text, size_t n, const char *key, const char *value, char *out,
                       size_t cap, size_t *out_n);
