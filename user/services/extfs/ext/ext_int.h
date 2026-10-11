/* ext: the library's own declarations, shared by its files and by no one
 * else (the service and the tools include <ext.h> only). */
#pragma once

#include "ext.h"

/* cache.c: carve the cache for fs->block_size from the mounter's region. */
status_t ext_cache_init(struct ext_fs *fs, void *mem, size_t mem_size);

/* group.c: check every group descriptor (ext_mount's last step). */
status_t ext_groups_check(struct ext_fs *fs);

/* inode.c: the directory entry type of an inode's mode. */
enum ext_ftype ext_mode_type(uint16_t mode);

/* map.c: the block map (ext2's and ext3's direct and indirect blocks). */
status_t ext_blockmap(struct ext_fs *fs, struct ext_inode *in, uint32_t lblk, struct ext_run *out);
