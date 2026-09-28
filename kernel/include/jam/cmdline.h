#pragma once

#include <stdbool.h>

void cmdline_set(const char *cmdline);
/* True if `word` appears as a whole space-separated word. */
bool cmdline_has(const char *word);
