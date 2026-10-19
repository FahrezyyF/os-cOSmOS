#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/stdlib/string.h"
#include "header/filesystem/ext2.h"

/**
 * EXT2 - IF2130 Edition driver.
 * Only disk.h, ext2.h, stdint.h, stdbool.h and string.h are included (book 2.5), so the
 * same file is compiled into the kernel, the host inserter and the host unit tests.
 */

const uint8_t fs_signature[BLOCK_SIZE] = {
    'c', 'O', 'S', 'm', 'O', 'S', ' ', 'E', 'X', 'T', '2', ' ', ' ', ' ', ' ',  ' ',
    'I', 'F', '2', '1', '3', '0', ' ', 'S', 'i', 's', 't', 'e', 'm', ' ', ' ',  ' ',
    'O', 'p', 'e', 'r', 'a', 's', 'i', ' ', '-', ' ', 'I', 'T', 'B', ' ', ' ',  ' ',
    'M', 'a', 'd', 'e', ' ', 'w', 'i', 't', 'h', ' ', '<', '3', ' ', ' ', ' ',  ' ',
    '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '-', '2', '0', '2', '6', '\n',
    [BLOCK_SIZE-2] = 'O',
    [BLOCK_SIZE-1] = 'k',
};

_Static_assert(sizeof(struct EXT2Inode) == 128, "EXT2Inode must match the 128-byte on-disk inode");
_Static_assert(sizeof(struct EXT2DirectoryEntry) == EXT2_DIR_ENTRY_HEADER, "directory entry header must be 8 bytes");
_Static_assert(sizeof(struct EXT2BlockGroupDescriptor) == 32, "group descriptor must be 32 bytes");
_Static_assert(sizeof(struct EXT2Superblock) <= BLOCK_SIZE, "superblock must fit in one block");
_Static_assert(GROUPS_COUNT == 8 && BLOCKS_PER_GROUP == 1024 && INODES_PER_GROUP == 64, "IF2130 geometry");
_Static_assert(sizeof(struct EXT2BlockGroupDescriptorTable) <= BLOCK_SIZE, "BGD table must fit in one block");

#define NO_OFFSET           0xFFFFFFFFu
#define PTR_CACHE_LEVELS    3
#define INDEX_BUCKETS       128u
#define INDEX_NONE          0u

/* ================================ DRIVER STATE ===================================== */

static struct EXT2Superblock               superblock;
static struct EXT2BlockGroupDescriptorTable bgd_table;
static struct BlockBuffer                  block_bitmap[GROUPS_COUNT];
static struct BlockBuffer                  inode_bitmap[GROUPS_COUNT];
static bool                                block_bitmap_dirty[GROUPS_COUNT];
static bool                                inode_bitmap_dirty[GROUPS_COUNT];
static uint32_t                            (*time_source)(void) = NULL;

// Write-back cache of indirect pointer blocks, one slot per indirection level
struct PointerCache {
    bool     valid;
    bool     dirty;
    uint32_t block;
    uint32_t ptr[EXT2_POINTERS_PER_BLOCK];
};
static struct PointerCache ptr_cache[PTR_CACHE_LEVELS];

// In-memory name index (file system indexing bonus): one node per inode number
struct NameIndexNode {
    bool     used;
    uint8_t  file_type;
    uint16_t next;   // next inode in the same bucket, INDEX_NONE terminates
    uint32_t parent;
    uint32_t hash;
};
static struct NameIndexNode name_index[EXT2_INODES_COUNT + 1];
static uint16_t             name_index_bucket[INDEX_BUCKETS];

// Location of a directory entry found by dir_find()
struct DirLocation {
    uint32_t block;       // physical block holding the entry
    uint32_t offset;      // entry offset inside the block
    uint32_t prev_offset; // previous entry in the same block, NO_OFFSET if first
};

/* ================================ SMALL HELPERS ==================================== */

static uint32_t now(void) {
    return time_source ? time_source() : 0;
}

static uint32_t div_ceil(uint32_t a, uint32_t b) {
    return a / b + (a % b != 0);
}

// Bitmaps use MSB-first bit order inside each byte, like the reference implementation
static bool bit_get(const uint8_t *bitmap, uint32_t i) {
    return (bitmap[i >> 3] >> (7 - (i & 7))) & 1;
}

static void bit_set(uint8_t *bitmap, uint32_t i) {
    bitmap[i >> 3] |= (uint8_t) (0x80 >> (i & 7));
}

static void bit_clear(uint8_t *bitmap, uint32_t i) {
    bitmap[i >> 3] &= (uint8_t) ~(0x80 >> (i & 7));
}

static uint32_t fnv1a(const char *name, uint8_t len) {
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < len; i++) {
        h ^= (uint8_t) name[i];
        h *= 16777619u;
    }
    return h;
}

