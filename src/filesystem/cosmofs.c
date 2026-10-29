#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/stdlib/string.h"
#include "header/filesystem/cosmofs.h"

#define BS                      COSMOFS_BLOCK_SIZE
#define COSMOFS_EXTENT_MAGIC    0x54584543u // "CEXT"
#define COSMOFS_JOURNAL_MAGIC   0x4E524A43u // "CJRN"
#define COSMOFS_COMMIT_MAGIC    0x544D4343u // "CCMT"
#define BITS_PER_BITMAP_BLOCK   (BS * 8u)
#define MAX_FILE_EXTENTS        (COSMOFS_INLINE_EXTENTS + COSMOFS_MAX_OVERFLOW * COSMOFS_OVERFLOW_EXTENTS)
#define CHECK_MAX_BLOCKS        65536u
#define CHECK_MAX_NODES         4096u

static const char boot_signature[] = "cOSmOS CosmoFS volume v1 - IF2130 Sistem Operasi ITB";

struct JournalDescriptor {
    uint32_t magic;
    uint32_t sequence;
    uint32_t count;
    uint32_t payload_checksum;
    uint32_t targets[COSMOFS_TXN_MAX_BLOCKS];
    uint8_t  reserved[BS - 16 - 4 * COSMOFS_TXN_MAX_BLOCKS - 4];
    uint32_t checksum;
} __attribute__((packed));

struct JournalCommit {
    uint32_t magic;
    uint32_t sequence;
    uint32_t payload_checksum;
    uint8_t  reserved[BS - 12 - 4];
    uint32_t checksum;
} __attribute__((packed));

_Static_assert(sizeof(struct CosmoFSSuperblock) == 128, "superblock layout");
_Static_assert(sizeof(struct CosmoFSNode) == 256, "node must be 256 bytes");
_Static_assert(sizeof(struct CosmoFSExtentBlock) == BS, "extent block must fill a block");
_Static_assert(sizeof(struct CosmoFSIndexSlot) == 8, "index slot must be 8 bytes");
_Static_assert(sizeof(struct JournalDescriptor) == BS && sizeof(struct JournalCommit) == BS, "journal records");
_Static_assert(COSMOFS_JOURNAL_BLOCKS >= COSMOFS_TXN_MAX_BLOCKS + 2, "journal must hold one full transaction");

static uint8_t zero_block[BS];

/* ================================ CRC32 ============================================ */

static uint32_t crc_table[256];
static bool     crc_ready = false;

static uint32_t crc32(const void *data, size_t len) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = true;
    }
    const uint8_t *p = (const uint8_t*) data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

// CRC of a record whose last 4 bytes are the checksum field
#define RECORD_CRC(ptr) crc32((ptr), sizeof(*(ptr)) - sizeof(uint32_t))

