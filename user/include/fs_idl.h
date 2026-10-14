/* <idl/fs.h> and <idl/file.h> for a program that also uses <os.h>.
 *
 * <os.h>'s file API (file_read, fs_stat, ...: paths through the namespace)
 * and the generated client stubs of the `fs` and `file` protocols (the same
 * names, but taking a channel) can't be declared in one file as they are.
 * Here the stubs that clash get the prefix idl_: idl_fs_stat(ch, ...),
 * idl_file_read(ch, ...), idl_file_map(ch, ...), idl_fs_getattr(ch, ...).
 * Everything else in the two headers keeps its name: fs_open, the _until
 * forms (fs_stat_until, file_read_until, ...), and the server side (struct fs_ops, fs_serve_one, ...). Who needs this:
 * libos's file calls, a server of the protocols (fat, the bootfs server),
 * and tests that call one directly.
 *
 * It also reads and writes fs.readdir's records (fs.idl says their
 * format): fs_dirent_put for a server, fs_dirent_get for a client. */
#pragma once

#include <os.h>

#define fs_stat    idl_fs_stat
#define fs_readdir idl_fs_readdir
#define fs_mkdir   idl_fs_mkdir
#define fs_unlink  idl_fs_unlink
#define fs_rename  idl_fs_rename
#define fs_sync    idl_fs_sync
#define fs_statfs  idl_fs_statfs
#define fs_getattr idl_fs_getattr
#define fs_setattr idl_fs_setattr
#define fs_symlink idl_fs_symlink
#define fs_readlink idl_fs_readlink
#define fs_link    idl_fs_link
#define fs_resolve idl_fs_resolve
#define fs_subview idl_fs_subview
#define file_read  idl_file_read
#define file_write idl_file_write
#define file_sync  idl_file_sync
#define file_stat  idl_file_stat
#define file_truncate idl_file_truncate
#define file_map   idl_file_map
#define file_getattr idl_file_getattr
#define file_setattr idl_file_setattr

#include <idl/file.h>
#include <idl/fs.h>

#undef fs_stat
#undef fs_readdir
#undef fs_mkdir
#undef fs_unlink
#undef fs_rename
#undef fs_sync
#undef fs_statfs
#undef fs_getattr
#undef fs_setattr
#undef fs_symlink
#undef fs_readlink
#undef fs_link
#undef fs_resolve
#undef fs_subview
#undef file_read
#undef file_write
#undef file_sync
#undef file_stat
#undef file_truncate
#undef file_map
#undef file_getattr
#undef file_setattr

/* Is `type` (a record's byte 27) one a record may have, for its is_dir
 * flag: unknown, a directory exactly when is_dir, a file or a link otherwise. */
static inline bool fs_dt_ok(uint8_t type, uint8_t is_dir)
{
    switch (type) {
    case FS_DT_UNKNOWN:
        return true;
    case FS_DT_DIR:
        return is_dir != 0;
    case FS_DT_REG:
    case FS_DT_LNK:
        return is_dir == 0;
    default:
        return false;
    }
}

/* One record of fs.readdir's `entries`. name is not NUL-terminated: it
 * points into the batch (get) or the server's own name (put). */
struct fs_dirent {
    uint64_t    id;
    uint64_t    size;        /* 0 for a directory */
    uint64_t    mtime;
    bool        is_dir;
    uint8_t     type;        /* FS_DT_*; 0: the server doesn't say */
    uint32_t    name_len;    /* 1 .. FS_NAME_LEN */
    const char *name;
};

/* Append e to the batch buf (cap bytes, *len of them used): false, nothing
 * written, if it doesn't fit or its name is empty or over FS_NAME_LEN. */
static inline bool fs_dirent_put(uint8_t *buf, uint32_t cap, uint32_t *len,
                                 const struct fs_dirent *e)
{
    uint32_t need = FS_DIRENT_FIXED + e->name_len;
    if (!e->name_len || e->name_len > FS_NAME_LEN || *len > cap || cap - *len < need)
        return false;
    uint8_t *p = buf + *len;
    uint16_t nl = (uint16_t)e->name_len;
    memcpy(p, &e->id, 8);
    memcpy(p + 8, &e->size, 8);
    memcpy(p + 16, &e->mtime, 8);
    memcpy(p + 24, &nl, 2);
    p[26] = e->is_dir ? 1 : 0;
    p[27] = e->type;
    memcpy(p + FS_DIRENT_FIXED, e->name, e->name_len);
    *len += need;
    return true;
}

/* The record at buf[*at] (the batch is len bytes) into *e, *at moved past
 * it: false if it is malformed (cut short, a name empty, over FS_NAME_LEN,
 * not UTF-8, holding a NUL or a '/', a flag byte not 0 or 1, a type that is
 * not an FS_DT_* or says the opposite of the flag). */
static inline bool fs_dirent_get(const uint8_t *buf, uint32_t len, uint32_t *at,
                                 struct fs_dirent *e)
{
    if (*at > len || len - *at < FS_DIRENT_FIXED)
        return false;
    const uint8_t *p = buf + *at;
    uint16_t nl;
    memcpy(&e->id, p, 8);
    memcpy(&e->size, p + 8, 8);
    memcpy(&e->mtime, p + 16, 8);
    memcpy(&nl, p + 24, 2);
    if (p[26] > 1 || !fs_dt_ok(p[27], p[26]) || !nl || nl > FS_NAME_LEN ||
        len - *at - FS_DIRENT_FIXED < nl)
        return false;
    e->is_dir = p[26];
    e->type = p[27];
    e->name_len = nl;
    e->name = (const char *)p + FS_DIRENT_FIXED;
    if (!idl_str_ok(e->name, nl))
        return false;
    for (uint32_t i = 0; i < nl; i++)
        if (e->name[i] == '/')
            return false;
    *at += FS_DIRENT_FIXED + nl;
    return true;
}