static bool is_dot_or_dotdot(const char *name, uint8_t len) {
    return (len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.');
}

// Valid name for a new entry: 1-255 bytes, no '/', no '\0', not "." / ".."
static bool is_valid_new_name(const char *name, uint8_t len) {
    if (name == NULL || len == 0 || is_dot_or_dotdot(name, len))
        return false;
    for (uint8_t i = 0; i < len; i++)
        if (name[i] == '/' || name[i] == '\0')
            return false;
    return true;
}

/* ================================ GEOMETRY ========================================= */

uint32_t inode_to_bgd(uint32_t inode) {
    return (inode - 1) / INODES_PER_GROUP;
}

uint32_t inode_to_local(uint32_t inode) {
    return (inode - 1) % INODES_PER_GROUP;
}

// Group 0 starts after the boot sector, superblock and BGD table
static uint32_t group_meta_start(uint32_t group) {
    return group * BLOCKS_PER_GROUP + (group == 0 ? 3u : 0u);
}

static uint32_t group_block_bitmap(uint32_t group) {
    return group_meta_start(group);
}

static uint32_t group_inode_bitmap(uint32_t group) {
    return group_meta_start(group) + 1;
}

static uint32_t group_inode_table(uint32_t group) {
    return group_meta_start(group) + 2;
}

static uint32_t group_first_data_index(uint32_t group) {
    return group_meta_start(group) + 2 + INODES_TABLE_BLOCK_COUNT - group * BLOCKS_PER_GROUP;
}

static bool is_data_block(uint32_t block) {
    if (block == 0 || block >= EXT2_BLOCKS_COUNT)
        return false;
    uint32_t group = block / BLOCKS_PER_GROUP;
    return block % BLOCKS_PER_GROUP >= group_first_data_index(group);
}

static bool inode_number_valid(uint32_t inode) {
    return inode >= 1 && inode <= EXT2_INODES_COUNT;
}

static bool inode_allocated(uint32_t inode) {
    return inode_number_valid(inode) && bit_get(inode_bitmap[inode_to_bgd(inode)].buf, inode_to_local(inode));
}

// Pointer blocks needed for a file with `n` data blocks
static uint32_t meta_blocks_for(uint32_t n) {
    const uint32_t P = EXT2_POINTERS_PER_BLOCK;
    if (n <= EXT2_NDIR_BLOCKS)
        return 0;
    n -= EXT2_NDIR_BLOCKS;
    uint32_t meta = 1; // single indirect
    if (n <= P)
        return meta;
    n -= P;
    uint32_t d = n < P * P ? n : P * P;
    meta += 1 + div_ceil(d, P); // doubly indirect + its pointer blocks
    n -= d;
    if (n == 0)
        return meta;
    meta += 1 + div_ceil(n, P * P) + div_ceil(n, P); // triply indirect tree
    return meta;
}

static uint32_t max_file_blocks(void) {
    const uint32_t P = EXT2_POINTERS_PER_BLOCK;
    return EXT2_NDIR_BLOCKS + P + P * P + P * P * P;
}

/* ================================ METADATA I/O ===================================== */

static void read_inode(uint32_t inode, struct EXT2Inode *out) {
    uint32_t group = inode_to_bgd(inode), local = inode_to_local(inode);
    struct BlockBuffer b;
    read_blocks(&b, group_inode_table(group) + local / INODES_PER_TABLE, 1);
    memcpy(out, b.buf + (local % INODES_PER_TABLE) * INODE_SIZE, INODE_SIZE);
}

void sync_node(struct EXT2Inode *node, uint32_t inode) {
    uint32_t group = inode_to_bgd(inode), local = inode_to_local(inode);
    uint32_t block = group_inode_table(group) + local / INODES_PER_TABLE;
    struct BlockBuffer b;
    read_blocks(&b, block, 1);
    memcpy(b.buf + (local % INODES_PER_TABLE) * INODE_SIZE, node, INODE_SIZE);
    write_blocks(&b, block, 1);
}

static bool is_directory_inode(const struct EXT2Inode *node) {
    return (node->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
}

static bool is_regular_inode(const struct EXT2Inode *node) {
    return (node->i_mode & EXT2_S_IFMT) == EXT2_S_IFREG;
}

// A usable directory: allocated inode whose mode says directory
static bool load_directory(uint32_t inode, struct EXT2Inode *out) {
    if (!inode_allocated(inode))
        return false;
    read_inode(inode, out);
    return is_directory_inode(out);
}

// Write superblock, group descriptors and dirty bitmaps ("commit" in book 2.6.2)
static void commit_metadata(void) {
    struct BlockBuffer b;
    superblock.s_wtime = now();
    superblock.s_state = EXT2_VALID_FS;

    memset(&b, 0, sizeof(b));
    memcpy(b.buf, &superblock, sizeof(superblock));
    write_blocks(&b, EXT2_SUPERBLOCK_LOCATION, 1);

    memset(&b, 0, sizeof(b));
    memcpy(b.buf, &bgd_table, sizeof(bgd_table));
    write_blocks(&b, EXT2_BGD_TABLE_LOCATION, 1);

    for (uint32_t g = 0; g < GROUPS_COUNT; g++) {
        if (block_bitmap_dirty[g]) {
            write_blocks(&block_bitmap[g], group_block_bitmap(g), 1);
            block_bitmap_dirty[g] = false;
        }
        if (inode_bitmap_dirty[g]) {
            write_blocks(&inode_bitmap[g], group_inode_bitmap(g), 1);
            inode_bitmap_dirty[g] = false;
        }
    }
}

/* ================================ ALLOCATION ======================================= */

static void ptr_cache_flush(void);

// Every public operation starts from an empty pointer cache, so no stale copy of a
// block freed by an earlier operation can ever be used
static void op_begin(void) {
    ptr_cache_flush();
}

static uint32_t alloc_block(uint32_t preferred_group) {
    for (uint32_t k = 0; k < GROUPS_COUNT; k++) {
        uint32_t group = (preferred_group + k) % GROUPS_COUNT;
        if (bgd_table.table[group].bg_free_blocks_count == 0)
            continue;
        uint8_t *bitmap = block_bitmap[group].buf;
        for (uint32_t i = group_first_data_index(group); i < BLOCKS_PER_GROUP; i++) {
            if ((i & 7) == 0 && bitmap[i >> 3] == 0xFF) {
                i += 7; // whole byte used, skip it
                continue;
            }
            if (!bit_get(bitmap, i)) {
                bit_set(bitmap, i);
                block_bitmap_dirty[group] = true;
                bgd_table.table[group].bg_free_blocks_count--;
                superblock.s_free_blocks_count--;
                return group * BLOCKS_PER_GROUP + i;
            }
        }
    }
    return 0;
}

static void free_block(uint32_t block) {
    if (!is_data_block(block))
        return;
    uint32_t group = block / BLOCKS_PER_GROUP, index = block % BLOCKS_PER_GROUP;
    if (!bit_get(block_bitmap[group].buf, index))
        return; // already free, never double count
    bit_clear(block_bitmap[group].buf, index);
    block_bitmap_dirty[group] = true;
    bgd_table.table[group].bg_free_blocks_count++;
    superblock.s_free_blocks_count++;
}

uint32_t allocate_node(void) {
    for (uint32_t group = 0; group < GROUPS_COUNT; group++) {
        if (bgd_table.table[group].bg_free_inodes_count == 0)
            continue;
        for (uint32_t i = 0; i < INODES_PER_GROUP; i++) {
            if (!bit_get(inode_bitmap[group].buf, i)) {
                bit_set(inode_bitmap[group].buf, i);
                inode_bitmap_dirty[group] = true;
                bgd_table.table[group].bg_free_inodes_count--;
                superblock.s_free_inodes_count--;
                return group * INODES_PER_GROUP + i + 1;
            }
        }
    }
    return 0;
}

static void free_inode_number(uint32_t inode) {
    if (!inode_allocated(inode))
        return;
    uint32_t group = inode_to_bgd(inode);
    bit_clear(inode_bitmap[group].buf, inode_to_local(inode));
    inode_bitmap_dirty[group] = true;
    bgd_table.table[group].bg_free_inodes_count++;
    superblock.s_free_inodes_count++;
}

/* ================================ BLOCK MAPPING ==================================== */

static void ptr_cache_flush(void) {
    for (uint32_t level = 0; level < PTR_CACHE_LEVELS; level++) {
        if (ptr_cache[level].valid && ptr_cache[level].dirty)
            write_blocks(ptr_cache[level].ptr, ptr_cache[level].block, 1);
        ptr_cache[level].dirty = false;
        ptr_cache[level].valid = false;
    }
}

static uint32_t *ptr_cache_load(uint32_t level, uint32_t block) {
    struct PointerCache *slot = &ptr_cache[level];
    if (slot->valid && slot->block == block)
        return slot->ptr;
    if (slot->valid && slot->dirty)
        write_blocks(slot->ptr, slot->block, 1);
    read_blocks(slot->ptr, block, 1);
    slot->valid = true;
    slot->dirty = false;
    slot->block = block;
    return slot->ptr;
}

static uint32_t *ptr_cache_new(uint32_t level, uint32_t block) {
    struct PointerCache *slot = &ptr_cache[level];
    if (slot->valid && slot->dirty)
        write_blocks(slot->ptr, slot->block, 1);
    memset(slot->ptr, 0, sizeof(slot->ptr));
    slot->valid = true;
    slot->dirty = true;
    slot->block = block;
    return slot->ptr;
}

/**
 * Translate a logical block index into (i_block slot, depth, path through pointer blocks)
 * @return false if the index is beyond the triply indirect range
 */
static bool bmap_path(uint32_t index, uint32_t *slot, uint32_t *depth, uint32_t path[3]) {
    const uint32_t P = EXT2_POINTERS_PER_BLOCK;
    if (index < EXT2_NDIR_BLOCKS) {
        *slot  = index;
        *depth = 0;
        return true;
    }
    index -= EXT2_NDIR_BLOCKS;
    if (index < P) {
        *slot   = EXT2_IND_BLOCK;
        *depth  = 1;
        path[0] = index;
        return true;
    }
    index -= P;
    if (index < P * P) {
        *slot   = EXT2_DIND_BLOCK;
        *depth  = 2;
        path[0] = index / P;
        path[1] = index % P;
        return true;
    }
    index -= P * P;
    if (index < P * P * P) {
        *slot   = EXT2_TIND_BLOCK;
        *depth  = 3;
        path[0] = index / (P * P);
        path[1] = (index / P) % P;
        path[2] = index % P;
        return true;
    }
    return false;
}

// Physical block of logical block `index`, 0 for a hole / invalid pointer
static uint32_t bmap_get(const struct EXT2Inode *node, uint32_t index) {
    uint32_t slot, depth, path[3];
    if (!bmap_path(index, &slot, &depth, path))
        return 0;
    uint32_t block = node->i_block[slot];
    for (uint32_t level = 0; level < depth; level++) {
        if (!is_data_block(block))
            return 0;
        block = ptr_cache_load(level, block)[path[level]];
    }
    return is_data_block(block) ? block : 0;
}

/**
 * Store `block` as logical block `index` of the node, allocating (zeroed) pointer
 * blocks on demand. Every allocated pointer block is counted in node->i_blocks.
 * @return false if a pointer block could not be allocated
 */
static bool bmap_set(struct EXT2Inode *node, uint32_t index, uint32_t block, uint32_t preferred_group) {
    uint32_t slot, depth, path[3];
    if (!bmap_path(index, &slot, &depth, path))
        return false;
    if (depth == 0) {
        node->i_block[slot] = block;
        return true;
    }

    if (!is_data_block(node->i_block[slot])) {
        uint32_t fresh = alloc_block(preferred_group);
        if (fresh == 0)
            return false;
        ptr_cache_new(0, fresh);
        node->i_block[slot] = fresh;
        node->i_blocks++;
    }
    uint32_t current = node->i_block[slot];
    for (uint32_t level = 0; level < depth; level++) {
        uint32_t *ptrs = ptr_cache_load(level, current);
        if (level + 1 == depth) {
            ptrs[path[level]]       = block;
            ptr_cache[level].dirty  = true;
            return true;
        }
        if (!is_data_block(ptrs[path[level]])) {
            uint32_t fresh = alloc_block(preferred_group);
            if (fresh == 0)
                return false;
            ptrs[path[level]]      = fresh; // ptrs still points into slot `level`
            ptr_cache[level].dirty = true;
            ptr_cache_new(level + 1, fresh);
            node->i_blocks++;
        }
        current = ptrs[path[level]];
    }
    return true;
}

// Data blocks of a node. Directories written by the reference implementation keep
// i_size == 0, so fall back to the number of data blocks implied by i_blocks.
static uint32_t node_data_blocks(const struct EXT2Inode *node) {
    uint32_t by_size = div_ceil(node->i_size, BLOCK_SIZE);
    if (by_size > 0 || !is_directory_inode(node))
        return by_size;
    uint32_t data = node->i_blocks;
    while (data > 0 && data + meta_blocks_for(data) > node->i_blocks)
        data--;
    return data;
}

static void free_tree(uint32_t block, uint32_t depth, uint32_t *remaining) {
    if (*remaining == 0)
        return;
    if (depth == 0) {
        free_block(block);
        (*remaining)--;
        return;
    }
    uint32_t capacity = 1;
    for (uint32_t d = 1; d < depth; d++)
        capacity *= EXT2_POINTERS_PER_BLOCK;
    if (!is_data_block(block)) {
        *remaining = *remaining > capacity * EXT2_POINTERS_PER_BLOCK ? *remaining - capacity * EXT2_POINTERS_PER_BLOCK : 0;
        return;
    }
    uint32_t ptrs[EXT2_POINTERS_PER_BLOCK];
    read_blocks(ptrs, block, 1);
    for (uint32_t i = 0; i < EXT2_POINTERS_PER_BLOCK && *remaining > 0; i++) {
        if (is_data_block(ptrs[i]))
            free_tree(ptrs[i], depth - 1, remaining);
        else
            *remaining = *remaining > capacity ? *remaining - capacity : 0; // hole
    }
    free_block(block);
}

void deallocate_blocks(void *loc, uint32_t blocks) {
    uint32_t *slots     = (uint32_t*) loc;
    uint32_t  remaining = blocks;
    for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS && remaining > 0; i++, remaining--)
        free_block(slots[i]);
    for (uint32_t depth = 1; depth <= 3 && remaining > 0; depth++)
        free_tree(slots[EXT2_NDIR_BLOCKS + depth - 1], depth, &remaining);
}

void allocate_node_blocks(void *ptr, struct EXT2Inode *node, uint32_t prefered_bgd) {
    const uint8_t *src     = (const uint8_t*) ptr;
    uint32_t       nblocks = div_ceil(node->i_size, BLOCK_SIZE);
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t block = alloc_block(prefered_bgd);
        if (block == 0)
            return; // callers pre-check free space, so this only happens on a corrupted bitmap
        uint32_t remaining = node->i_size - i * BLOCK_SIZE;
        if (remaining >= BLOCK_SIZE) {
            write_blocks(src + i * BLOCK_SIZE, block, 1);
        } else {
            struct BlockBuffer tail;
            memset(&tail, 0, sizeof(tail));
            memcpy(tail.buf, src + i * BLOCK_SIZE, remaining);
            write_blocks(&tail, block, 1);
        }
        node->i_blocks++;
        if (!bmap_set(node, i, block, prefered_bgd))
            return;
    }
}

