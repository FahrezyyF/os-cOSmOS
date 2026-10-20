#ifndef _COSMOFS_H
#define _COSMOFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * CosmoFS - cOSmOS' own file system (bonus: "File system Non-EXT2 dan Non-FAT32")
 *
 * Design goals: crash safety, contiguous files, and O(1) name lookups.
 *
 *   +--------+------------+---------+---------+----------------+------------+-------------+
 *   | boot   | superblock | journal | bitmap  | node table     | name index | data blocks |
 *   | blk 0  | blk 1      | 40 blks | 1 bit / | 256-byte nodes | hash table | extents     |
 *   |        | (CRC32)    |         | block   | (CRC32 each)   | (open addr)|             |
 *   +--------+------------+---------+---------+----------------+------------+-------------+
 *
 * - Nodes: every file / directory is one 256-byte node holding the metadata required by the
 *   bonus spec (name, size, is_present flag, parent directory) plus timestamps.
 * - Directories own no data blocks: children form a doubly linked list of nodes sorted by
 *   name (first_child / next_sibling / prev_sibling), so `ls` output is always sorted.
 * - Files are stored as extents (start, length) - 8 inline in the node, more in a chain of
 *   extent overflow blocks. The allocator picks the best-fitting free run, so most files
 *   are a single contiguous extent (no fragmentation, fast multi-block reads).
 * - Name index (bonus: File System Indexing): an on-disk open-addressing hash table maps
 *   FNV-1a(name) -> node. It serves (parent, name) lookups and global `find` without
 *   walking the tree, and is rebuilt automatically when tombstones pile up.
 * - Journaling: every operation is one transaction. Modified metadata blocks are first
 *   written to the journal (descriptor + payload + commit record, all CRC32 protected),
 *   then checkpointed to their home location. New data blocks are written before the
 *   commit (ordered mode). After a crash, mount replays the last committed transaction,
 *   so every operation is atomic: it either fully happened or not at all.
 *
 * The driver is a reentrant context (struct CosmoFS) over a pluggable block device, so the
 * same code runs in the kernel (ATA slave disk, cosmo.bin) and in host tools / tests.
 * Like ext2.c, cosmofs.c only includes stdint, stdbool, stddef and string.h.
 */

#define COSMOFS_BLOCK_SIZE         512u
#define COSMOFS_MAGIC              0x4D534F43u // "COSM"
#define COSMOFS_VERSION            1u
#define COSMOFS_NAME_MAX           127u
#define COSMOFS_ROOT_NODE          1u
#define COSMOFS_INLINE_EXTENTS     8u
#define COSMOFS_OVERFLOW_EXTENTS   61u
#define COSMOFS_MAX_OVERFLOW       16u          // per file -> 8 + 16 * 61 extents
#define COSMOFS_JOURNAL_BLOCKS     40u
#define COSMOFS_TXN_MAX_BLOCKS     32u
#define COSMOFS_CACHE_BLOCKS       32u
#define COSMOFS_NODES_PER_BLOCK    (COSMOFS_BLOCK_SIZE / sizeof(struct CosmoFSNode))
#define COSMOFS_SLOTS_PER_BLOCK    (COSMOFS_BLOCK_SIZE / sizeof(struct CosmoFSIndexSlot))
#define COSMOFS_INDEX_EMPTY        0u
#define COSMOFS_INDEX_TOMBSTONE    0xFFFFFFFFu

#define COSMOFS_TYPE_FILE          1u
#define COSMOFS_TYPE_DIR           2u
#define COSMOFS_NODE_PRESENT       0x01u

/* ================================ ON-DISK STRUCTURES ================================ */

struct CosmoFSSuperblock {
    uint32_t magic;
    uint16_t version;
    uint16_t block_size;
    uint32_t total_blocks;
    uint32_t journal_start;
    uint32_t journal_blocks;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t node_table_start;
    uint32_t node_table_blocks;
    uint32_t node_count;
    uint32_t index_start;
    uint32_t index_blocks;
    uint32_t index_slots;
    uint32_t data_start;
    uint32_t free_blocks;
    uint32_t free_nodes;
    uint32_t root_node;
    uint32_t index_tombstones;
    uint32_t journal_sequence;  // sequence of the last checkpointed transaction
    uint32_t mount_count;
    uint32_t created_time;
    uint32_t mount_time;
    uint32_t write_time;
    char     label[32];
    uint32_t checksum;          // CRC32 of all fields above
} __attribute__((packed));

