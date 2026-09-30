/* ramfs's filesystem: a table of nodes. Node 0 is the root directory;
 * every other node names its directory (`parent`) and is a directory or a
 * file whose bytes live in one heap block. Paths arrive cleaned
 * (fs_path_clean: "a/b/c") and are walked a name at a time. Names are
 * compared byte for byte.
 *
 * Bounded: RAMFS_NODES nodes (ERR_NO_RESOURCES past them) and
 * RAMFS_CAPACITY bytes of file data (ERR_NO_SPACE). A file that is open
 * can't be removed (ERR_BAD_STATE), so an open file's node never goes.
 * The errors are the fat service's, so what is tested here holds there. */
#include "ramfs.h"

struct node {
    bool     used;                /* the slot holds a node */
    bool     dir;                 /* a directory */
    int      parent;              /* its directory's node (the root: itself) */
    char     name[FS_PATH_MAX];   /* its name in that directory */
    uint8_t *data;                /* a file's bytes: malloc'd, NULL while cap is 0 */
    uint64_t size, cap;           /* bytes in the file; bytes allocated */
    unsigned opens;               /* open files on it */
};

/* An open file's state (fsserver_file.ctx). */
struct open {
    int      node;    /* which file */
    uint32_t flags;   /* fs.open's */
};

static struct node nodes[RAMFS_NODES];
static uint64_t stored;   /* the files' sizes added up */

void ramfs_init(void)
{
    memset(nodes, 0, sizeof(nodes));
    nodes[0].used = nodes[0].dir = true;
    stored = 0;
}

/* ---- names and paths --------------------------------------------------------------- */

/* The node called name[0..len) in directory dir, or -1. */
static int child(int dir, const char *name, size_t len)
{
    for (int i = 1; i < RAMFS_NODES; i++)
        if (nodes[i].used && nodes[i].parent == dir && strlen(nodes[i].name) == len &&
            !memcmp(nodes[i].name, name, len))
            return i;
    return -1;
}

/* The node a cleaned path names, or -1. */
static int walk(const char *rel)
{
    int at = 0;
    while (*rel && at >= 0) {
        const char *slash = strchr(rel, '/');
        size_t len = slash ? (size_t)(slash - rel) : strlen(rel);
        at = nodes[at].dir ? child(at, rel, len) : -1;
        rel += len + (slash ? 1 : 0);
    }
    return at;
}

/* path cleaned into rel (FS_PATH_MAX bytes), then cut at its last name:
 * *dir is the directory it is in, *name the name. ERR_INVALID_ARGS: no NUL
 * in the field, or the root itself; ERR_NOT_FOUND: no such directory. */
static status_t leaf_of(const uint8_t path[256], char *rel, int *dir, const char **name)
{
    if (fs_path_clean(path, rel) != OK || !rel[0])
        return ERR_INVALID_ARGS;
    char *slash = NULL;
    for (char *p = rel; *p; p++)
        if (*p == '/')
            slash = p;
    *name = slash ? slash + 1 : rel;
    if (slash)
        *slash = '\0';
    *dir = slash ? walk(rel) : 0;
    return *dir >= 0 && nodes[*dir].dir ? OK : ERR_NOT_FOUND;
}

