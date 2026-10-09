/* A POSIX program's start (user/posix/crt/): start.c reads the startup
 * message and the hand-over and builds Linux's block for musl; tz.c
 * writes the system's zone as a TZ string. */
#pragma once

#include "stdfd.h"

/* The system's time zone as a POSIX TZ value ("AEST-10AEDT,M10.1.0/2,
 * M4.1.0/3") in buf, cut short to fit cap (cap > 0). */
void tz_posix(char *buf, size_t cap);