struct CosmoFSExtent {
    uint32_t start;   // first block
    uint32_t length;  // number of blocks
} __attribute__((packed));

struct CosmoFSNode {
    uint8_t  flags;          // COSMOFS_NODE_PRESENT = is_present flag
    uint8_t  type;           // COSMOFS_TYPE_FILE / COSMOFS_TYPE_DIR
    uint8_t  name_len;
    uint8_t  extent_count;   // inline extents in use
    uint32_t id;             // own node number (sanity check)
    uint32_t parent;         // parent directory node
    uint32_t first_child;    // directories: head of the sorted child list
    uint32_t next_sibling;
    uint32_t prev_sibling;
    uint32_t child_count;
    uint32_t size;           // bytes
    uint32_t block_count;    // data blocks
    uint32_t overflow;       // first extent overflow block, 0 = none
    uint32_t ctime;
    uint32_t mtime;
    uint32_t generation;     // incremented every time the node slot is reused
    struct CosmoFSExtent extents[COSMOFS_INLINE_EXTENTS];
    char     name[COSMOFS_NAME_MAX + 1];
    uint8_t  reserved[8];
    uint32_t checksum;       // CRC32 of the node with this field = 0
} __attribute__((packed));

struct CosmoFSExtentBlock {
    uint32_t magic;          // COSMOFS_EXTENT_MAGIC
    uint32_t owner;          // node id
    uint32_t next;           // next overflow block, 0 = last
    uint32_t count;          // extents in use
    struct CosmoFSExtent extents[COSMOFS_OVERFLOW_EXTENTS];
    uint32_t reserved;
    uint32_t checksum;
} __attribute__((packed));

struct CosmoFSIndexSlot {
    uint32_t hash;           // FNV-1a of the name
    uint32_t node;           // 0 = empty, 0xFFFFFFFF = tombstone
} __attribute__((packed));

/* ================================ DRIVER INTERFACE ================================== */

/**
 * Block device used by the driver. All callbacks return false on I/O error.
 * `flush` is a write barrier: every earlier write must be durable when it returns.
 */
struct CosmoFSDevice {
    void     *context;
    uint32_t  block_count;
    bool    (*read)(void *context, uint32_t lba, uint32_t count, void *buf);
    bool    (*write)(void *context, uint32_t lba, uint32_t count, const void *buf);
    bool    (*flush)(void *context);
};

struct CosmoFSCacheEntry {
    bool     valid;
    uint32_t block;
    uint32_t last_use;
    uint8_t  data[COSMOFS_BLOCK_SIZE];
};

struct CosmoFSTxnEntry {
    uint32_t block;
    uint8_t  data[COSMOFS_BLOCK_SIZE];
};

/**
 * One mounted CosmoFS volume. Keep it in static storage (~34 KiB).
 */
struct CosmoFS {
    struct CosmoFSDevice     dev;
    bool                     mounted;
    struct CosmoFSSuperblock sb;
    struct CosmoFSSuperblock sb_backup;          // restored when a transaction aborts
    uint32_t                 (*time_source)(void);
    uint32_t                 free_node_hint;
    uint32_t                 replayed_transactions; // journal replays done by the last mount
    uint32_t                 io_errors;

    struct CosmoFSCacheEntry cache[COSMOFS_CACHE_BLOCKS];
    uint32_t                 cache_clock;

    bool                     txn_active;
    bool                     txn_failed;
    uint32_t                 txn_count;
    struct CosmoFSTxnEntry   txn[COSMOFS_TXN_MAX_BLOCKS];
};

/**
 * Same shape as EXT2DriverRequest so the shell can drive both file systems alike.
 *
 * @param buf          Data buffer (read: destination, write: source)
 * @param name         Entry name, not necessarily null-terminated
 * @param name_len     1 - COSMOFS_NAME_MAX bytes
 * @param parent       Node of the directory that contains the entry
 * @param buffer_size  Size of buf / size of the file to write
 * @param is_directory write(): create a directory
 */
struct CosmoFSRequest {
    void       *buf;
    const char *name;
    uint8_t     name_len;
    uint32_t    parent;
    uint32_t    buffer_size;
    bool        is_directory;
};

/**
 * One record written by cosmofs_read_directory(): "." and ".." first, then the
 * children in name order.
 */
struct CosmoFSDirEntry {
    uint32_t node;
    uint32_t size;
    uint8_t  type;
    uint8_t  name_len;
    char     name[COSMOFS_NAME_MAX + 1]; // null-terminated
} __attribute__((packed));

