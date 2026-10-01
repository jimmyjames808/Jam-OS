/* The kernel command line from the loader: space-separated boot words
 * ("selftest", "nopcid") and key=N values ("stress=60"). Set once at boot;
 * read-only afterwards. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void cmdline_set(const char *cmdline);
const char *cmdline_get(void);
/* True if `word` appears as a whole space-separated word. */
bool cmdline_has(const char *word);
/* Value of key=N as a number; `dflt` if the key is absent, `bare` if it
 * appears without "=N". */
uint64_t cmdline_get_u64(const char *key, uint64_t dflt, uint64_t bare);
/* Value of key=word as a string, cut to size - 1 bytes and NUL-terminated
 * in buf (empty if the key is absent or bare). Returns its length. */
size_t cmdline_get_str(const char *key, char *buf, size_t size);
