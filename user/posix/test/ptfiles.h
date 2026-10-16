/* posixtest's files section (files.c, files2.c, fileskid.c): what its
 * three files share. */
#pragma once

#include "pt.h"

#define PT_DIR "/data/ptfiles"   /* the section's own directory on /data, made afresh */

/* Is /data an ext volume (statfs says 0xef53)? The section is written for ext, the default;
 * where FAT's answers differ (no links, no modes, ".." stops at the mount) it checks those
 * too when /data is FAT (a stick made with DATA_FS=fat). */
bool pt_data_ext(void);

/* PT_DIR emptied and made again (after waiting for /data to be mounted):
 * false if there is no /data to test on. */
bool pt_files_start(void);
/* PT_DIR and everything in it removed. */
void pt_files_end(void);
/* Does the namespace have a /svc (files.c)? If not, says so on a line naming `what`, which
 * the caller then skips. */
bool pt_have_svc(const char *what);
/* files2.c: listings, the current directory, the *at calls, stat, mmap. */
void pt_files2(void);
/* fileskid.c: a child given files by a hand-over, and one given /data
 * read-only. */
void pt_files_children(void);