static uint32_t name_hash(const char *name, uint8_t len) {
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < len; i++) {
        h ^= (uint8_t) name[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t div_ceil(uint32_t a, uint32_t b) {
    return a / b + (a % b != 0);
}

static uint32_t now(struct CosmoFS *fs) {
    return fs->time_source ? fs->time_source() : 0;
}

static bool is_dot(const char *name, uint8_t len) {
    return len == 1 && name[0] == '.';
}

static bool is_dotdot(const char *name, uint8_t len) {
    return len == 2 && name[0] == '.' && name[1] == '.';
}

static bool valid_new_name(const char *name, uint8_t len) {
    if (name == NULL || len == 0 || len > COSMOFS_NAME_MAX || is_dot(name, len) || is_dotdot(name, len))
        return false;
    for (uint8_t i = 0; i < len; i++)
        if (name[i] == '/' || name[i] == '\0')
            return false;
    return true;
}

static int name_compare(const char *a, uint8_t alen, const char *b, uint8_t blen) {
    uint8_t n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c != 0)
        return c;
    return (int) alen - (int) blen;
}

/* ================================ DEVICE ACCESS ===================================== */

static bool raw_read(const struct CosmoFSDevice *dev, uint32_t lba, uint32_t count, void *buf) {
    return count == 0 || (lba < dev->block_count && count <= dev->block_count - lba &&
                          dev->read(dev->context, lba, count, buf));
}

static bool raw_write(const struct CosmoFSDevice *dev, uint32_t lba, uint32_t count, const void *buf) {
    return count == 0 || (lba < dev->block_count && count <= dev->block_count - lba &&
                          dev->write(dev->context, lba, count, buf));
}

static bool raw_flush(const struct CosmoFSDevice *dev) {
    return dev->flush == NULL || dev->flush(dev->context);
}

static bool dev_read(struct CosmoFS *fs, uint32_t lba, uint32_t count, void *buf) {
    if (raw_read(&fs->dev, lba, count, buf))
        return true;
    fs->io_errors++;
    return false;
}

static bool dev_write(struct CosmoFS *fs, uint32_t lba, uint32_t count, const void *buf) {
    if (raw_write(&fs->dev, lba, count, buf))
        return true;
    fs->io_errors++;
    return false;
}

static bool dev_flush(struct CosmoFS *fs) {
    if (raw_flush(&fs->dev))
        return true;
    fs->io_errors++;
    return false;
}

/* ================================ METADATA CACHE & TRANSACTIONS ===================== */

static uint8_t *cache_get(struct CosmoFS *fs, uint32_t block) {
    struct CosmoFSCacheEntry *victim = &fs->cache[0];
    for (uint32_t i = 0; i < COSMOFS_CACHE_BLOCKS; i++) {
        struct CosmoFSCacheEntry *e = &fs->cache[i];
        if (e->valid && e->block == block) {
            e->last_use = ++fs->cache_clock;
            return e->data;
        }
        if (!e->valid)
            victim = e;
        else if (victim->valid && e->last_use < victim->last_use)
            victim = e;
    }
    victim->valid = false;
    if (!dev_read(fs, block, 1, victim->data))
        return NULL;
    victim->valid    = true;
    victim->block    = block;
    victim->last_use = ++fs->cache_clock;
    return victim->data;
}

static void cache_update(struct CosmoFS *fs, uint32_t block, const uint8_t *data) {
    for (uint32_t i = 0; i < COSMOFS_CACHE_BLOCKS; i++)
        if (fs->cache[i].valid && fs->cache[i].block == block)
            memcpy(fs->cache[i].data, data, BS);
}

static void cache_invalidate(struct CosmoFS *fs) {
    for (uint32_t i = 0; i < COSMOFS_CACHE_BLOCKS; i++)
        fs->cache[i].valid = false;
}

static struct CosmoFSTxnEntry *txn_find(struct CosmoFS *fs, uint32_t block) {
    if (!fs->txn_active)
        return NULL;
    for (uint32_t i = 0; i < fs->txn_count; i++)
        if (fs->txn[i].block == block)
            return &fs->txn[i];
    return NULL;
}

// Current content of a metadata block (the transaction's copy wins over the disk)
static const uint8_t *meta_read(struct CosmoFS *fs, uint32_t block) {
    struct CosmoFSTxnEntry *e = txn_find(fs, block);
    if (e != NULL)
        return e->data;
    const uint8_t *data = cache_get(fs, block);
    if (data == NULL) {
        fs->txn_failed = true;
        return zero_block;
    }
    return data;
}

/**
 * Writable copy of a metadata block inside the open transaction. On failure (no open
 * transaction, transaction full, I/O error) the transaction is poisoned and a scratch
 * buffer is returned, so callers never need NULL checks: the commit will abort.
 */
static uint8_t *meta_write(struct CosmoFS *fs, uint32_t block) {
    static uint8_t sink[BS];
    struct CosmoFSTxnEntry *e = txn_find(fs, block);
    if (e != NULL)
        return e->data;
    if (!fs->txn_active || fs->txn_count >= COSMOFS_TXN_MAX_BLOCKS) {
        fs->txn_failed = true;
        return sink;
    }
    const uint8_t *current = cache_get(fs, block);
    if (current == NULL) {
        fs->txn_failed = true;
        return sink;
    }
    e        = &fs->txn[fs->txn_count++];
    e->block = block;
    memcpy(e->data, current, BS);
    return e->data;
}

static void txn_begin(struct CosmoFS *fs) {
    fs->txn_active = true;
    fs->txn_failed = false;
    fs->txn_count  = 0;
    fs->sb_backup  = fs->sb;
}

static void txn_abort(struct CosmoFS *fs) {
    fs->txn_active = false;
    fs->txn_count  = 0;
    fs->sb         = fs->sb_backup;
}

static void sb_seal(struct CosmoFSSuperblock *sb) {
    sb->checksum = RECORD_CRC(sb);
}

/**
 * Commit protocol (write-ahead journal, ordered data):
 *   1. payload blocks -> journal[1..n], descriptor -> journal[0], FLUSH
 *   2. commit record  -> journal[n+1], FLUSH          (transaction is now durable)
 *   3. checkpoint: every block to its home location, FLUSH, superblock last, FLUSH
 * The superblock carries the transaction sequence, so mount can tell whether the
 * journal still needs to be replayed.
 */
static bool txn_commit(struct CosmoFS *fs) {
    if (fs->txn_failed) {
        txn_abort(fs);
        return false;
    }
    fs->sb.write_time = now(fs);
    fs->sb.journal_sequence++;
    sb_seal(&fs->sb);
    uint8_t *sb_block = meta_write(fs, 1);
    if (fs->txn_failed) {
        txn_abort(fs);
        return false;
    }
    memset(sb_block, 0, BS);
    memcpy(sb_block, &fs->sb, sizeof(fs->sb));

    static struct JournalDescriptor desc;
    static struct JournalCommit     commit;
    memset(&desc, 0, sizeof(desc));
    memset(&commit, 0, sizeof(commit));
    uint32_t js = fs->sb.journal_start;

    // Payload checksum = CRC of the CRCs of every payload block, in order
    uint32_t block_crcs[COSMOFS_TXN_MAX_BLOCKS] = {0}; // (only the first txn_count are hashed)
    bool ok = true;
    for (uint32_t i = 0; i < fs->txn_count; i++) {
        block_crcs[i]   = crc32(fs->txn[i].data, BS);
        desc.targets[i] = fs->txn[i].block;
        ok = ok && dev_write(fs, js + 1 + i, 1, fs->txn[i].data);
    }
    desc.magic            = COSMOFS_JOURNAL_MAGIC;
    desc.sequence         = fs->sb.journal_sequence;
    desc.count            = fs->txn_count;
    desc.payload_checksum = crc32(block_crcs, fs->txn_count * sizeof(uint32_t));
    desc.checksum         = RECORD_CRC(&desc);
    ok = ok && dev_write(fs, js, 1, &desc) && dev_flush(fs);

    commit.magic            = COSMOFS_COMMIT_MAGIC;
    commit.sequence         = desc.sequence;
    commit.payload_checksum = desc.payload_checksum;
    commit.checksum         = RECORD_CRC(&commit);
    ok = ok && dev_write(fs, js + 1 + fs->txn_count, 1, &commit) && dev_flush(fs);
    if (!ok) {
        txn_abort(fs); // not durable: the on-disk state is still the previous one
        return false;
    }

    // Checkpoint (a crash from here on is repaired by journal replay at mount)
    struct CosmoFSTxnEntry *sb_entry = NULL;
    for (uint32_t i = 0; i < fs->txn_count; i++) {
        if (fs->txn[i].block == 1) {
            sb_entry = &fs->txn[i];
            continue;
        }
        dev_write(fs, fs->txn[i].block, 1, fs->txn[i].data);
        cache_update(fs, fs->txn[i].block, fs->txn[i].data);
    }
    dev_flush(fs);
    if (sb_entry != NULL) {
        dev_write(fs, 1, 1, sb_entry->data);
        cache_update(fs, 1, sb_entry->data);
    }
    dev_flush(fs);
    fs->txn_active = false;
    fs->txn_count  = 0;
    return true;
}

/* ================================ SUPERBLOCK & RECOVERY ============================= */

static bool sb_valid(const struct CosmoFSSuperblock *sb, uint32_t device_blocks) {
    if (sb->magic != COSMOFS_MAGIC || sb->version != COSMOFS_VERSION || sb->block_size != BS)
        return false;
    if (sb->checksum != RECORD_CRC(sb))
        return false;
    if (sb->total_blocks > device_blocks || sb->journal_start != 2 || sb->journal_blocks < COSMOFS_TXN_MAX_BLOCKS + 2)
        return false;
    if (sb->bitmap_start != sb->journal_start + sb->journal_blocks ||
        sb->bitmap_blocks != div_ceil(sb->total_blocks, BITS_PER_BITMAP_BLOCK) ||
        sb->node_table_start != sb->bitmap_start + sb->bitmap_blocks ||
        sb->node_table_blocks * COSMOFS_NODES_PER_BLOCK != sb->node_count ||
        sb->index_start != sb->node_table_start + sb->node_table_blocks ||
        sb->index_blocks * COSMOFS_SLOTS_PER_BLOCK != sb->index_slots ||
        sb->data_start != sb->index_start + sb->index_blocks || sb->data_start >= sb->total_blocks)
        return false;
    if ((sb->index_slots & (sb->index_slots - 1)) != 0 || sb->index_slots < sb->node_count || sb->root_node != COSMOFS_ROOT_NODE)
        return false;
    return sb->free_blocks <= sb->total_blocks && sb->free_nodes < sb->node_count;
}

// Replay the last transaction if it was committed but its checkpoint did not finish
static bool recover(struct CosmoFS *fs) {
    static struct JournalDescriptor desc;
    static struct JournalCommit     commit;
    uint32_t js = fs->sb.journal_start;
    if (!dev_read(fs, js, 1, &desc))
        return false;
    if (desc.magic != COSMOFS_JOURNAL_MAGIC || desc.checksum != RECORD_CRC(&desc) ||
        desc.count == 0 || desc.count > COSMOFS_TXN_MAX_BLOCKS)
        return true; // empty or torn journal: nothing to do
    if (desc.sequence != fs->sb.journal_sequence + 1)
        return true; // already checkpointed (or stale)
    if (!dev_read(fs, js + 1 + desc.count, 1, &commit))
        return false;
    if (commit.magic != COSMOFS_COMMIT_MAGIC || commit.checksum != RECORD_CRC(&commit) ||
        commit.sequence != desc.sequence || commit.payload_checksum != desc.payload_checksum)
        return true; // crashed before the commit record: transaction never happened

    // The transaction buffer doubles as replay storage
    uint32_t block_crcs[COSMOFS_TXN_MAX_BLOCKS] = {0}; // (only the first txn_count are hashed)
    for (uint32_t i = 0; i < desc.count; i++) {
        uint32_t target = desc.targets[i];
        if (target == 0 || target >= fs->sb.total_blocks ||
            (target >= js && target < js + fs->sb.journal_blocks) || target >= fs->sb.data_start)
            return true; // corrupted descriptor, refuse to scribble over the disk
        if (!dev_read(fs, js + 1 + i, 1, fs->txn[i].data))
            return false;
        fs->txn[i].block = target;
        block_crcs[i]    = crc32(fs->txn[i].data, BS);
    }
    if (crc32(block_crcs, desc.count * sizeof(uint32_t)) != desc.payload_checksum)
        return true;

    int32_t sb_index = -1;
    for (uint32_t i = 0; i < desc.count; i++) {
        if (fs->txn[i].block == 1) {
            sb_index = (int32_t) i;
            continue;
        }
        if (!dev_write(fs, fs->txn[i].block, 1, fs->txn[i].data))
            return false;
    }
    if (!dev_flush(fs))
        return false;
    if (sb_index >= 0) {
        if (!dev_write(fs, 1, 1, fs->txn[sb_index].data) || !dev_flush(fs))
            return false;
        memcpy(&fs->sb, fs->txn[sb_index].data, sizeof(fs->sb));
    }
    fs->replayed_transactions++;
    cache_invalidate(fs);
    return true;
}

/* ================================ NODES ============================================= */

static bool node_id_valid(struct CosmoFS *fs, uint32_t id) {
    return id >= 1 && id <= fs->sb.node_count;
}

static uint32_t node_block(struct CosmoFS *fs, uint32_t id) {
    return fs->sb.node_table_start + (id - 1) / COSMOFS_NODES_PER_BLOCK;
}

static uint32_t node_offset(uint32_t id) {
    return ((id - 1) % COSMOFS_NODES_PER_BLOCK) * sizeof(struct CosmoFSNode);
}

// Load a node that must be present with a valid checksum
static bool node_load(struct CosmoFS *fs, uint32_t id, struct CosmoFSNode *out) {
    if (!node_id_valid(fs, id))
        return false;
    memcpy(out, meta_read(fs, node_block(fs, id)) + node_offset(id), sizeof(*out));
    return (out->flags & COSMOFS_NODE_PRESENT) && out->id == id && out->checksum == RECORD_CRC(out) &&
           out->name_len <= COSMOFS_NAME_MAX && (out->type == COSMOFS_TYPE_FILE || out->type == COSMOFS_TYPE_DIR);
}

static void node_store(struct CosmoFS *fs, const struct CosmoFSNode *node) {
    struct CosmoFSNode sealed = *node;
    sealed.checksum = RECORD_CRC(&sealed);
    memcpy(meta_write(fs, node_block(fs, node->id)) + node_offset(node->id), &sealed, sizeof(sealed));
}

static bool load_dir(struct CosmoFS *fs, uint32_t id, struct CosmoFSNode *out) {
    return node_load(fs, id, out) && out->type == COSMOFS_TYPE_DIR;
}

static uint32_t node_alloc(struct CosmoFS *fs, uint32_t *generation) {
    uint32_t count = fs->sb.node_count;
    for (uint32_t k = 0; k < count; k++) {
        uint32_t id = (fs->free_node_hint - 1 + k) % count + 1;
        if (id == COSMOFS_ROOT_NODE)
            continue;
        const struct CosmoFSNode *raw = (const struct CosmoFSNode*) (meta_read(fs, node_block(fs, id)) + node_offset(id));
        if (!(raw->flags & COSMOFS_NODE_PRESENT)) {
            *generation          = raw->generation + 1;
            fs->free_node_hint   = id % count + 1;
            return id;
        }
    }
    return 0;
}

static void node_free(struct CosmoFS *fs, const struct CosmoFSNode *node) {
    struct CosmoFSNode empty;
    memset(&empty, 0, sizeof(empty));
    empty.id         = node->id;
    empty.generation = node->generation;
    node_store(fs, &empty);
    fs->sb.free_nodes++;
}

/* ================================ BITMAP & EXTENTS ================================== */

static bool bitmap_get(struct CosmoFS *fs, uint32_t block) {
    const uint8_t *b = meta_read(fs, fs->sb.bitmap_start + block / BITS_PER_BITMAP_BLOCK);
    uint32_t bit = block % BITS_PER_BITMAP_BLOCK;
    return (b[bit >> 3] >> (bit & 7)) & 1;
}

static void bitmap_mark_range(struct CosmoFS *fs, uint32_t start, uint32_t length, bool used) {
    for (uint32_t block = start; block < start + length; block++) {
        uint8_t *b   = meta_write(fs, fs->sb.bitmap_start + block / BITS_PER_BITMAP_BLOCK);
        uint32_t bit = block % BITS_PER_BITMAP_BLOCK;
        if (used)
            b[bit >> 3] |= (uint8_t) (1u << (bit & 7));
        else
            b[bit >> 3] &= (uint8_t) ~(1u << (bit & 7));
    }
    if (used)
        fs->sb.free_blocks -= length;
    else
        fs->sb.free_blocks += length;
}

/**
 * Free run selection: the smallest free run that holds `want` blocks (best fit),
 * otherwise the largest free run (the caller then continues with the rest).
 */
static bool find_free_run(struct CosmoFS *fs, uint32_t want, uint32_t *start, uint32_t *length) {
    uint32_t best_start = 0, best_len = 0, big_start = 0, big_len = 0;
    uint32_t run_start = 0, run_len = 0;
    uint32_t total = fs->sb.total_blocks;
    for (uint32_t base = 0; base < total; base += BITS_PER_BITMAP_BLOCK) {
        const uint8_t *b = meta_read(fs, fs->sb.bitmap_start + base / BITS_PER_BITMAP_BLOCK);
        for (uint32_t bit = 0; bit < BITS_PER_BITMAP_BLOCK; bit++) {
            uint32_t block = base + bit;
            bool is_free = block >= fs->sb.data_start && block < total && !((b[bit >> 3] >> (bit & 7)) & 1);
            if (is_free) {
                if (run_len++ == 0)
                    run_start = block;
                continue;
            }
            if (run_len != 0) {
                if (run_len >= want && (best_len == 0 || run_len < best_len)) {
                    best_start = run_start;
                    best_len   = run_len;
                }
                if (run_len > big_len) {
                    big_start = run_start;
                    big_len   = run_len;
                }
                run_len = 0;
            }
            if (block >= total)
                break;
        }
    }
    if (run_len != 0) {
        if (run_len >= want && (best_len == 0 || run_len < best_len)) {
            best_start = run_start;
            best_len   = run_len;
        }
        if (run_len > big_len) {
            big_start = run_start;
            big_len   = run_len;
        }
    }
    if (best_len != 0) {
        *start  = best_start;
        *length = want;
        return true;
    }
    if (big_len != 0) {
        *start  = big_start;
        *length = big_len;
        return true;
    }
    return false;
}

static void seal_extent_block(struct CosmoFSExtentBlock *eb) {
    eb->checksum = RECORD_CRC(eb);
}

static bool load_extent_block(struct CosmoFS *fs, uint32_t block, uint32_t owner, struct CosmoFSExtentBlock *out) {
    if (block < fs->sb.data_start || block >= fs->sb.total_blocks || !dev_read(fs, block, 1, out))
        return false;
    return out->magic == COSMOFS_EXTENT_MAGIC && out->owner == owner && out->count <= COSMOFS_OVERFLOW_EXTENTS &&
           out->checksum == RECORD_CRC(out);
}

// Write `length` blocks of file data starting at `src` (last block zero padded)
static bool write_data_run(struct CosmoFS *fs, uint32_t start, uint32_t length, const uint8_t *src, uint32_t bytes) {
    uint32_t full = bytes / BS;
    if (full > length)
        full = length;
    for (uint32_t done = 0; done < full;) {
        uint32_t chunk = full - done > 128 ? 128 : full - done;
        if (!dev_write(fs, start + done, chunk, src + done * BS))
            return false;
        done += chunk;
    }
    if (full < length) {
        static uint8_t tail[BS];
        memset(tail, 0, BS);
        memcpy(tail, src + full * BS, bytes - full * BS);
        if (!dev_write(fs, start + full, 1, tail))
            return false;
    }
    return true;
}

/**
 * Allocate and write the data of a new file, streaming extents into the node and,
 * once the 8 inline slots are used, into freshly allocated overflow blocks.
 * Data and overflow blocks are new blocks, written before the commit (ordered mode).
 */
static bool allocate_file_data(struct CosmoFS *fs, struct CosmoFSNode *node, const uint8_t *data, uint32_t size) {
    static struct CosmoFSExtentBlock overflow;
    uint32_t remaining   = div_ceil(size, BS);
    uint32_t offset      = 0;
    uint32_t overflow_no = 0;     // block of the overflow record being filled
    uint32_t overflows   = 0;
    node->block_count  = remaining;
    node->extent_count = 0;
    node->overflow     = 0;

    while (remaining > 0) {
        uint32_t start, length;
        if (!find_free_run(fs, remaining, &start, &length))
            return false;
        bitmap_mark_range(fs, start, length, true);
        uint32_t bytes = size - offset;
        if (bytes > length * BS)
            bytes = length * BS;
        if (!write_data_run(fs, start, length, data + offset, bytes))
            return false;
        offset    += bytes;
        remaining -= length;

        struct CosmoFSExtent extent = {.start = start, .length = length};
        if (node->extent_count < COSMOFS_INLINE_EXTENTS) {
            node->extents[node->extent_count++] = extent;
            continue;
        }
        if (overflow_no == 0 || overflow.count == COSMOFS_OVERFLOW_EXTENTS) {
            if (overflows == COSMOFS_MAX_OVERFLOW)
                return false; // pathological fragmentation
            uint32_t ob, ol;
            if (!find_free_run(fs, 1, &ob, &ol))
                return false;
            bitmap_mark_range(fs, ob, 1, true);
            if (overflow_no == 0) {
                node->overflow = ob;
            } else {
                overflow.next = ob;
                seal_extent_block(&overflow);
                if (!dev_write(fs, overflow_no, 1, &overflow))
                    return false;
            }
            memset(&overflow, 0, sizeof(overflow));
            overflow.magic = COSMOFS_EXTENT_MAGIC;
            overflow.owner = node->id;
            overflow_no    = ob;
            overflows++;
        }
        overflow.extents[overflow.count++] = extent;
    }
    if (overflow_no != 0) {
        seal_extent_block(&overflow);
        if (!dev_write(fs, overflow_no, 1, &overflow))
            return false;
    }
    return true;
}

typedef bool (*ExtentVisitor)(struct CosmoFS *fs, const struct CosmoFSExtent *extent, void *ctx);

/**
 * Visit every extent of a file in order; overflow blocks are reported to
 * `visit_overflow` (may be NULL) as single-block extents.
 * @return false on a corrupted overflow chain or when a visitor stops
 */
static bool for_each_extent(struct CosmoFS *fs, const struct CosmoFSNode *node, ExtentVisitor visit,
                            ExtentVisitor visit_overflow, void *ctx) {
    for (uint32_t i = 0; i < node->extent_count && i < COSMOFS_INLINE_EXTENTS; i++)
        if (!visit(fs, &node->extents[i], ctx))
            return false;
    static struct CosmoFSExtentBlock eb;
    uint32_t block = node->overflow;
    for (uint32_t hops = 0; block != 0; hops++) {
        if (hops >= COSMOFS_MAX_OVERFLOW || !load_extent_block(fs, block, node->id, &eb))
            return false;
        struct CosmoFSExtent self = {.start = block, .length = 1};
        uint32_t next = eb.next, count = eb.count;
        struct CosmoFSExtent extents[COSMOFS_OVERFLOW_EXTENTS];
        memcpy(extents, eb.extents, sizeof(extents)); // eb is reused by nested calls
        if (visit_overflow != NULL && !visit_overflow(fs, &self, ctx))
            return false;
        for (uint32_t i = 0; i < count; i++)
            if (!visit(fs, &extents[i], ctx))
                return false;
        block = next;
    }
    return true;
}

struct ReadContext {
    uint8_t  *dst;
    uint32_t  offset;
    uint32_t  size;
};

static bool read_extent_visitor(struct CosmoFS *fs, const struct CosmoFSExtent *extent, void *ctx) {
    struct ReadContext *rc = (struct ReadContext*) ctx;
    if (extent->start < fs->sb.data_start || extent->length > fs->sb.total_blocks - extent->start)
        return false;
    for (uint32_t done = 0; done < extent->length && rc->offset < rc->size;) {
        uint32_t left_bytes = rc->size - rc->offset;
        uint32_t full       = left_bytes / BS;
        uint32_t chunk      = extent->length - done;
        if (chunk > 128)
            chunk = 128;
        if (full == 0) {
            static uint8_t tail[BS];
            if (!dev_read(fs, extent->start + done, 1, tail))
                return false;
            memcpy(rc->dst + rc->offset, tail, left_bytes);
            rc->offset += left_bytes;
            return true;
        }
        if (chunk > full)
            chunk = full;
        if (!dev_read(fs, extent->start + done, chunk, rc->dst + rc->offset))
            return false;
        rc->offset += chunk * BS;
        done       += chunk;
    }
    return true;
}

static bool free_extent_visitor(struct CosmoFS *fs, const struct CosmoFSExtent *extent, void *ctx) {
    (void) ctx;
    if (extent->start < fs->sb.data_start || extent->length > fs->sb.total_blocks - extent->start)
        return false;
    bitmap_mark_range(fs, extent->start, extent->length, false);
    return true;
}

static bool count_extent_visitor(struct CosmoFS *fs, const struct CosmoFSExtent *extent, void *ctx) {
    (void) fs;
    (void) extent;
    (*(uint32_t*) ctx)++;
    return true;
}

/* ================================ NAME INDEX ======================================== */

static struct CosmoFSIndexSlot slot_read(struct CosmoFS *fs, uint32_t i) {
    struct CosmoFSIndexSlot slot;
    const uint8_t *b = meta_read(fs, fs->sb.index_start + i / COSMOFS_SLOTS_PER_BLOCK);
    memcpy(&slot, b + (i % COSMOFS_SLOTS_PER_BLOCK) * sizeof(slot), sizeof(slot));
    return slot;
}

static void slot_write(struct CosmoFS *fs, uint32_t i, uint32_t hash, uint32_t node) {
    struct CosmoFSIndexSlot slot = {.hash = hash, .node = node};
    uint8_t *b = meta_write(fs, fs->sb.index_start + i / COSMOFS_SLOTS_PER_BLOCK);
    memcpy(b + (i % COSMOFS_SLOTS_PER_BLOCK) * sizeof(slot), &slot, sizeof(slot));
}

static void index_insert(struct CosmoFS *fs, uint32_t hash, uint32_t node) {
    uint32_t mask = fs->sb.index_slots - 1;
    for (uint32_t k = 0, i = hash & mask; k < fs->sb.index_slots; k++, i = (i + 1) & mask) {
        struct CosmoFSIndexSlot slot = slot_read(fs, i);
        if (slot.node == COSMOFS_INDEX_EMPTY || slot.node == COSMOFS_INDEX_TOMBSTONE) {
            if (slot.node == COSMOFS_INDEX_TOMBSTONE && fs->sb.index_tombstones > 0)
                fs->sb.index_tombstones--;
            slot_write(fs, i, hash, node);
            return;
        }
    }
    fs->txn_failed = true; // cannot happen: slots >= 2 * nodes
}

static void index_remove(struct CosmoFS *fs, uint32_t hash, uint32_t node) {
    uint32_t mask = fs->sb.index_slots - 1;
    for (uint32_t k = 0, i = hash & mask; k < fs->sb.index_slots; k++, i = (i + 1) & mask) {
        struct CosmoFSIndexSlot slot = slot_read(fs, i);
        if (slot.node == COSMOFS_INDEX_EMPTY)
            return;
        if (slot.node == node) {
            slot_write(fs, i, 0, COSMOFS_INDEX_TOMBSTONE);
            fs->sb.index_tombstones++;
            return;
        }
    }
}

// Lookup (parent, name) -> node id through the index, 0 if absent
static uint32_t index_lookup(struct CosmoFS *fs, uint32_t parent, const char *name, uint8_t len, struct CosmoFSNode *out) {
    uint32_t hash = name_hash(name, len);
    uint32_t mask = fs->sb.index_slots - 1;
    for (uint32_t k = 0, i = hash & mask; k < fs->sb.index_slots; k++, i = (i + 1) & mask) {
        struct CosmoFSIndexSlot slot = slot_read(fs, i);
        if (slot.node == COSMOFS_INDEX_EMPTY)
            return 0;
        if (slot.node == COSMOFS_INDEX_TOMBSTONE || slot.hash != hash)
            continue;
        if (node_load(fs, slot.node, out) && out->parent == parent && out->name_len == len &&
            memcmp(out->name, name, len) == 0)
            return slot.node;
    }
    return 0;
}

// Rebuild the whole table (clears tombstones). Needs index_blocks transaction slots.
static void index_rebuild(struct CosmoFS *fs) {
    for (uint32_t b = 0; b < fs->sb.index_blocks; b++)
        memset(meta_write(fs, fs->sb.index_start + b), 0, BS);
    fs->sb.index_tombstones = 0;
    struct CosmoFSNode node;
    for (uint32_t id = 1; id <= fs->sb.node_count; id++)
        if (id != COSMOFS_ROOT_NODE && node_load(fs, id, &node))
            index_insert(fs, name_hash(node.name, node.name_len), id);
}

static void index_maintain(struct CosmoFS *fs) {
    if (fs->sb.index_tombstones > fs->sb.index_slots / 4 &&
        fs->txn_count + fs->sb.index_blocks + 1 <= COSMOFS_TXN_MAX_BLOCKS)
        index_rebuild(fs);
}

/* ================================ DIRECTORY LISTS =================================== */

// Insert `child` into the sorted child list of directory `dir_id`. Stores the directory and the
// neighbours; the caller stores `child` itself afterwards.
static bool child_insert(struct CosmoFS *fs, uint32_t dir_id, struct CosmoFSNode *child) {
    struct CosmoFSNode dir, cur;
    if (!load_dir(fs, dir_id, &dir))
        return false;
    uint32_t prev = 0, next = dir.first_child;
    for (uint32_t steps = 0; next != 0; steps++) {
        if (steps > fs->sb.node_count || !node_load(fs, next, &cur))
            return false;
        if (name_compare(child->name, child->name_len, cur.name, cur.name_len) < 0)
            break;
        prev = next;
        next = cur.next_sibling;
    }
    child->parent       = dir_id;
    child->prev_sibling = prev;
    child->next_sibling = next;
    if (prev != 0) {
        struct CosmoFSNode p;
        if (!node_load(fs, prev, &p))
            return false;
        p.next_sibling = child->id;
        node_store(fs, &p);
    } else {
        dir.first_child = child->id;
    }
    if (next != 0) {
        if (!node_load(fs, next, &cur))
            return false;
        cur.prev_sibling = child->id;
        node_store(fs, &cur);
    }
    dir.child_count++;
    dir.mtime = now(fs);
    node_store(fs, &dir);
    return true;
}

static bool child_remove(struct CosmoFS *fs, uint32_t dir_id, struct CosmoFSNode *child) {
    struct CosmoFSNode dir, neighbour;
    if (!load_dir(fs, dir_id, &dir))
        return false;
    if (child->prev_sibling != 0) {
        if (!node_load(fs, child->prev_sibling, &neighbour))
            return false;
        neighbour.next_sibling = child->next_sibling;
        node_store(fs, &neighbour);
    } else {
        dir.first_child = child->next_sibling;
    }
    if (child->next_sibling != 0) {
        if (!node_load(fs, child->next_sibling, &neighbour))
            return false;
        neighbour.prev_sibling = child->prev_sibling;
        node_store(fs, &neighbour);
    }
    if (dir.child_count > 0)
        dir.child_count--;
    dir.mtime = now(fs);
    node_store(fs, &dir);
    child->prev_sibling = child->next_sibling = 0;
    return true;
}

/* ================================ FORMAT & MOUNT ==================================== */

static uint32_t sb_node_block(const struct CosmoFSSuperblock *sb, uint32_t id) {
    return sb->node_table_start + (id - 1) / COSMOFS_NODES_PER_BLOCK;
}

bool cosmofs_format(const struct CosmoFSDevice *dev, const char *label, uint32_t (*time_source)(void)) {
    if (dev == NULL || dev->block_count < 512)
        return false;
    uint32_t total = dev->block_count;

    struct CosmoFSSuperblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic             = COSMOFS_MAGIC;
    sb.version           = COSMOFS_VERSION;
    sb.block_size        = BS;
    sb.total_blocks      = total;
    sb.journal_start     = 2;
    sb.journal_blocks    = COSMOFS_JOURNAL_BLOCKS;
    sb.bitmap_start      = sb.journal_start + sb.journal_blocks;
    sb.bitmap_blocks     = div_ceil(total, BITS_PER_BITMAP_BLOCK);
    uint32_t nodes       = total / 16;
    if (nodes < 64) nodes = 64;
    if (nodes > CHECK_MAX_NODES) nodes = CHECK_MAX_NODES;
    nodes               -= nodes % COSMOFS_NODES_PER_BLOCK;
    sb.node_count        = nodes;
    sb.node_table_start  = sb.bitmap_start + sb.bitmap_blocks;
    sb.node_table_blocks = nodes / COSMOFS_NODES_PER_BLOCK;
    uint32_t slots       = COSMOFS_SLOTS_PER_BLOCK;
    while (slots < 2 * nodes)
        slots *= 2;
    sb.index_slots       = slots;
    sb.index_blocks      = slots / COSMOFS_SLOTS_PER_BLOCK;
    sb.index_start       = sb.node_table_start + sb.node_table_blocks;
    sb.data_start        = sb.index_start + sb.index_blocks;
    if (sb.data_start + 16 > total)
        return false;
    sb.free_blocks       = total - sb.data_start;
    sb.free_nodes        = nodes - 1;
    sb.root_node         = COSMOFS_ROOT_NODE;
    sb.created_time      = time_source ? time_source() : 0;
    const char *text     = label ? label : "cosmos";
    for (uint32_t i = 0; i < sizeof(sb.label) - 1 && text[i] != '\0'; i++)
        sb.label[i] = text[i];

    static uint8_t block[BS];
    memset(block, 0, BS);
    memcpy(block, boot_signature, sizeof(boot_signature));
    bool ok = raw_write(dev, 0, 1, block);

    // Zero journal, node table and index (the bitmap is written below)
    for (uint32_t b = sb.journal_start; b < sb.journal_start + sb.journal_blocks && ok; b++)
        ok = raw_write(dev, b, 1, zero_block);
    for (uint32_t b = sb.node_table_start; b < sb.data_start && ok; b++)
        ok = raw_write(dev, b, 1, zero_block);

    // Bitmap: metadata blocks and the padding after the last block are "used"
    for (uint32_t k = 0; k < sb.bitmap_blocks && ok; k++) {
        memset(block, 0, BS);
        for (uint32_t bit = 0; bit < BITS_PER_BITMAP_BLOCK; bit++) {
            uint32_t b = k * BITS_PER_BITMAP_BLOCK + bit;
            if (b < sb.data_start || b >= total)
                block[bit >> 3] |= (uint8_t) (1u << (bit & 7));
        }
        ok = raw_write(dev, sb.bitmap_start + k, 1, block);
    }

    // Root directory node
    struct CosmoFSNode root;
    memset(&root, 0, sizeof(root));
    root.flags      = COSMOFS_NODE_PRESENT;
    root.type       = COSMOFS_TYPE_DIR;
    root.id         = COSMOFS_ROOT_NODE;
    root.parent     = COSMOFS_ROOT_NODE;
    root.ctime      = root.mtime = sb.created_time;
    root.generation = 1;
    root.checksum   = RECORD_CRC(&root);
    memset(block, 0, BS);
    memcpy(block + node_offset(COSMOFS_ROOT_NODE), &root, sizeof(root));
    ok = ok && raw_write(dev, sb_node_block(&sb, COSMOFS_ROOT_NODE), 1, block);

    sb_seal(&sb);
    memset(block, 0, BS);
    memcpy(block, &sb, sizeof(sb));
    ok = ok && raw_write(dev, 1, 1, block) && raw_flush(dev);
    return ok;
}

int8_t cosmofs_mount(struct CosmoFS *fs, const struct CosmoFSDevice *dev, uint32_t (*time_source)(void)) {
    memset(fs, 0, sizeof(*fs));
    fs->dev            = *dev;
    fs->time_source    = time_source;
    fs->free_node_hint = 1;
    static uint8_t block[BS];
    if (!dev_read(fs, 0, 1, block))
        return -1;
    if (memcmp(block, boot_signature, sizeof(boot_signature)) != 0)
        return 1;
    if (!dev_read(fs, 1, 1, block))
        return -1;
    memcpy(&fs->sb, block, sizeof(fs->sb));
    if (!sb_valid(&fs->sb, dev->block_count))
        return -1;
    if (!recover(fs) || !sb_valid(&fs->sb, dev->block_count))
        return -1;

    txn_begin(fs);
    fs->sb.mount_count++;
    fs->sb.mount_time = now(fs);
    if (!txn_commit(fs))
        return -1;
    fs->mounted = true;
    return 0;
}

/* ================================ CRUD ============================================== */

// Resolve name inside parent ("." and ".." included); returns the node id or 0
static uint32_t resolve(struct CosmoFS *fs, const struct CosmoFSNode *parent, const char *name, uint8_t len,
                        struct CosmoFSNode *out) {
    if (len == 0 || is_dot(name, len)) {
        *out = *parent;
        return parent->id;
    }
    if (is_dotdot(name, len))
        return node_load(fs, parent->parent, out) ? parent->parent : 0;
    return index_lookup(fs, parent->id, name, len, out);
}

int8_t cosmofs_write(struct CosmoFS *fs, const struct CosmoFSRequest *request) {
    if (fs == NULL || !fs->mounted || request == NULL || !valid_new_name(request->name, request->name_len))
        return -1;
    struct CosmoFSNode parent, existing;
    if (!load_dir(fs, request->parent, &parent))
        return 2;
    if (index_lookup(fs, parent.id, request->name, request->name_len, &existing) != 0)
        return 1;
    bool     is_dir  = request->is_directory;
    uint32_t size    = is_dir ? 0 : request->buffer_size;
    uint32_t nblocks = div_ceil(size, BS);
    if ((size > 0 && request->buf == NULL) || fs->sb.free_nodes == 0 || fs->sb.free_blocks < nblocks)
        return -1;

    txn_begin(fs);
    uint32_t generation = 0;
    uint32_t id = node_alloc(fs, &generation);
    if (id == 0) {
        txn_abort(fs);
        return -1;
    }
    struct CosmoFSNode node;
    memset(&node, 0, sizeof(node));
    node.flags      = COSMOFS_NODE_PRESENT;
    node.type       = is_dir ? COSMOFS_TYPE_DIR : COSMOFS_TYPE_FILE;
    node.id         = id;
    node.generation = generation;
    node.name_len   = request->name_len;
    memcpy(node.name, request->name, request->name_len);
    node.ctime      = node.mtime = now(fs);
    node.size       = size;
    if (nblocks > 0 && !allocate_file_data(fs, &node, (const uint8_t*) request->buf, size))
        fs->txn_failed = true;
    if (!fs->txn_failed && !child_insert(fs, parent.id, &node))
        fs->txn_failed = true;
    node_store(fs, &node);
    index_insert(fs, name_hash(node.name, node.name_len), id);
    fs->sb.free_nodes--;
    return txn_commit(fs) ? 0 : -1;
}

int8_t cosmofs_read(struct CosmoFS *fs, const struct CosmoFSRequest *request) {
    if (fs == NULL || !fs->mounted || request == NULL)
        return -1;
    struct CosmoFSNode parent, node;
    if (!load_dir(fs, request->parent, &parent))
        return 4;
    if (request->name == NULL || request->name_len == 0)
        return 3;
    if (resolve(fs, &parent, request->name, request->name_len, &node) == 0)
        return 3;
    if (node.type != COSMOFS_TYPE_FILE)
        return 1;
    if (request->buffer_size < node.size)
        return 2;
    if (node.size == 0)
        return 0;
    // buf may be 0 (= user virtual address 0, where programs are loaded)
    struct ReadContext rc = {.dst = (uint8_t*) request->buf, .offset = 0, .size = node.size};
    if (!for_each_extent(fs, &node, read_extent_visitor, NULL, &rc) || rc.offset != node.size)
        return -1;
    return 0;
}

static void fill_dir_entry(struct CosmoFSDirEntry *entry, uint32_t id, const struct CosmoFSNode *node,
                           const char *name, uint8_t len) {
    memset(entry, 0, sizeof(*entry));
    entry->node     = id;
    entry->size     = node->size;
    entry->type     = node->type;
    entry->name_len = len;
    memcpy(entry->name, name, len);
}

int8_t cosmofs_read_directory(struct CosmoFS *fs, struct CosmoFSRequest *request) {
    if (fs == NULL || !fs->mounted || request == NULL)
        return -1;
    struct CosmoFSNode parent, dir, up, child;
    if (!load_dir(fs, request->parent, &parent))
        return 3;
    if (request->name_len > 0 && request->name == NULL)
        return 2;
    if (resolve(fs, &parent, request->name, request->name_len, &dir) == 0)
        return 2;
    if (dir.type != COSMOFS_TYPE_DIR)
        return 1;
    uint32_t needed = (2 + dir.child_count) * (uint32_t) sizeof(struct CosmoFSDirEntry);
    if (request->buf == NULL || request->buffer_size < needed) {
        request->buffer_size = needed;
        return -1;
    }
    if (!node_load(fs, dir.parent, &up))
        return -1;
    struct CosmoFSDirEntry *out = (struct CosmoFSDirEntry*) request->buf;
    uint32_t count = 0;
    fill_dir_entry(&out[count++], dir.id, &dir, ".", 1);
    fill_dir_entry(&out[count++], up.id, &up, "..", 2);
    uint32_t next = dir.first_child;
    while (next != 0 && count < 2 + dir.child_count) {
        if (!node_load(fs, next, &child))
            return -1;
        fill_dir_entry(&out[count++], next, &child, child.name, child.name_len);
        next = child.next_sibling;
    }
    request->buffer_size = count * (uint32_t) sizeof(struct CosmoFSDirEntry);
    return 0;
}

int8_t cosmofs_delete(struct CosmoFS *fs, const struct CosmoFSRequest *request) {
    if (fs == NULL || !fs->mounted || request == NULL)
        return -1;
    struct CosmoFSNode parent, node;
    if (!load_dir(fs, request->parent, &parent))
        return 3;
    if (request->name == NULL || request->name_len == 0)
        return 1;
    if (is_dot(request->name, request->name_len) || is_dotdot(request->name, request->name_len))
        return -1;
    uint32_t id = index_lookup(fs, parent.id, request->name, request->name_len, &node);
    if (id == 0)
        return 1;
    if (id == COSMOFS_ROOT_NODE)
        return -1;
    if (node.type == COSMOFS_TYPE_DIR && node.child_count > 0)
        return 2;

    txn_begin(fs);
    if (node.type == COSMOFS_TYPE_FILE &&
        !for_each_extent(fs, &node, free_extent_visitor, free_extent_visitor, NULL))
        fs->txn_failed = true;
    if (!child_remove(fs, parent.id, &node))
        fs->txn_failed = true;
    index_remove(fs, name_hash(node.name, node.name_len), id);
    node_free(fs, &node);
    index_maintain(fs);
    return txn_commit(fs) ? 0 : -1;
}

int8_t cosmofs_rename(struct CosmoFS *fs, uint32_t parent_id, const char *name, uint8_t name_len,
                      uint32_t new_parent_id, const char *new_name, uint8_t new_name_len) {
    if (fs == NULL || !fs->mounted || name == NULL || name_len == 0 || is_dot(name, name_len) ||
        is_dotdot(name, name_len) || !valid_new_name(new_name, new_name_len))
        return -1;
    struct CosmoFSNode parent, new_parent, node, clash;
    if (!load_dir(fs, parent_id, &parent) || !load_dir(fs, new_parent_id, &new_parent))
        return 3;
    uint32_t id = index_lookup(fs, parent_id, name, name_len, &node);
    if (id == 0)
        return 1;
    if (parent_id == new_parent_id && name_len == new_name_len && memcmp(name, new_name, name_len) == 0)
        return 0;
    if (index_lookup(fs, new_parent_id, new_name, new_name_len, &clash) != 0)
        return 2;
    if (node.type == COSMOFS_TYPE_DIR) {
        uint32_t cursor = new_parent_id;
        for (uint32_t depth = 0; depth <= fs->sb.node_count; depth++) {
            if (cursor == id)
                return 4;
            if (cursor == COSMOFS_ROOT_NODE)
                break;
            struct CosmoFSNode up;
            if (!node_load(fs, cursor, &up))
                return -1;
            cursor = up.parent;
        }
    }

    txn_begin(fs);
    index_remove(fs, name_hash(node.name, node.name_len), id);
    if (!child_remove(fs, parent_id, &node))
        fs->txn_failed = true;
    memset(node.name, 0, sizeof(node.name));
    memcpy(node.name, new_name, new_name_len);
    node.name_len = new_name_len;
    node.mtime    = now(fs);
    if (!fs->txn_failed && !child_insert(fs, new_parent_id, &node))
        fs->txn_failed = true;
    node_store(fs, &node);
    index_insert(fs, name_hash(node.name, node.name_len), id);
    index_maintain(fs);
    return txn_commit(fs) ? 0 : -1;
}

static void fill_stat(struct CosmoFS *fs, const struct CosmoFSNode *node, struct CosmoFSStat *out) {
    memset(out, 0, sizeof(*out));
    out->node        = node->id;
    out->parent      = node->parent;
    out->type        = node->type;
    out->size        = node->size;
    out->block_count = node->block_count;
    out->child_count = node->child_count;
    out->ctime       = node->ctime;
    out->mtime       = node->mtime;
    memcpy(out->name, node->name, node->name_len);
    if (node->type == COSMOFS_TYPE_FILE)
        for_each_extent(fs, node, count_extent_visitor, NULL, &out->extent_count);
}

int8_t cosmofs_lookup(struct CosmoFS *fs, uint32_t parent_id, const char *name, uint8_t name_len, struct CosmoFSStat *out) {
    if (fs == NULL || !fs->mounted)
        return 2;
    struct CosmoFSNode parent, node;
    if (!load_dir(fs, parent_id, &parent))
        return 2;
    if (name == NULL || name_len == 0 || resolve(fs, &parent, name, name_len, &node) == 0)
        return 1;
    if (out != NULL)
        fill_stat(fs, &node, out);
    return 0;
}

bool cosmofs_stat(struct CosmoFS *fs, uint32_t id, struct CosmoFSStat *out) {
    struct CosmoFSNode node;
    if (fs == NULL || !fs->mounted || !node_load(fs, id, &node))
        return false;
    fill_stat(fs, &node, out);
    return true;
}

uint32_t cosmofs_find(struct CosmoFS *fs, const char *name, uint8_t name_len,
                      struct CosmoFSFindResult *results, uint32_t max_results) {
    if (fs == NULL || !fs->mounted || name == NULL || name_len == 0)
        return 0;
    uint32_t hash = name_hash(name, name_len), mask = fs->sb.index_slots - 1, count = 0;
    for (uint32_t k = 0, i = hash & mask; k < fs->sb.index_slots; k++, i = (i + 1) & mask) {
        struct CosmoFSIndexSlot slot = slot_read(fs, i);
        if (slot.node == COSMOFS_INDEX_EMPTY)
            break;
        if (slot.node == COSMOFS_INDEX_TOMBSTONE || slot.hash != hash)
            continue;
        struct CosmoFSNode node;
        if (!node_load(fs, slot.node, &node) || node.name_len != name_len || memcmp(node.name, name, name_len) != 0)
            continue;
        if (count < max_results) {
            results[count].node   = slot.node;
            results[count].parent = node.parent;
            results[count].type   = node.type;
        }
        count++;
    }
    return count;
}

void cosmofs_get_stats(struct CosmoFS *fs, struct CosmoFSStats *stats) {
    memset(stats, 0, sizeof(*stats));
    if (fs == NULL || !fs->mounted)
        return;
    stats->total_blocks          = fs->sb.total_blocks;
    stats->data_blocks           = fs->sb.total_blocks - fs->sb.data_start;
    stats->free_blocks           = fs->sb.free_blocks;
    stats->node_count            = fs->sb.node_count;
    stats->free_nodes            = fs->sb.free_nodes;
    stats->index_slots           = fs->sb.index_slots;
    stats->index_used            = fs->sb.node_count - fs->sb.free_nodes - 1;
    stats->index_tombstones      = fs->sb.index_tombstones;
    stats->journal_sequence      = fs->sb.journal_sequence;
    stats->mount_count           = fs->sb.mount_count;
    stats->replayed_transactions = fs->replayed_transactions;
    memcpy(stats->label, fs->sb.label, sizeof(stats->label));
}

/* ================================ CONSISTENCY CHECK ================================= */

struct CheckState {
    struct CosmoFSCheckReport *report;
    uint8_t  used[CHECK_MAX_BLOCKS / 8];
    uint8_t  seen[CHECK_MAX_NODES / 8 + 1];
    uint32_t queue[CHECK_MAX_NODES];
};

static void check_fail(struct CosmoFSCheckReport *r, const char *what, uint32_t value) {
    if (r->errors++ != 0)
        return;
    uint32_t len = 0;
    while (what[len] != '\0' && len < sizeof(r->message) - 12) {
        r->message[len] = what[len];
        len++;
    }
    char digits[11];
    uint32_t n = 0;
    do {
        digits[n++] = (char) ('0' + value % 10);
        value /= 10;
    } while (value != 0);
    r->message[len++] = ' ';
    while (n > 0)
        r->message[len++] = digits[--n];
    r->message[len] = '\0';
}

static struct CheckState check_state;

static bool check_extent_visitor(struct CosmoFS *fs, const struct CosmoFSExtent *extent, void *ctx) {
    struct CheckState *cs = (struct CheckState*) ctx;
    if (extent->length == 0 || extent->start < fs->sb.data_start || extent->length > fs->sb.total_blocks - extent->start) {
        check_fail(cs->report, "extent outside the data area at block", extent->start);
        return true;
    }
    cs->report->extents++;
    for (uint32_t b = extent->start; b < extent->start + extent->length; b++) {
        if ((cs->used[b >> 3] >> (b & 7)) & 1) {
            check_fail(cs->report, "block used twice", b);
            continue;
        }
        cs->used[b >> 3] |= (uint8_t) (1u << (b & 7));
        cs->report->used_blocks++;
    }
    return true;
}

bool cosmofs_check(struct CosmoFS *fs, struct CosmoFSCheckReport *report) {
    struct CheckState *cs = &check_state;
    memset(report, 0, sizeof(*report));
    memset(cs, 0, sizeof(*cs));
    cs->report = report;
    if (fs == NULL || !fs->mounted) {
        check_fail(report, "volume not mounted", 0);
        return false;
    }
    if (!sb_valid(&fs->sb, fs->dev.block_count) || fs->sb.total_blocks > CHECK_MAX_BLOCKS ||
        fs->sb.node_count > CHECK_MAX_NODES) {
        check_fail(report, "superblock invalid or too large to check", fs->sb.total_blocks);
        return false;
    }
    for (uint32_t b = 0; b < fs->sb.data_start; b++)
        cs->used[b >> 3] |= (uint8_t) (1u << (b & 7));

    // Breadth-first walk over the child lists
    uint32_t head = 0, tail = 0;
    cs->queue[tail++] = COSMOFS_ROOT_NODE;
    cs->seen[COSMOFS_ROOT_NODE >> 3] |= (uint8_t) (1u << (COSMOFS_ROOT_NODE & 7));
    struct CosmoFSNode root;
    if (!load_dir(fs, COSMOFS_ROOT_NODE, &root) || root.parent != COSMOFS_ROOT_NODE) {
        check_fail(report, "root node damaged", COSMOFS_ROOT_NODE);
        return false;
    }
    uint32_t reachable = 1;
    report->dirs = 1;
    while (head < tail) {
        uint32_t dir_id = cs->queue[head++];
        struct CosmoFSNode dir, child;
        if (!load_dir(fs, dir_id, &dir)) {
            check_fail(report, "directory node damaged", dir_id);
            continue;
        }
        uint32_t prev = 0, count = 0, next = dir.first_child;
        char last_name[COSMOFS_NAME_MAX + 1];
        uint8_t last_len = 0;
        while (next != 0) {
            if (!node_load(fs, next, &child)) {
                check_fail(report, "child node damaged (checksum / flags)", next);
                break;
            }
            if ((cs->seen[next >> 3] >> (next & 7)) & 1) {
                check_fail(report, "node reachable twice (cycle / cross link)", next);
                break;
            }
            cs->seen[next >> 3] |= (uint8_t) (1u << (next & 7));
            reachable++;
            count++;
            if (child.parent != dir_id)
                check_fail(report, "parent pointer mismatch on node", next);
            if (child.prev_sibling != prev)
                check_fail(report, "prev_sibling mismatch on node", next);
            if (count > 1 && name_compare(last_name, last_len, child.name, child.name_len) >= 0)
                check_fail(report, "children not sorted / duplicate name in dir", dir_id);
            if (!valid_new_name(child.name, child.name_len))
                check_fail(report, "invalid name on node", next);
            memcpy(last_name, child.name, child.name_len);
            last_len = child.name_len;

            struct CosmoFSNode indexed;
            if (index_lookup(fs, dir_id, child.name, child.name_len, &indexed) != next)
                check_fail(report, "node missing from the name index", next);

            if (child.type == COSMOFS_TYPE_DIR) {
                report->dirs++;
                if (child.size != 0 || child.extent_count != 0 || child.overflow != 0)
                    check_fail(report, "directory owns data blocks, node", next);
                if (tail < CHECK_MAX_NODES)
                    cs->queue[tail++] = next;
            } else {
                report->files++;
                uint32_t before = report->used_blocks;
                if (!for_each_extent(fs, &child, check_extent_visitor, check_extent_visitor, cs))
                    check_fail(report, "corrupted extent chain on node", next);
                uint32_t overflow_blocks = 0;
                for (uint32_t ob = child.overflow; ob != 0 && overflow_blocks <= COSMOFS_MAX_OVERFLOW; overflow_blocks++) {
                    static struct CosmoFSExtentBlock eb;
                    if (!load_extent_block(fs, ob, next, &eb))
                        break;
                    ob = eb.next;
                }
                if (report->used_blocks - before != child.block_count + overflow_blocks)
                    check_fail(report, "block_count mismatch on node", next);
                if (child.block_count != div_ceil(child.size, BS))
                    check_fail(report, "size / block_count mismatch on node", next);
                if (child.child_count != 0 || child.first_child != 0)
                    check_fail(report, "file has children, node", next);
            }
            prev = next;
            next = child.next_sibling;
        }
        if (count != dir.child_count)
            check_fail(report, "child_count mismatch on directory", dir_id);
    }

    // Every present node must be reachable, every free node must be really free
    uint32_t present = 0;
    for (uint32_t id = 1; id <= fs->sb.node_count; id++) {
        const struct CosmoFSNode *raw = (const struct CosmoFSNode*) (meta_read(fs, node_block(fs, id)) + node_offset(id));
        bool is_present = (raw->flags & COSMOFS_NODE_PRESENT) != 0;
        present += is_present;
        bool seen = (cs->seen[id >> 3] >> (id & 7)) & 1;
        if (is_present && !seen)
            check_fail(report, "orphan node (present but unreachable)", id);
    }
    if (present != reachable)
        check_fail(report, "present node count differs from reachable count", present);
    if (fs->sb.free_nodes != fs->sb.node_count - present)
        check_fail(report, "superblock free_nodes wrong, expected", fs->sb.node_count - present);

    // Bitmap must equal the rebuilt usage
    uint32_t free_blocks = 0;
    for (uint32_t b = 0; b < fs->sb.total_blocks; b++) {
        bool on_disk = bitmap_get(fs, b);
        bool in_use  = (cs->used[b >> 3] >> (b & 7)) & 1;
        if (on_disk && !in_use)
            check_fail(report, "leaked block (marked used, unreferenced)", b);
        if (!on_disk && in_use)
            check_fail(report, "block in use but marked free", b);
        free_blocks += !on_disk;
    }
    if (free_blocks != fs->sb.free_blocks)
        check_fail(report, "superblock free_blocks wrong, expected", free_blocks);

    // Index: no entry may point to a missing node
    uint32_t live = 0, tombstones = 0;
    for (uint32_t i = 0; i < fs->sb.index_slots; i++) {
        struct CosmoFSIndexSlot slot = slot_read(fs, i);
        if (slot.node == COSMOFS_INDEX_EMPTY)
            continue;
        if (slot.node == COSMOFS_INDEX_TOMBSTONE) {
            tombstones++;
            continue;
        }
        live++;
        struct CosmoFSNode node;
        if (!node_load(fs, slot.node, &node) || name_hash(node.name, node.name_len) != slot.hash)
            check_fail(report, "stale index slot", i);
    }
    if (live != reachable - 1)
        check_fail(report, "index entry count differs from node count", live);
    if (tombstones != fs->sb.index_tombstones)
        check_fail(report, "tombstone counter wrong, expected", tombstones);
    return report->errors == 0;
}