// Copy `size` bytes of file data into dst, batching physically contiguous blocks
static bool read_node_data(const struct EXT2Inode *node, uint8_t *dst, uint32_t size) {
    uint32_t full_blocks = size / BLOCK_SIZE;
    uint32_t i = 0;
    while (i < full_blocks) {
        uint32_t block = bmap_get(node, i);
        if (block == 0) {
            memset(dst + i * BLOCK_SIZE, 0, BLOCK_SIZE); // sparse hole
            i++;
            continue;
        }
        uint32_t run = 1;
        while (i + run < full_blocks && run < 128 && bmap_get(node, i + run) == block + run)
            run++;
        read_blocks(dst + i * BLOCK_SIZE, block, (uint8_t) run);
        i += run;
    }
    uint32_t tail = size % BLOCK_SIZE;
    if (tail != 0) {
        struct BlockBuffer last;
        uint32_t block = bmap_get(node, full_blocks);
        if (block == 0)
            memset(&last, 0, sizeof(last));
        else
            read_blocks(&last, block, 1);
        memcpy(dst + full_blocks * BLOCK_SIZE, last.buf, tail);
    }
    return true;
}

/* ================================ DIRECTORY ENTRIES ================================ */

char *get_entry_name(void *entry) {
    return (char*) entry + EXT2_DIR_ENTRY_HEADER;
}

struct EXT2DirectoryEntry *get_directory_entry(void *ptr, uint32_t offset) {
    return (struct EXT2DirectoryEntry*) ((uint8_t*) ptr + offset);
}

struct EXT2DirectoryEntry *get_next_directory_entry(struct EXT2DirectoryEntry *entry) {
    return get_directory_entry(entry, entry->rec_len);
}

uint16_t get_entry_record_len(uint8_t name_len) {
    return (uint16_t) ((EXT2_DIR_ENTRY_HEADER + name_len + 3u) & ~3u);
}

uint32_t get_dir_first_child_offset(void *ptr) {
    struct EXT2DirectoryEntry *self   = get_directory_entry(ptr, 0);
    struct EXT2DirectoryEntry *parent = get_next_directory_entry(self);
    return self->rec_len + parent->rec_len;
}

// name_len without trailing '\0' padding (the reference image sometimes counts the NUL)
static uint8_t entry_name_len(struct EXT2DirectoryEntry *entry) {
    uint8_t len  = entry->name_len;
    char   *name = get_entry_name(entry);
    while (len > 0 && name[len - 1] == '\0')
        len--;
    return len;
}

// Entry at `offset` is structurally valid inside a block
static bool entry_sane(struct EXT2DirectoryEntry *entry, uint32_t offset) {
    return entry->rec_len >= EXT2_DIR_ENTRY_HEADER && (entry->rec_len & 3u) == 0 &&
           offset + entry->rec_len <= BLOCK_SIZE &&
           (uint32_t) EXT2_DIR_ENTRY_HEADER + entry->name_len <= entry->rec_len;
}

static bool entry_matches(struct EXT2DirectoryEntry *entry, const char *name, uint8_t len) {
    return entry->inode != 0 && entry_name_len(entry) == len && memcmp(get_entry_name(entry), name, len) == 0;
}

/**
 * Iterate every live entry of a directory; the callback returns false to stop.
 * Handles both rec_len chains that span to the block end (ext2) and chains that
 * end with a zero rec_len terminator (reference implementation).
 */
