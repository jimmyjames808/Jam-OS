/* ramfs: a filesystem in memory behind the `fs` and `file` protocols, for
 * tests only: the stand-in for a disk's filesystem service, so libos's
 * file calls and the shell's file commands can be tested end to end
 * without USB storage (utest starts it; tools/shell-tests/files.txt runs a
 * shell on top of it). Nothing in a normal boot starts it.
 *
 * ram.c is the filesystem (a table of nodes, each a directory or a file
 * with its bytes on the heap); main.c starts it in one of its modes. */
#pragma once

#include <fsserver.h>

#define RAMFS_NODES    128           /* files and directories, the root included */
#define RAMFS_CAPACITY (4u << 20)    /* bytes of file data in all: past it, ERR_NO_SPACE */

extern const struct fs_ops   ramfs_fs_ops;
extern const struct file_ops ramfs_file_ops;
/* fsserver's `closed`: a file's client is gone. */
void ramfs_closed(struct fsserver_file *f);
/* An empty filesystem: just the root. */
void ramfs_init(void);
