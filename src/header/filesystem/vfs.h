#ifndef _VFS_H
#define _VFS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Tiny virtual file system layer: one interface over the two mounted volumes
 *   VFS_EXT2    - EXT2 IF2130 Edition on storage.bin (ATA primary master)
 *   VFS_COSMOFS - CosmoFS on cosmo.bin (ATA primary slave)
 * Error codes are the ones of the underlying driver (they share the same tables).
 */

enum VFSKind {
    VFS_EXT2    = 0,
    VFS_COSMOFS = 1,
    VFS_COUNT   = 2,
};

#define VFS_TYPE_FILE 1
#define VFS_TYPE_DIR  2
#define VFS_NAME_MAX  255

struct VFSEntry {
    uint32_t id;     // inode (EXT2) or node (CosmoFS)
    uint32_t size;
    uint8_t  type;   // VFS_TYPE_*
    uint8_t  name_len;
    char     name[VFS_NAME_MAX + 1];
};

struct VFSStat {
    uint32_t id;
    uint32_t parent;
    uint8_t  type;
    uint32_t size;
    uint32_t blocks;        // blocks reserved, including indirect / overflow blocks
    uint32_t extents;       // CosmoFS only (EXT2 reports 0)
    uint32_t links;         // EXT2 only
    uint32_t ctime;
    uint32_t mtime;
};

struct VFSStats {
    const char *fs_name;
    const char *device;
    uint32_t    block_size;
    uint32_t    total_blocks;
    uint32_t    free_blocks;
    uint32_t    total_nodes;
    uint32_t    free_nodes;
    uint32_t    mount_count;
    uint32_t    journal_sequence;   // CosmoFS only
    uint32_t    replayed;           // CosmoFS journal replays at mount
};

struct VFSFindHit {
    uint32_t id;
    uint32_t parent;
    uint8_t  type;
};

/**
 * Detect both ATA drives and mount them, formatting blank disks.
 * @param log Optional callback receiving one human readable line per step
 */
void vfs_mount_all(void (*log)(const char *line));

bool        vfs_available(enum VFSKind kind);
const char *vfs_name(enum VFSKind kind);
uint32_t    vfs_root(enum VFSKind kind);

// 0 found, 1 not found, 2 invalid directory
int8_t   vfs_lookup(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len, struct VFSEntry *out);
bool     vfs_stat(enum VFSKind kind, uint32_t id, struct VFSStat *out);

/**
 * List a directory including "." and "..".
 * @return number of entries written (<= max), or a negative read_directory() error code - 10
 */
int32_t  vfs_list(enum VFSKind kind, uint32_t dir, struct VFSEntry *out, uint32_t max);

int8_t   vfs_read(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len, void *buf, uint32_t buf_size);
int8_t   vfs_write(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len, const void *buf, uint32_t size, bool is_dir);
int8_t   vfs_delete(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len);
int8_t   vfs_rename(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len,
                    uint32_t new_dir, const char *new_name, uint8_t new_len);

// Indexed search by exact name over the whole volume
uint32_t vfs_find(enum VFSKind kind, const char *name, uint8_t len, struct VFSFindHit *hits, uint32_t max);

// Name and parent of a node (false for the root), used to print paths
bool     vfs_name_of(enum VFSKind kind, uint32_t id, uint32_t *parent, char *name_out);

void     vfs_stats(enum VFSKind kind, struct VFSStats *out);

// fsck; message receives the first error (or "clean")
bool     vfs_check(enum VFSKind kind, uint32_t *errors, char *message, uint32_t message_size);

#endif