typedef bool (*DirVisitor)(struct EXT2DirectoryEntry *entry, uint32_t block, uint32_t offset,
                           uint32_t prev_offset, void *ctx);

static bool dir_for_each(const struct EXT2Inode *dir, DirVisitor visit, void *ctx) {
    uint32_t nblocks = node_data_blocks(dir);
    struct BlockBuffer b;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t block = bmap_get(dir, bi);
        if (block == 0)
            continue;
        read_blocks(&b, block, 1);
        uint32_t offset = 0, prev = NO_OFFSET;
        while (offset + EXT2_DIR_ENTRY_HEADER <= BLOCK_SIZE) {
            struct EXT2DirectoryEntry *entry = get_directory_entry(b.buf, offset);
            if (entry->rec_len == 0 || !entry_sane(entry, offset))
                break;
            if (entry->inode != 0 && !visit(entry, block, offset, prev, ctx))
                return false;
            prev    = offset;
            offset += entry->rec_len;
        }
    }
    return true;
}

struct FindContext {
    const char         *name;
    uint8_t             len;
    uint32_t            inode;     // when name == NULL: search by inode number
    bool                found;
    struct DirLocation  location;
    struct EXT2DirectoryEntry entry;     // header only, the name is in found_name
    uint8_t             found_name_len;
    char                found_name[EXT2_NAME_MAX + 1];
};

static bool find_visitor(struct EXT2DirectoryEntry *entry, uint32_t block, uint32_t offset,
                         uint32_t prev_offset, void *ctx) {
    struct FindContext *fc = (struct FindContext*) ctx;
    bool hit = fc->name != NULL ? entry_matches(entry, fc->name, fc->len)
                                : (entry->inode == fc->inode && !is_dot_or_dotdot(get_entry_name(entry), entry_name_len(entry)));
    if (!hit)
        return true;
    fc->found                = true;
    fc->location.block       = block;
    fc->location.offset      = offset;
    fc->location.prev_offset = prev_offset;
    fc->entry                = *entry;
    uint8_t len = entry_name_len(entry);
    memcpy(fc->found_name, get_entry_name(entry), len);
    fc->found_name[len] = '\0';
    fc->found_name_len  = len;
    return false;
}

static bool dir_find(const struct EXT2Inode *dir, const char *name, uint8_t len, struct FindContext *fc) {
    memset(fc, 0, sizeof(*fc));
    fc->name = name;
    fc->len  = len;
    dir_for_each(dir, find_visitor, fc);
    return fc->found;
}

static bool dir_find_inode(const struct EXT2Inode *dir, uint32_t inode, struct FindContext *fc) {
    memset(fc, 0, sizeof(*fc));
    fc->inode = inode;
    dir_for_each(dir, find_visitor, fc);
    return fc->found;
}

static void write_entry(uint8_t *block_buf, uint32_t offset, uint16_t rec_len, uint32_t inode,
                        const char *name, uint8_t len, uint8_t file_type) {
    struct EXT2DirectoryEntry *entry = get_directory_entry(block_buf, offset);
    entry->inode     = inode;
    entry->rec_len   = rec_len;
    entry->name_len  = len;
    entry->file_type = file_type;
    memcpy(get_entry_name(entry), name, len);
    // Zero the padding so names are always followed by NUL bytes inside the record
    memset(get_entry_name(entry) + len, 0, rec_len - EXT2_DIR_ENTRY_HEADER - len);
}

/**
 * Try to place an entry of `need` bytes in an existing directory block.
 * @param apply false = dry run (only report whether it fits)
 */
static bool dir_place_in_block(uint8_t *buf, uint16_t need, bool apply, uint32_t inode,
                               const char *name, uint8_t len, uint8_t file_type) {
    uint32_t offset = 0;
    while (offset + EXT2_DIR_ENTRY_HEADER <= BLOCK_SIZE) {
        struct EXT2DirectoryEntry *entry = get_directory_entry(buf, offset);
        if (entry->rec_len == 0) {
            // Unterminated tail: the rest of the block is free space
            if (BLOCK_SIZE - offset < need)
                return false;
            if (apply)
                write_entry(buf, offset, (uint16_t) (BLOCK_SIZE - offset), inode, name, len, file_type);
            return true;
        }
        if (!entry_sane(entry, offset))
            return false;
        if (entry->inode == 0) {
            if (entry->rec_len >= need) {
                if (apply)
                    write_entry(buf, offset, entry->rec_len, inode, name, len, file_type);
                return true;
            }
        } else {
            uint16_t used = get_entry_record_len(entry->name_len);
            if (entry->rec_len >= used + need) {
                if (apply) {
                    uint16_t rest  = (uint16_t) (entry->rec_len - used);
                    entry->rec_len = used;
                    write_entry(buf, offset + used, rest, inode, name, len, file_type);
                }
                return true;
            }
        }
        offset += entry->rec_len;
    }
    return false;
}

// Extra blocks needed to add an entry of `len` bytes to `dir` (0 if an existing block has room)
static uint32_t dir_append_cost(const struct EXT2Inode *dir, uint8_t len) {
    uint16_t need    = get_entry_record_len(len);
    uint32_t nblocks = node_data_blocks(dir);
    struct BlockBuffer b;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t block = bmap_get(dir, bi);
        if (block == 0)
            continue;
        read_blocks(&b, block, 1);
        if (dir_place_in_block(b.buf, need, false, 0, NULL, 0, 0))
            return 0;
    }
    return 1 + meta_blocks_for(nblocks + 1) - meta_blocks_for(nblocks);
}

/**
 * Add an entry to a directory, growing it by one block when needed.
 * `dir` is updated in memory (i_size / i_block / i_blocks); the caller syncs it.
 */
static bool dir_add_entry(struct EXT2Inode *dir, uint32_t dir_inode, uint32_t child,
                          const char *name, uint8_t len, uint8_t file_type) {
    uint16_t need    = get_entry_record_len(len);
    uint32_t nblocks = node_data_blocks(dir);
    struct BlockBuffer b;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t block = bmap_get(dir, bi);
        if (block == 0)
            continue;
        read_blocks(&b, block, 1);
        if (dir_place_in_block(b.buf, need, true, child, name, len, file_type)) {
            write_blocks(&b, block, 1);
            return true;
        }
    }

    uint32_t group = inode_to_bgd(dir_inode);
    uint32_t block = alloc_block(group);
    if (block == 0)
        return false;
    memset(&b, 0, sizeof(b));
    write_entry(b.buf, 0, BLOCK_SIZE, child, name, len, file_type);
    write_blocks(&b, block, 1);
    dir->i_blocks++;
    if (!bmap_set(dir, nblocks, block, group))
        return false;
    dir->i_size = (nblocks + 1) * BLOCK_SIZE;
    return true;
}

// Remove the entry at `loc`: merge it into the previous record, or mark it unused
static void dir_remove_entry(const struct DirLocation *loc) {
    struct BlockBuffer b;
    read_blocks(&b, loc->block, 1);
    struct EXT2DirectoryEntry *entry = get_directory_entry(b.buf, loc->offset);
    if (loc->prev_offset != NO_OFFSET) {
        struct EXT2DirectoryEntry *prev = get_directory_entry(b.buf, loc->prev_offset);
        prev->rec_len = (uint16_t) (prev->rec_len + entry->rec_len);
    } else {
        entry->inode     = 0;
        entry->name_len  = 0;
        entry->file_type = EXT2_FT_UNKNOWN;
    }
    write_blocks(&b, loc->block, 1);
}

// Point the ".." entry of a directory to a new parent
static void dir_set_parent(const struct EXT2Inode *dir, uint32_t new_parent) {
    struct FindContext fc;
    if (!dir_find(dir, "..", 2, &fc))
        return;
    struct BlockBuffer b;
    read_blocks(&b, fc.location.block, 1);
    get_directory_entry(b.buf, fc.location.offset)->inode = new_parent;
    write_blocks(&b, fc.location.block, 1);
}