struct CosmoFSStat {
    uint32_t node;
    uint32_t parent;
    uint8_t  type;
    uint32_t size;
    uint32_t block_count;
    uint32_t extent_count;   // total extents (inline + overflow)
    uint32_t child_count;
    uint32_t ctime;
    uint32_t mtime;
    char     name[COSMOFS_NAME_MAX + 1];
};

struct CosmoFSFindResult {
    uint32_t node;
    uint32_t parent;
    uint8_t  type;
};

struct CosmoFSStats {
    uint32_t total_blocks;
    uint32_t data_blocks;
    uint32_t free_blocks;
    uint32_t node_count;
    uint32_t free_nodes;
    uint32_t index_slots;
    uint32_t index_used;
    uint32_t index_tombstones;
    uint32_t journal_sequence;
    uint32_t mount_count;
    uint32_t replayed_transactions;
    char     label[32];
};

struct CosmoFSCheckReport {
    uint32_t errors;
    uint32_t files;
    uint32_t dirs;
    uint32_t used_blocks;
    uint32_t extents;
    char     message[96];
};

/**
 * Write a fresh, empty CosmoFS (root directory only) onto the device.
 * @param label Volume label (up to 31 chars, may be NULL)
 * @return false on I/O error or when the device is too small (< 512 blocks)
 */
bool cosmofs_format(const struct CosmoFSDevice *dev, const char *label, uint32_t (*time_source)(void));

/**
 * Mount a volume: verify the superblock, replay the journal if the last transaction
 * was committed but not fully checkpointed, then bump the mount counter.
 * @return 0 mounted - 1 no CosmoFS signature (format it) - -1 corrupted / I/O error
 */
int8_t cosmofs_mount(struct CosmoFS *fs, const struct CosmoFSDevice *dev, uint32_t (*time_source)(void));

/**
 * Create a file or directory.
 * @return 0 success - 1 name already exists - 2 invalid parent - -1 other (bad name, no space, I/O)
 */
int8_t cosmofs_write(struct CosmoFS *fs, const struct CosmoFSRequest *request);

/**
 * Read a whole file into request->buf.
 * @return 0 success - 1 not a file - 2 buffer too small - 3 not found - 4 invalid parent - -1 other
 */
int8_t cosmofs_read(struct CosmoFS *fs, const struct CosmoFSRequest *request);

/**
 * List a directory (request->name inside request->parent, or the parent itself for "." / empty name)
 * into request->buf as struct CosmoFSDirEntry records. On success request->buffer_size is set to
 * the number of bytes written.
 * @return 0 success - 1 not a folder - 2 not found - 3 invalid parent - -1 buffer too small
 *         (request->buffer_size then holds the size needed)
 */
int8_t cosmofs_read_directory(struct CosmoFS *fs, struct CosmoFSRequest *request);

/**
 * Delete a file or an empty directory.
 * @return 0 success - 1 not found - 2 folder not empty - 3 invalid parent - -1 other
 */
int8_t cosmofs_delete(struct CosmoFS *fs, const struct CosmoFSRequest *request);

/**
 * Rename / move an entry (atomic, journaled).
 * @return 0 success - 1 source not found - 2 destination exists - 3 invalid folder -
 *         4 folder moved into itself / its subfolder - -1 other
 */
int8_t cosmofs_rename(struct CosmoFS *fs, uint32_t parent, const char *name, uint8_t name_len,
                      uint32_t new_parent, const char *new_name, uint8_t new_name_len);

/**
 * Look up `name` in directory `parent` through the hash index.
 * @return 0 found - 1 not found - 2 invalid parent
 */
int8_t cosmofs_lookup(struct CosmoFS *fs, uint32_t parent, const char *name, uint8_t name_len, struct CosmoFSStat *out);

// Metadata of a node by number, false if the node is not present
bool cosmofs_stat(struct CosmoFS *fs, uint32_t node, struct CosmoFSStat *out);

/**
 * Every node named `name` anywhere in the volume, straight from the hash index.
 * @return number of matches (may exceed max_results)
 */
uint32_t cosmofs_find(struct CosmoFS *fs, const char *name, uint8_t name_len,
                      struct CosmoFSFindResult *results, uint32_t max_results);

void cosmofs_get_stats(struct CosmoFS *fs, struct CosmoFSStats *stats);

/**
 * Full fsck: superblock / node checksums, tree links and order, extent overlap,
 * bitmap and free counters, and that the index matches the tree exactly.
 */
bool cosmofs_check(struct CosmoFS *fs, struct CosmoFSCheckReport *report);

#endif