/* A new node called name in dir. */
static status_t create(int dir, const char *name, bool is_dir, int *out)
{
    for (int i = 1; i < RAMFS_NODES; i++) {
        if (nodes[i].used)
            continue;
        memset(&nodes[i], 0, sizeof(nodes[i]));
        nodes[i].used = true;
        nodes[i].dir = is_dir;
        nodes[i].parent = dir;
        memcpy(nodes[i].name, name, strlen(name) + 1);
        *out = i;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

/* ---- a file's bytes ---------------------------------------------------------------- */

/* Make the file size bytes long: cut, or grown with zeros. */
static status_t resize(struct node *n, uint64_t size)
{
    if (size > n->size && size - n->size > RAMFS_CAPACITY - stored)
        return ERR_NO_SPACE;
    if (size > n->cap) {
        uint64_t cap = n->cap ? n->cap : 256;
        while (cap < size)
            cap *= 2;
        uint8_t *data = malloc(cap);
        if (!data)
            return ERR_NO_SPACE;
        if (n->size)
            memcpy(data, n->data, n->size);
        free(n->data);
        n->data = data;
        n->cap = cap;
    }
    if (size > n->size)
        memset(n->data + n->size, 0, size - n->size);
    stored = stored - n->size + size;
    n->size = size;
    return OK;
}

/* ---- fs ---------------------------------------------------------------------------- */

/* The file fs.open means: the one at the path, or a new one (FS_CREATE). */
static status_t open_node(const uint8_t path[256], uint32_t flags, int *out)
{
    char rel[FS_PATH_MAX];
    const char *name;
    int dir;
    status_t st = leaf_of(path, rel, &dir, &name);
    if (st != OK)
        return st;
    int i = child(dir, name, strlen(name));
    if (i < 0 && !(flags & FS_CREATE))
        return ERR_NOT_FOUND;
    if (i < 0)
        st = create(dir, name, false, &i);
    if (st == OK && nodes[i].dir)
        st = ERR_WRONG_TYPE;
    if (st == OK && (flags & FS_TRUNCATE))
        st = resize(&nodes[i], 0);
    *out = i;
    return st;
}

static status_t op_open(void *ctx, const uint8_t path[256], uint32_t flags, handle_t *out_file,
                        handle_t *out_buffer, uint64_t *out_size)
{
    bool write = flags & FS_WRITE;
    if ((flags & ~FS_FLAGS) || !(flags & (FS_READ | FS_WRITE)) ||
        (!write && (flags & (FS_CREATE | FS_TRUNCATE | FS_APPEND))))
        return ERR_INVALID_ARGS;
    int i;
    status_t st = open_node(path, flags, &i);
    if (st != OK)
        return st;
    struct open *o = malloc(sizeof(*o));
    struct fsserver_file *f;
    if (!o)
        return ERR_NO_MEMORY;
    o->node = i;
    o->flags = flags;
    st = fsserver_open(ctx, o, write, out_file, out_buffer, &f);
    if (st != OK) {
        free(o);
        return st;
    }
    nodes[i].opens++;
    *out_size = nodes[i].size;
    return OK;
}

void ramfs_closed(struct fsserver_file *f)
{
    struct open *o = f->ctx;
    nodes[o->node].opens--;
    free(o);
}

static status_t op_stat(void *ctx, const uint8_t path[256], uint64_t *out_size,
                        uint8_t *out_is_dir, uint64_t *out_mtime)
{
    (void)ctx;
    char rel[FS_PATH_MAX];
    if (fs_path_clean(path, rel) != OK)
        return ERR_INVALID_ARGS;
    int i = walk(rel);
    if (i < 0)
        return ERR_NOT_FOUND;
    *out_size = nodes[i].size;
    *out_is_dir = nodes[i].dir;
    *out_mtime = 0;   /* no clock here */
    return OK;
}

static status_t op_readdir(void *ctx, const uint8_t path[256], uint32_t index,
                           uint8_t out_name[256], uint8_t *out_is_dir, uint64_t *out_size)
{
    (void)ctx;
    char rel[FS_PATH_MAX];
    if (fs_path_clean(path, rel) != OK)
        return ERR_INVALID_ARGS;
    int dir = walk(rel);
    if (dir < 0)
        return ERR_NOT_FOUND;
    if (!nodes[dir].dir)
        return ERR_WRONG_TYPE;
    for (int i = 1; i < RAMFS_NODES; i++) {
        if (!nodes[i].used || nodes[i].parent != dir || index-- != 0)
            continue;
        memcpy(out_name, nodes[i].name, strlen(nodes[i].name) + 1);
        *out_is_dir = nodes[i].dir;
        *out_size = nodes[i].size;
        return OK;
    }
    return ERR_NOT_FOUND;
}

static status_t op_mkdir(void *ctx, const uint8_t path[256])
{
    (void)ctx;
    char rel[FS_PATH_MAX];
    const char *name;
    int dir, i;
    status_t st = leaf_of(path, rel, &dir, &name);
    if (st != OK)
        return st;
    if (child(dir, name, strlen(name)) >= 0)
        return ERR_ALREADY_EXISTS;
    return create(dir, name, true, &i);
}

static bool empty_dir(int dir)
{
    for (int i = 1; i < RAMFS_NODES; i++)
        if (nodes[i].used && nodes[i].parent == dir)
            return false;
    return true;
}

static status_t op_unlink(void *ctx, const uint8_t path[256])
{
    (void)ctx;
    char rel[FS_PATH_MAX];
    if (fs_path_clean(path, rel) != OK)
        return ERR_INVALID_ARGS;
    int i = walk(rel);
    if (i < 0)
        return ERR_NOT_FOUND;
    if (i == 0)
        return ERR_ACCESS_DENIED;
    if (nodes[i].opens || (nodes[i].dir && !empty_dir(i)))
        return ERR_BAD_STATE;   /* open, or a directory with entries */
    stored -= nodes[i].size;
    free(nodes[i].data);
    memset(&nodes[i], 0, sizeof(nodes[i]));
    return OK;
}

/* Node i is dir or below it. */
static bool below(int i, int dir)
{
    for (unsigned guard = 0; guard < RAMFS_NODES; guard++) {
        if (i == dir)
            return true;
        if (i == 0)
            return false;
        i = nodes[i].parent;
    }
    return false;
}

static status_t op_rename(void *ctx, const uint8_t from[256], const uint8_t to[256])
{
    (void)ctx;
    char rel_from[FS_PATH_MAX], rel_to[FS_PATH_MAX];
    const char *name;
    int dir;
    if (fs_path_clean(from, rel_from) != OK)
        return ERR_INVALID_ARGS;
    int i = walk(rel_from);
    if (i < 0)
        return ERR_NOT_FOUND;
    status_t st = leaf_of(to, rel_to, &dir, &name);
    if (st != OK)
        return st;
    if (i == 0 || below(dir, i))
        return ERR_INVALID_ARGS;   /* the root, or a directory into itself */
    if (child(dir, name, strlen(name)) >= 0)
        return ERR_ALREADY_EXISTS;
    nodes[i].parent = dir;
    memcpy(nodes[i].name, name, strlen(name) + 1);
    return OK;
}

static status_t op_sync(void *ctx)
{
    (void)ctx;
    return OK;   /* memory is all there is */
}

static status_t op_statfs(void *ctx, uint64_t *out_total, uint64_t *out_free,
                          uint8_t *out_read_only, uint8_t out_label[16])
{
    (void)ctx;
    *out_total = RAMFS_CAPACITY;
    *out_free = RAMFS_CAPACITY - stored;
    *out_read_only = 0;
    memcpy(out_label, "RAMFS", 6);
    return OK;
}

/* ---- file -------------------------------------------------------------------------- */

static struct node *node_of(void *ctx, uint32_t *flags)
{
    const struct open *o = ((struct fsserver_file *)ctx)->ctx;
    *flags = o->flags;
    return &nodes[o->node];
}

static status_t file_op_read(void *ctx, uint64_t offset, uint32_t length, uint32_t *out_actual)
{
    struct fsserver_file *f = ctx;
    uint32_t flags;
    struct node *n = node_of(ctx, &flags);
    if (!(flags & FS_READ))
        return ERR_ACCESS_DENIED;
    if (length > FSSERVER_BUF_SIZE)
        return ERR_INVALID_ARGS;
    uint64_t left = offset < n->size ? n->size - offset : 0;
    uint32_t len = left < length ? (uint32_t)left : length;
    status_t st = len ? jam_vmo_write(f->buf, 0, n->data + offset, len) : OK;
    if (st == OK)
        *out_actual = len;
    return st;
}

static status_t file_op_write(void *ctx, uint64_t offset, uint32_t length, uint32_t *out_actual)
{
    struct fsserver_file *f = ctx;
    uint32_t flags;
    struct node *n = node_of(ctx, &flags);
    if (!(flags & FS_WRITE))
        return ERR_ACCESS_DENIED;
    if (length > FSSERVER_BUF_SIZE)
        return ERR_INVALID_ARGS;
    if (flags & FS_APPEND)
        offset = n->size;
    if (offset > RAMFS_CAPACITY)
        return ERR_NO_SPACE;
    status_t st = offset + length > n->size ? resize(n, offset + length) : OK;
    if (st == OK && length)
        st = jam_vmo_read(f->buf, 0, n->data + offset, length);
    if (st == OK)
        *out_actual = length;
    return st;
}

static status_t file_op_truncate(void *ctx, uint64_t size)
{
    uint32_t flags;
    struct node *n = node_of(ctx, &flags);
    if (!(flags & FS_WRITE))
        return ERR_ACCESS_DENIED;
    return size > RAMFS_CAPACITY ? ERR_NO_SPACE : resize(n, size);
}

static status_t file_op_stat(void *ctx, uint64_t *out_size, uint64_t *out_mtime)
{
    uint32_t flags;
    *out_size = node_of(ctx, &flags)->size;
    *out_mtime = 0;
    return OK;
}

const struct fs_ops ramfs_fs_ops = {
    .open = op_open, .stat = op_stat, .readdir = op_readdir, .mkdir = op_mkdir,
    .unlink = op_unlink, .rename = op_rename, .sync = op_sync, .statfs = op_statfs,
};

const struct file_ops ramfs_file_ops = {
    .read = file_op_read, .write = file_op_write, .truncate = file_op_truncate,
    .stat = file_op_stat, .sync = op_sync,
};