static bool child_visitor(struct EXT2DirectoryEntry *entry, uint32_t block, uint32_t offset,
                          uint32_t prev_offset, void *ctx) {
    (void) block;
    (void) offset;
    (void) prev_offset;
    if (is_dot_or_dotdot(get_entry_name(entry), entry_name_len(entry)))
        return true;
    *(bool*) ctx = true;
    return false;
}

bool is_directory_empty(uint32_t inode) {
    op_begin();
    struct EXT2Inode dir;
    if (!load_directory(inode, &dir))
        return false;
    bool has_child = false;
    dir_for_each(&dir, child_visitor, &has_child);
    return !has_child;
}

void init_directory_table(struct EXT2Inode *node, uint32_t inode, uint32_t parent_inode) {
    uint32_t block = alloc_block(inode_to_bgd(inode));
    struct BlockBuffer b;
    memset(&b, 0, sizeof(b));
    write_entry(b.buf, 0, get_entry_record_len(1), inode, ".", 1, EXT2_FT_DIR);
    write_entry(b.buf, get_entry_record_len(1), (uint16_t) (BLOCK_SIZE - get_entry_record_len(1)),
                parent_inode, "..", 2, EXT2_FT_DIR);
    write_blocks(&b, block, 1);

    uint32_t t = now();
    memset(node, 0, sizeof(*node));
    node->i_mode        = EXT2_S_IFDIR;
    node->i_size        = BLOCK_SIZE;
    node->i_blocks      = 1;
    node->i_block[0]    = block;
    node->i_links_count = 2;
    node->i_atime = node->i_ctime = node->i_mtime = t;
}

/* ================================ NAME INDEX ======================================= */

static void index_remove(uint32_t inode) {
    if (!inode_number_valid(inode) || !name_index[inode].used)
        return;
    uint32_t bucket = name_index[inode].hash % INDEX_BUCKETS;
    uint16_t *link  = &name_index_bucket[bucket];
    while (*link != INDEX_NONE) {
        if (*link == inode) {
            *link = name_index[inode].next;
            break;
        }
        link = &name_index[*link].next;
    }
    name_index[inode].used = false;
}

static void index_insert(uint32_t inode, uint32_t parent, const char *name, uint8_t len, uint8_t file_type) {
    if (!inode_number_valid(inode))
        return;
    index_remove(inode);
    struct NameIndexNode *node = &name_index[inode];
    node->used      = true;
    node->file_type = file_type;
    node->parent    = parent;
    node->hash      = fnv1a(name, len);
    uint32_t bucket = node->hash % INDEX_BUCKETS;
    node->next      = name_index_bucket[bucket];
    name_index_bucket[bucket] = (uint16_t) inode;
}

struct IndexBuildContext {
    uint32_t parent;
    uint16_t *queue;
    uint32_t *tail;
    uint8_t  *visited;
};

static bool index_build_visitor(struct EXT2DirectoryEntry *entry, uint32_t block, uint32_t offset,
                                uint32_t prev_offset, void *ctx) {
    (void) block;
    (void) offset;
    (void) prev_offset;
    struct IndexBuildContext *ic = (struct IndexBuildContext*) ctx;
    uint8_t len = entry_name_len(entry);
    if (is_dot_or_dotdot(get_entry_name(entry), len) || !inode_allocated(entry->inode))
        return true;
    if (bit_get(ic->visited, entry->inode))
        return true; // second link to the same inode: keep the first one
    bit_set(ic->visited, entry->inode);
    struct EXT2Inode child;
    read_inode(entry->inode, &child);
    uint8_t type = is_directory_inode(&child) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    index_insert(entry->inode, ic->parent, get_entry_name(entry), len, type);
    if (type == EXT2_FT_DIR && *ic->tail < EXT2_INODES_COUNT)
        ic->queue[(*ic->tail)++] = (uint16_t) entry->inode;
    return true;
}

// Breadth-first walk from the root, done once per mount
static void index_build(void) {
    static uint16_t queue[EXT2_INODES_COUNT];
    static uint8_t  visited[EXT2_INODES_COUNT / 8 + 1];
    memset(name_index, 0, sizeof(name_index));
    memset(name_index_bucket, 0, sizeof(name_index_bucket));
    memset(visited, 0, sizeof(visited));

    uint32_t head = 0, tail = 0;
    queue[tail++] = EXT2_ROOT_INODE;
    bit_set(visited, EXT2_ROOT_INODE);
    while (head < tail) {
        uint32_t dir_inode = queue[head++];
        struct EXT2Inode dir;
        if (!load_directory(dir_inode, &dir))
            continue;
        struct IndexBuildContext ic = {.parent = dir_inode, .queue = queue, .tail = &tail, .visited = visited};
        dir_for_each(&dir, index_build_visitor, &ic);
    }
    ptr_cache_flush();
}

/* ================================ INITIALIZER ====================================== */

bool is_empty_storage(void) {
    struct BlockBuffer boot_sector;
    read_blocks(&boot_sector, BOOT_SECTOR, 1);
    return memcmp(boot_sector.buf, fs_signature, BLOCK_SIZE) != 0;
}

static void reset_volatile_state(void) {
    memset(ptr_cache, 0, sizeof(ptr_cache));
    memset(block_bitmap_dirty, 0, sizeof(block_bitmap_dirty));
    memset(inode_bitmap_dirty, 0, sizeof(inode_bitmap_dirty));
}

static void set_superblock_geometry(void) {
    superblock.s_inodes_count        = EXT2_INODES_COUNT;
    superblock.s_blocks_count        = EXT2_BLOCKS_COUNT;
    superblock.s_r_blocks_count      = 0;
    superblock.s_first_data_block    = EXT2_SUPERBLOCK_LOCATION;
    superblock.s_first_ino           = EXT2_ROOT_INODE;
    superblock.s_blocks_per_group    = BLOCKS_PER_GROUP;
    superblock.s_frags_per_group     = BLOCKS_PER_GROUP;
    superblock.s_inodes_per_group    = INODES_PER_GROUP;
    superblock.s_magic               = EXT2_SUPER_MAGIC;
    superblock.s_prealloc_blocks     = 0;
    superblock.s_prealloc_dir_blocks = 0;
    for (uint32_t g = 0; g < GROUPS_COUNT; g++) {
        bgd_table.table[g].bg_block_bitmap = group_block_bitmap(g);
        bgd_table.table[g].bg_inode_bitmap = group_inode_bitmap(g);
        bgd_table.table[g].bg_inode_table  = group_inode_table(g);
    }
}

void create_ext2(void) {
    reset_volatile_state();
    write_blocks(fs_signature, BOOT_SECTOR, 1);

    memset(&superblock, 0, sizeof(superblock));
    memset(&bgd_table, 0, sizeof(bgd_table));
    set_superblock_geometry();
    superblock.s_free_blocks_count = 0;
    superblock.s_free_inodes_count = EXT2_INODES_COUNT;
    superblock.s_mtime             = now();
    superblock.s_mnt_count         = 1;

    for (uint32_t g = 0; g < GROUPS_COUNT; g++) {
        memset(&block_bitmap[g], 0, sizeof(block_bitmap[g]));
        memset(&inode_bitmap[g], 0, sizeof(inode_bitmap[g]));
        uint32_t first_data = group_first_data_index(g);
        for (uint32_t i = 0; i < first_data; i++)
            bit_set(block_bitmap[g].buf, i); // boot / superblock / BGD / bitmaps / inode table
        bgd_table.table[g].bg_free_blocks_count = (uint16_t) (BLOCKS_PER_GROUP - first_data);
        bgd_table.table[g].bg_free_inodes_count = INODES_PER_GROUP;
        bgd_table.table[g].bg_used_dirs_count   = 0;
        superblock.s_free_blocks_count         += BLOCKS_PER_GROUP - first_data;
        block_bitmap_dirty[g] = inode_bitmap_dirty[g] = true;
    }

    // Root directory: inode 1, "." and ".." both point to itself
    uint32_t root = allocate_node();
    struct EXT2Inode root_node;
    init_directory_table(&root_node, root, root);
    sync_node(&root_node, root);
    bgd_table.table[inode_to_bgd(root)].bg_used_dirs_count++;
    commit_metadata();
}

