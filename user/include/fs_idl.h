/* <idl/fs.h> and <idl/file.h> for a program that also uses <os.h>.
 *
 * <os.h>'s file API (file_read, fs_stat, ...: paths through the namespace)
 * and the generated client stubs of the `fs` and `file` protocols (the same
 * names, but taking a channel) can't be declared in one file as they are.
 * Here the stubs that clash get the prefix idl_: idl_fs_stat(ch, ...),
 * idl_file_read(ch, ...). Everything else in the two headers keeps its
 * name: fs_open, fs_statfs, file_truncate, file_stat, the _until forms,
 * and the server side (struct fs_ops, fs_serve_one, ...). Who needs this:
 * a server of the protocols (fat), and tests that call one directly. */
#pragma once

#include <os.h>

#define fs_stat    idl_fs_stat
#define fs_readdir idl_fs_readdir
#define fs_mkdir   idl_fs_mkdir
#define fs_unlink  idl_fs_unlink
#define fs_rename  idl_fs_rename
#define fs_sync    idl_fs_sync
#define file_read  idl_file_read
#define file_write idl_file_write
#define file_sync  idl_file_sync

#include <idl/file.h>
#include <idl/fs.h>

#undef fs_stat
#undef fs_readdir
#undef fs_mkdir
#undef fs_unlink
#undef fs_rename
#undef fs_sync
#undef file_read
#undef file_write
#undef file_sync
