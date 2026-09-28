#pragma once

#include <stdbool.h>
#include <stdint.h>

void cmdline_set(const char *cmdline);
/* True if `word` appears as a whole space-separated word. */
bool cmdline_has(const char *word);
/* Value of key=N as a number; `dflt` if the key is absent, `bare` if it
 * appears without "=N". */
uint64_t cmdline_get_u64(const char *key, uint64_t dflt, uint64_t bare);