// Recompute free counters from the bitmaps (repairs images with stale counters)
static void recount_free(void) {
    superblock.s_free_blocks_count = 0;
    superblock.s_free_inodes_count = 0;
    for (uint32_t g = 0; g < GROUPS_COUNT; g++) {
        // Metadata blocks must always be marked used, otherwise they could be allocated
        for (uint32_t i = 0; i < group_first_data_index(g); i++) {
            if (!bit_get(block_bitmap[g].buf, i)) {
                bit_set(block_bitmap[g].buf, i);
                block_bitmap_dirty[g] = true;
            }
        }
        uint32_t free_blocks = 0, free_inodes = 0, dirs = 0;
        for (uint32_t i = 0; i < BLOCKS_PER_GROUP; i++)
            free_blocks += !bit_get(block_bitmap[g].buf, i);
        for (uint32_t table_block = 0; table_block < INODES_TABLE_BLOCK_COUNT; table_block++) {
            struct BlockBuffer b;
            bool loaded = false;
            for (uint32_t k = 0; k < INODES_PER_TABLE; k++) {
                uint32_t i = table_block * INODES_PER_TABLE + k;
                if (!bit_get(inode_bitmap[g].buf, i)) {
                    free_inodes++;
                    continue;
                }
                if (!loaded) {
                    read_blocks(&b, group_inode_table(g) + table_block, 1);
                    loaded = true;
                }
                dirs += is_directory_inode((struct EXT2Inode*) (b.buf + k * INODE_SIZE));
            }
        }
        bgd_table.table[g].bg_free_blocks_count = (uint16_t) free_blocks;
        bgd_table.table[g].bg_free_inodes_count = (uint16_t) free_inodes;
        bgd_table.table[g].bg_used_dirs_count   = (uint16_t) dirs;
        superblock.s_free_blocks_count += free_blocks;
        superblock.s_free_inodes_count += free_inodes;
    }
}

void initialize_filesystem_ext2(void) {
    reset_volatile_state();
    if (is_empty_storage()) {
        create_ext2();
    } else {
        struct BlockBuffer b;
        read_blocks(&b, EXT2_SUPERBLOCK_LOCATION, 1);
        memcpy(&superblock, b.buf, sizeof(superblock));
        read_blocks(&b, EXT2_BGD_TABLE_LOCATION, 1);
        memcpy(&bgd_table, b.buf, sizeof(bgd_table)); // only GROUPS_COUNT entries, not the whole block

        // Geometry is hardcoded (book 2.4.5); always use it instead of trusting the disk
        set_superblock_geometry();
        for (uint32_t g = 0; g < GROUPS_COUNT; g++) {
            read_blocks(&block_bitmap[g], group_block_bitmap(g), 1);
            read_blocks(&inode_bitmap[g], group_inode_bitmap(g), 1);
        }
        recount_free();
        superblock.s_mnt_count++;
        superblock.s_mtime = now();
        commit_metadata();
    }
    index_build();
}

/* ================================ CRUD ============================================= */

int8_t read_directory(struct EXT2DriverRequest *prequest) {
    if (prequest == NULL)
        return -1;
    op_begin();
    struct EXT2Inode parent;
    if (!load_directory(prequest->parent_inode, &parent))
        return 3;

    uint32_t target = prequest->parent_inode;
    if (prequest->name_len > 0) {
        struct FindContext fc;
        if (prequest->name == NULL || !dir_find(&parent, prequest->name, prequest->name_len, &fc))
            return 2;
        target = fc.entry.inode;
    }

    struct EXT2Inode dir;
    if (!inode_allocated(target))
        return 2;
    read_inode(target, &dir);
    if (!is_directory_inode(&dir))
        return 1;

    uint32_t nblocks = node_data_blocks(&dir);
    uint32_t size    = nblocks * BLOCK_SIZE;
    if (prequest->buf == NULL || prequest->buffer_size < size) {
        prequest->buffer_size = size;
        ptr_cache_flush();
        return -1;
    }
    uint8_t *dst = (uint8_t*) prequest->buf;
    for (uint32_t bi = 0; bi < nblocks; bi++) {
        uint32_t block = bmap_get(&dir, bi);
        if (block == 0)
            memset(dst + bi * BLOCK_SIZE, 0, BLOCK_SIZE);
        else
            read_blocks(dst + bi * BLOCK_SIZE, block, 1);
    }
    // Zero the rest so the caller can stop at the first block whose first rec_len is 0
    memset(dst + size, 0, prequest->buffer_size - size);
    ptr_cache_flush();
    return 0;
}

int8_t read(struct EXT2DriverRequest request) {
    op_begin();
    struct EXT2Inode parent;
    if (!load_directory(request.parent_inode, &parent))
        return 4;
    struct FindContext fc;
    if (request.name == NULL || request.name_len == 0 || !dir_find(&parent, request.name, request.name_len, &fc))
        return 3;
    if (!inode_allocated(fc.entry.inode))
        return -1;

    struct EXT2Inode node;
    read_inode(fc.entry.inode, &node);
    if (!is_regular_inode(&node))
        return 1;
    if (request.buffer_size < node.i_size)
        return 2;
    if (node.i_size > 0 && request.buf == NULL)
        return -1;
    bool ok = read_node_data(&node, (uint8_t*) request.buf, node.i_size);
    ptr_cache_flush();
    return ok ? 0 : -1;
}

int8_t write(struct EXT2DriverRequest *request) {
    op_begin();
    if (request == NULL || !is_valid_new_name(request->name, request->name_len))
        return -1;
    struct EXT2Inode parent;
    uint32_t parent_inode = request->parent_inode;
    if (!load_directory(parent_inode, &parent))
        return 2;
    struct FindContext fc;
    if (dir_find(&parent, request->name, request->name_len, &fc))
        return 1;

    bool     is_dir      = request->is_directory;
    uint32_t size        = is_dir ? 0 : request->buffer_size;
    uint32_t data_blocks = is_dir ? 1 : div_ceil(size, BLOCK_SIZE);
    if (!is_dir && size > 0 && request->buf == NULL)
        return -1;
    if (data_blocks > max_file_blocks())
        return -1;

    // Reserve everything up front so a failure never leaves a half-written entry
    uint32_t needed = data_blocks + meta_blocks_for(data_blocks) + dir_append_cost(&parent, request->name_len);
    if (superblock.s_free_inodes_count == 0 || superblock.s_free_blocks_count < needed) {
        ptr_cache_flush();
        return -1;
    }

    uint32_t inode = allocate_node();
    if (inode == 0)
        return -1;
    struct EXT2Inode node;
    if (is_dir) {
        init_directory_table(&node, inode, parent_inode);
        bgd_table.table[inode_to_bgd(inode)].bg_used_dirs_count++;
        parent.i_links_count++; // the child's ".." links back to the parent
    } else {
        uint32_t t = now();
        memset(&node, 0, sizeof(node));
        node.i_mode        = EXT2_S_IFREG;
        node.i_size        = size;
        node.i_links_count = 1;
        node.i_atime = node.i_ctime = node.i_mtime = t;
        allocate_node_blocks(request->buf, &node, inode_to_bgd(inode));
    }
    ptr_cache_flush();
    sync_node(&node, inode);

    uint8_t file_type = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    bool added = dir_add_entry(&parent, parent_inode, inode, request->name, request->name_len, file_type);
    parent.i_mtime = now();
    ptr_cache_flush();
    sync_node(&parent, parent_inode);
    commit_metadata();
    if (!added)
        return -1;
    index_insert(inode, parent_inode, request->name, request->name_len, file_type);
    return 0;
}

void deallocate_node(uint32_t inode) {
    if (!inode_allocated(inode))
        return;
    struct EXT2Inode node;
    read_inode(inode, &node);
    deallocate_blocks(node.i_block, node_data_blocks(&node));
    uint32_t dtime = now();
    memset(&node, 0, sizeof(node));
    node.i_dtime = dtime;
    sync_node(&node, inode);
    free_inode_number(inode);
}

int8_t delete(struct EXT2DriverRequest request) {
    op_begin();
    struct EXT2Inode parent;
    if (!load_directory(request.parent_inode, &parent))
        return 3;
    if (request.name == NULL || request.name_len == 0)
        return 1;
    if (is_dot_or_dotdot(request.name, request.name_len))
        return -1; // "." and ".." can never be removed (book 2.4.5)
    struct FindContext fc;
    if (!dir_find(&parent, request.name, request.name_len, &fc))
        return 1;
    uint32_t inode = fc.entry.inode;
    if (inode == EXT2_ROOT_INODE || !inode_allocated(inode))
        return -1;

    struct EXT2Inode node;
    read_inode(inode, &node);
    bool is_dir = is_directory_inode(&node);
    if (is_dir && !is_directory_empty(inode))
        return 2;

    deallocate_node(inode);
    dir_remove_entry(&fc.location);
    if (is_dir) {
        uint32_t group = inode_to_bgd(inode);
        if (bgd_table.table[group].bg_used_dirs_count > 0)
            bgd_table.table[group].bg_used_dirs_count--;
        if (parent.i_links_count > 2)
            parent.i_links_count--;
    }
    parent.i_mtime = now();
    sync_node(&parent, request.parent_inode);
    ptr_cache_flush();
    commit_metadata();
    index_remove(inode);
    return 0;
}

/* ================================ EXTENSIONS ======================================= */

void ext2_set_time_source(uint32_t (*now_fn)(void)) {
    time_source = now_fn;
}

bool ext2_get_inode(uint32_t inode, struct EXT2Inode *out) {
    if (!inode_allocated(inode))
        return false;
    read_inode(inode, out);
    return true;
}

int8_t ext2_lookup(uint32_t parent_inode, const char *name, uint8_t name_len, uint32_t *out_inode, uint8_t *out_file_type) {
    op_begin();
    struct EXT2Inode parent;
    if (!load_directory(parent_inode, &parent))
        return 2;
    struct FindContext fc;
    bool found = name != NULL && name_len > 0 && dir_find(&parent, name, name_len, &fc);
    ptr_cache_flush();
    if (!found || !inode_allocated(fc.entry.inode))
        return 1;
    if (out_inode)
        *out_inode = fc.entry.inode;
    if (out_file_type) {
        struct EXT2Inode node;
        read_inode(fc.entry.inode, &node);
        *out_file_type = is_directory_inode(&node) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    }
    return 0;
}

// Parent of a directory from its ".." entry, 0 if unreadable
static uint32_t dir_parent(uint32_t dir_inode) {
    struct EXT2Inode dir;
    struct FindContext fc;
    if (!load_directory(dir_inode, &dir) || !dir_find(&dir, "..", 2, &fc))
        return 0;
    return fc.entry.inode;
}

int8_t ext2_rename(uint32_t parent_inode, const char *name, uint8_t name_len,
                   uint32_t new_parent_inode, const char *new_name, uint8_t new_name_len) {
    op_begin();
    if (name == NULL || name_len == 0 || is_dot_or_dotdot(name, name_len) || !is_valid_new_name(new_name, new_name_len))
        return -1;
    struct EXT2Inode parent, new_parent;
    if (!load_directory(parent_inode, &parent) || !load_directory(new_parent_inode, &new_parent))
        return 3;
    struct FindContext src;
    if (!dir_find(&parent, name, name_len, &src))
        return 1;
    uint32_t inode = src.entry.inode;
    if (parent_inode == new_parent_inode && name_len == new_name_len && memcmp(name, new_name, name_len) == 0)
        return 0;
    struct FindContext dst;
    if (dir_find(&new_parent, new_name, new_name_len, &dst))
        return 2;

    struct EXT2Inode node;
    if (!ext2_get_inode(inode, &node))
        return -1;
    bool is_dir = is_directory_inode(&node);
    if (is_dir) {
        // Walk up from the destination: reaching the moved directory means a cycle
        uint32_t cursor = new_parent_inode;
        for (uint32_t depth = 0; depth < EXT2_INODES_COUNT; depth++) {
            if (cursor == inode)
                return 4;
            if (cursor == EXT2_ROOT_INODE || cursor == 0)
                break;
            cursor = dir_parent(cursor);
        }
    }
    if (superblock.s_free_blocks_count < dir_append_cost(&new_parent, new_name_len)) {
        ptr_cache_flush();
        return -1;
    }

    uint8_t file_type = is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    bool same_dir = parent_inode == new_parent_inode;
    struct EXT2Inode *target_dir = same_dir ? &parent : &new_parent;
    if (!dir_add_entry(target_dir, new_parent_inode, inode, new_name, new_name_len, file_type)) {
        ptr_cache_flush();
        commit_metadata();
        return -1;
    }
    ptr_cache_flush();
    // Adding may have split the record in front of the old entry: locate it again
    if (dir_find(&parent, name, name_len, &src))
        dir_remove_entry(&src.location);

    uint32_t t = now();
    if (is_dir && !same_dir) {
        dir_set_parent(&node, new_parent_inode);
        if (parent.i_links_count > 2)
            parent.i_links_count--;
        new_parent.i_links_count++;
    }
    parent.i_mtime = t;
    sync_node(&parent, parent_inode);
    if (!same_dir) {
        new_parent.i_mtime = t;
        sync_node(&new_parent, new_parent_inode);
    }
    node.i_ctime = t;
    sync_node(&node, inode);
    ptr_cache_flush();
    commit_metadata();
    index_insert(inode, new_parent_inode, new_name, new_name_len, file_type);
    return 0;
}

uint32_t ext2_find(const char *name, uint8_t name_len, struct EXT2FindResult *results, uint32_t max_results) {
    op_begin();
    if (name == NULL || name_len == 0)
        return 0;
    uint32_t hash  = fnv1a(name, name_len);
    uint32_t count = 0;
    for (uint16_t inode = name_index_bucket[hash % INDEX_BUCKETS]; inode != INDEX_NONE; inode = name_index[inode].next) {
        struct NameIndexNode *node = &name_index[inode];
        if (node->hash != hash)
            continue;
        // Hash hit: confirm the real name in the parent directory (collisions are possible)
        struct EXT2Inode parent;
        struct FindContext fc;
        if (!load_directory(node->parent, &parent) || !dir_find_inode(&parent, inode, &fc))
            continue;
        if (fc.found_name_len != name_len || memcmp(fc.found_name, name, name_len) != 0)
            continue;
        if (count < max_results) {
            results[count].inode        = inode;
            results[count].parent_inode = node->parent;
            results[count].file_type    = node->file_type;
        }
        count++;
    }
    ptr_cache_flush();
    return count;
}

bool ext2_get_name(uint32_t inode, uint32_t *parent_inode, char *name_buf) {
    op_begin();
    if (!inode_number_valid(inode) || !name_index[inode].used)
        return false;
    struct EXT2Inode parent;
    struct FindContext fc;
    bool ok = load_directory(name_index[inode].parent, &parent) && dir_find_inode(&parent, inode, &fc);
    ptr_cache_flush();
    if (!ok)
        return false;
    if (parent_inode)
        *parent_inode = name_index[inode].parent;
    memcpy(name_buf, fc.found_name, sizeof(fc.found_name));
    return true;
}

void ext2_get_stats(struct EXT2Stats *stats) {
    stats->blocks_count      = EXT2_BLOCKS_COUNT;
    stats->free_blocks_count = superblock.s_free_blocks_count;
    stats->inodes_count      = EXT2_INODES_COUNT;
    stats->free_inodes_count = superblock.s_free_inodes_count;
    stats->used_dirs_count   = 0;
    for (uint32_t g = 0; g < GROUPS_COUNT; g++)
        stats->used_dirs_count += bgd_table.table[g].bg_used_dirs_count;
    stats->mount_count = superblock.s_mnt_count;
}

/* ================================ CONSISTENCY CHECK ================================ */

struct CheckState {
    struct EXT2CheckReport *report;
    uint8_t  used_blocks[EXT2_BLOCKS_COUNT / 8];
    uint8_t  seen_inodes[EXT2_INODES_COUNT / 8 + 1];
    uint16_t queue[EXT2_INODES_COUNT];
    uint32_t tail;
    uint32_t current_dir;
    uint32_t subdirs;
};

static void check_error(struct CheckState *cs, const char *message, uint32_t value) {
    struct EXT2CheckReport *r = cs->report;
    if (r->errors++ != 0)
        return;
    // Tiny formatter: message + " " + decimal value (no printf in this file)
    uint32_t len = 0;
    while (message[len] != '\0' && len < sizeof(r->message) - 12) {
        r->message[len] = message[len];
        len++;
    }
    char digits[11];
    uint32_t nd = 0;
    do {
        digits[nd++] = (char) ('0' + value % 10);
        value /= 10;
    } while (value != 0);
    r->message[len++] = ' ';
    while (nd > 0)
        r->message[len++] = digits[--nd];
    r->message[len] = '\0';
}

static void check_mark_block(struct CheckState *cs, uint32_t block) {
    if (!is_data_block(block)) {
        check_error(cs, "invalid block pointer", block);
        return;
    }
    if (bit_get(cs->used_blocks, block)) {
        check_error(cs, "block referenced twice", block);
        return;
    }
    bit_set(cs->used_blocks, block);
    cs->report->used_blocks++;
}

static void check_tree(struct CheckState *cs, uint32_t block, uint32_t depth, uint32_t *remaining) {
    if (*remaining == 0)
        return;
    check_mark_block(cs, block);
    if (depth == 0) {
        (*remaining)--;
        return;
    }
    if (!is_data_block(block))
        return;
    uint32_t ptrs[EXT2_POINTERS_PER_BLOCK];
    read_blocks(ptrs, block, 1);
    for (uint32_t i = 0; i < EXT2_POINTERS_PER_BLOCK && *remaining > 0; i++) {
        if (ptrs[i] == 0) {
            check_error(cs, "hole inside indirect block", block);
            return;
        }
        check_tree(cs, ptrs[i], depth - 1, remaining);
    }
}

static void check_inode_blocks(struct CheckState *cs, uint32_t inode, const struct EXT2Inode *node) {
    uint32_t data_blocks = node_data_blocks(node);
    uint32_t before      = cs->report->used_blocks;
    uint32_t remaining   = data_blocks;
    for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS && remaining > 0; i++, remaining--)
        check_mark_block(cs, node->i_block[i]);
    for (uint32_t depth = 1; depth <= 3 && remaining > 0; depth++)
        check_tree(cs, node->i_block[EXT2_NDIR_BLOCKS + depth - 1], depth, &remaining);
    if (cs->report->used_blocks - before != node->i_blocks)
        check_error(cs, "i_blocks mismatch on inode", inode);
}

static bool check_visitor(struct EXT2DirectoryEntry *entry, uint32_t block, uint32_t offset,
                          uint32_t prev_offset, void *ctx) {
    (void) block;
    (void) offset;
    (void) prev_offset;
    struct CheckState *cs = (struct CheckState*) ctx;
    uint8_t len = entry_name_len(entry);
    if (is_dot_or_dotdot(get_entry_name(entry), len))
        return true;
    uint32_t child = entry->inode;
    if (!inode_allocated(child)) {
        check_error(cs, "entry points to a free inode", child);
        return true;
    }
    if (bit_get(cs->seen_inodes, child)) {
        check_error(cs, "inode linked twice", child);
        return true;
    }
    bit_set(cs->seen_inodes, child);
    struct EXT2Inode node;
    read_inode(child, &node);
    if (is_directory_inode(&node)) {
        cs->report->dirs++;
        cs->subdirs++;
        if (entry->file_type != EXT2_FT_DIR)
            check_error(cs, "directory entry has wrong file_type, inode", child);
        if (dir_parent(child) != cs->current_dir)
            check_error(cs, "'..' does not point to the parent, inode", child);
        if (cs->tail < EXT2_INODES_COUNT)
            cs->queue[cs->tail++] = (uint16_t) child;
    } else if (is_regular_inode(&node)) {
        cs->report->files++;
        if (entry->file_type != EXT2_FT_REG_FILE)
            check_error(cs, "file entry has wrong file_type, inode", child);
        if (node.i_links_count != 1)
            check_error(cs, "file link count is not 1, inode", child);
    } else {
        check_error(cs, "inode has unknown mode", child);
    }
    check_inode_blocks(cs, child, &node);
    return true;
}

bool ext2_check(struct EXT2CheckReport *report) {
    op_begin();
    static struct CheckState cs;
    memset(&cs, 0, sizeof(cs));
    memset(report, 0, sizeof(*report));
    cs.report = report;

    for (uint32_t g = 0; g < GROUPS_COUNT; g++)
        for (uint32_t i = 0; i < group_first_data_index(g); i++)
            bit_set(cs.used_blocks, g * BLOCKS_PER_GROUP + i);

    struct EXT2Inode root;
    if (!load_directory(EXT2_ROOT_INODE, &root)) {
        check_error(&cs, "root inode is not a directory", EXT2_ROOT_INODE);
        return false;
    }
    bit_set(cs.seen_inodes, EXT2_ROOT_INODE);
    report->dirs = 1;
    if (dir_parent(EXT2_ROOT_INODE) != EXT2_ROOT_INODE)
        check_error(&cs, "root '..' is not root", EXT2_ROOT_INODE);
    check_inode_blocks(&cs, EXT2_ROOT_INODE, &root);
    cs.queue[cs.tail++] = EXT2_ROOT_INODE;

    for (uint32_t head = 0; head < cs.tail; head++) {
        uint32_t dir_inode = cs.queue[head];
        struct EXT2Inode dir;
        if (!load_directory(dir_inode, &dir))
            continue;
        cs.current_dir = dir_inode;
        cs.subdirs     = 0;
        dir_for_each(&dir, check_visitor, &cs);
        if (dir.i_links_count != 2 + cs.subdirs)
            check_error(&cs, "directory link count mismatch, inode", dir_inode);
    }

    // Rebuilt usage must match the on-disk bitmaps exactly
    uint32_t free_blocks = 0, free_inodes = 0;
    for (uint32_t block = 0; block < EXT2_BLOCKS_COUNT; block++) {
        bool on_disk = bit_get(block_bitmap[block / BLOCKS_PER_GROUP].buf, block % BLOCKS_PER_GROUP);
        bool in_use  = bit_get(cs.used_blocks, block);
        if (on_disk && !in_use)
            check_error(&cs, "block marked used but unreferenced (leak)", block);
        if (!on_disk && in_use)
            check_error(&cs, "block in use but marked free", block);
        free_blocks += !on_disk;
    }
    for (uint32_t inode = 1; inode <= EXT2_INODES_COUNT; inode++) {
        bool on_disk = inode_allocated(inode);
        bool in_use  = bit_get(cs.seen_inodes, inode);
        if (on_disk && !in_use)
            check_error(&cs, "inode marked used but unreachable", inode);
        if (!on_disk && in_use)
            check_error(&cs, "inode in use but marked free", inode);
        free_inodes += !on_disk;
    }
    if (free_blocks != superblock.s_free_blocks_count)
        check_error(&cs, "superblock free block count is wrong, expected", free_blocks);
    if (free_inodes != superblock.s_free_inodes_count)
        check_error(&cs, "superblock free inode count is wrong, expected", free_inodes);
    ptr_cache_flush();
    return report->errors == 0;
}
