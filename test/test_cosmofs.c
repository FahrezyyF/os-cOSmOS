// Host tests for src/filesystem/cosmofs.c, including crash-consistency (power loss) tests.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "header/filesystem/cosmofs.h"

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...) do {                                                   \
        checks++;                                                               \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            fprintf(stderr, "  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                       \
            fprintf(stderr, "\n");                                              \
            if (failures > 40) exit(1);                                         \
        }                                                                       \
    } while (0)

#define ROOT COSMOFS_ROOT_NODE
#define DISK_BLOCKS 8192u

/* --------------------------- memory device with power-loss simulation ------------- */

struct MemDevice {
    uint8_t  data[DISK_BLOCKS * COSMOFS_BLOCK_SIZE];
    int64_t  write_budget; // blocks that may still reach the disk, -1 = unlimited
    uint64_t writes;
    uint64_t flushes;
};

static struct MemDevice disk;
static struct MemDevice snapshot;

static bool md_read(void *ctx, uint32_t lba, uint32_t count, void *buf) {
    struct MemDevice *d = ctx;
    memcpy(buf, d->data + (size_t) lba * COSMOFS_BLOCK_SIZE, (size_t) count * COSMOFS_BLOCK_SIZE);
    return true;
}

static bool md_write(void *ctx, uint32_t lba, uint32_t count, const void *buf) {
    struct MemDevice *d = ctx;
    for (uint32_t i = 0; i < count; i++) {
        d->writes++;
        if (d->write_budget == 0)
            continue; // power is gone: the write silently never happens
        if (d->write_budget > 0)
            d->write_budget--;
        memcpy(d->data + (size_t) (lba + i) * COSMOFS_BLOCK_SIZE, (const uint8_t*) buf + (size_t) i * COSMOFS_BLOCK_SIZE, COSMOFS_BLOCK_SIZE);
    }
    return true;
}

static bool md_flush(void *ctx) {
    ((struct MemDevice*) ctx)->flushes++;
    return true;
}

static struct CosmoFSDevice device = {
    .context = &disk, .block_count = DISK_BLOCKS, .read = md_read, .write = md_write, .flush = md_flush,
};

static struct CosmoFS fs;

static uint32_t fake_clock(void) {
    return 1760000000u;
}

static void fresh(void) {
    memset(disk.data, 0, sizeof(disk.data));
    disk.write_budget = -1;
    CHECK(cosmofs_format(&device, "test", fake_clock), "format");
    CHECK(cosmofs_mount(&fs, &device, fake_clock) == 0, "mount");
}

static void remount(void) {
    disk.write_budget = -1;
    CHECK(cosmofs_mount(&fs, &device, fake_clock) == 0, "remount");
}

static void expect_consistent(const char *where) {
    struct CosmoFSCheckReport r;
    bool ok = cosmofs_check(&fs, &r);
    CHECK(ok, "%s: %u error(s): %s", where, r.errors, r.message);
}

static uint8_t *make_data(uint32_t size, uint32_t seed) {
    uint8_t *d = malloc(size ? size : 1);
    uint32_t s = seed * 2654435761u + 7;
    for (uint32_t i = 0; i < size; i++) {
        s = s * 1103515245u + 12345u;
        d[i] = (uint8_t) (s >> 16);
    }
    return d;
}

static int8_t put(uint32_t parent, const char *name, const void *data, uint32_t size) {
    struct CosmoFSRequest r = {.buf = (void*) data, .name = name, .name_len = (uint8_t) strlen(name),
                               .parent = parent, .buffer_size = size, .is_directory = false};
    return cosmofs_write(&fs, &r);
}

static int8_t mkdir_(uint32_t parent, const char *name) {
    struct CosmoFSRequest r = {.name = name, .name_len = (uint8_t) strlen(name), .parent = parent, .is_directory = true};
    return cosmofs_write(&fs, &r);
}

static int8_t get(uint32_t parent, const char *name, void *buf, uint32_t size) {
    struct CosmoFSRequest r = {.buf = buf, .name = name, .name_len = (uint8_t) strlen(name), .parent = parent, .buffer_size = size};
    return cosmofs_read(&fs, &r);
}

static int8_t rm(uint32_t parent, const char *name) {
    struct CosmoFSRequest r = {.name = name, .name_len = (uint8_t) strlen(name), .parent = parent};
    return cosmofs_delete(&fs, &r);
}

static uint32_t node_of(uint32_t parent, const char *name) {
    struct CosmoFSStat st;
    return cosmofs_lookup(&fs, parent, name, (uint8_t) strlen(name), &st) == 0 ? st.node : 0;
}

static int list(uint32_t parent, const char *name, struct CosmoFSDirEntry *out, int max) {
    struct CosmoFSRequest r = {.buf = out, .name = name, .name_len = (uint8_t) strlen(name), .parent = parent,
                               .buffer_size = (uint32_t) (max * sizeof(struct CosmoFSDirEntry))};
    int8_t ret = cosmofs_read_directory(&fs, &r);
    return ret == 0 ? (int) (r.buffer_size / sizeof(struct CosmoFSDirEntry)) : -100 - ret;
}

/* ---------------------------------------------------------------------------------- */

static void test_format_mount(void) {
    printf("[format] layout, signature, mount counter\n");
    fresh();
    struct CosmoFSStats st;
    cosmofs_get_stats(&fs, &st);
    CHECK(st.total_blocks == DISK_BLOCKS && st.node_count == 512 && st.index_slots == 1024, "geometry");
    CHECK(st.data_blocks == DISK_BLOCKS - 316 && st.free_blocks == st.data_blocks, "data area starts at 316");
    CHECK(st.free_nodes == 511 && st.mount_count == 1 && st.journal_sequence == 1, "counters (%u %u %u)", st.free_nodes, st.mount_count, st.journal_sequence);
    CHECK(strcmp(st.label, "test") == 0, "label");
    expect_consistent("format");
    remount();
    cosmofs_get_stats(&fs, &st);
    CHECK(st.mount_count == 2 && st.replayed_transactions == 0, "second mount");

    struct CosmoFSDirEntry e[4];
    CHECK(list(ROOT, ".", e, 4) == 2 && strcmp(e[0].name, ".") == 0 && strcmp(e[1].name, "..") == 0 &&
          e[1].node == ROOT, "root lists . and ..");

    // Unformatted device -> 1, corrupted superblock -> -1
    struct CosmoFS other;
    memcpy(snapshot.data, disk.data, sizeof(disk.data));
    memset(disk.data, 0, sizeof(disk.data));
    CHECK(cosmofs_mount(&other, &device, NULL) == 1, "blank device is not CosmoFS");
    memcpy(disk.data, snapshot.data, sizeof(disk.data));
    disk.data[COSMOFS_BLOCK_SIZE + 20] ^= 0x40;
    CHECK(cosmofs_mount(&other, &device, NULL) == -1, "superblock CRC detects corruption");
    memcpy(disk.data, snapshot.data, sizeof(disk.data));
}

static void test_crud(void) {
    printf("[crud] round trips and error codes\n");
    fresh();
    static const uint32_t sizes[] = {0, 1, 511, 512, 513, 2047, 2048, 2049, 100000, 1500000};
    char name[32];
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(name, sizeof(name), "f%u", sizes[i]);
        uint8_t *data = make_data(sizes[i], i);
        CHECK(put(ROOT, name, data, sizes[i]) == 0, "write %s", name);
        uint8_t *back = malloc(sizes[i] + 32);
        memset(back, 0xAB, sizes[i] + 32);
        CHECK(get(ROOT, name, back, sizes[i] + 32) == 0 && memcmp(back, data, sizes[i]) == 0, "read %s", name);
        CHECK(back[sizes[i]] == 0xAB && back[sizes[i] + 31] == 0xAB, "no overrun %s", name);
        if (sizes[i])
            CHECK(get(ROOT, name, back, sizes[i] - 1) == 2, "small buffer %s", name);
        struct CosmoFSStat st;
        CHECK(cosmofs_lookup(&fs, ROOT, name, (uint8_t) strlen(name), &st) == 0 && st.size == sizes[i] &&
              st.extent_count == (sizes[i] ? 1u : 0u), "one contiguous extent for %s (%u)", name, st.extent_count);
        free(back);
        free(data);
    }
    expect_consistent("crud files");

    CHECK(mkdir_(ROOT, "dir") == 0, "mkdir");
    uint32_t dir = node_of(ROOT, "dir"), file = node_of(ROOT, "f1");
    CHECK(put(ROOT, "f1", "x", 1) == 1 && mkdir_(ROOT, "f1") == 1 && put(ROOT, "dir", "x", 1) == 1, "exists -> 1");
    CHECK(put(file, "x", "x", 1) == 2 && put(999, "x", "x", 1) == 2 && put(0, "x", "x", 1) == 2, "bad parent -> 2");
    CHECK(put(ROOT, ".", "x", 1) == -1 && put(ROOT, "a/b", "x", 1) == -1 && put(ROOT, "", "x", 0) == -1, "bad names -> -1");
    char longname[200];
    memset(longname, 'n', 128);
    longname[128] = '\0';
    CHECK(put(ROOT, longname, "x", 1) == -1, "name longer than 127 -> -1");
    longname[127] = '\0';
    CHECK(put(ROOT, longname, "x", 1) == 0, "127-byte name ok");

    uint8_t buf[64];
    CHECK(get(ROOT, "dir", buf, sizeof(buf)) == 1 && get(ROOT, "nope", buf, sizeof(buf)) == 3 &&
          get(file, "x", buf, sizeof(buf)) == 4, "read codes");
    struct CosmoFSDirEntry e[8];
    struct CosmoFSRequest rd = {.buf = e, .name = "f1", .name_len = 2, .parent = ROOT, .buffer_size = sizeof(e)};
    CHECK(cosmofs_read_directory(&fs, &rd) == 1, "read_directory on file -> 1");
    rd.name = "zz"; rd.buffer_size = sizeof(e);
    CHECK(cosmofs_read_directory(&fs, &rd) == 2, "missing -> 2");
    rd.parent = file; rd.name = "x"; rd.name_len = 1; rd.buffer_size = sizeof(e);
    CHECK(cosmofs_read_directory(&fs, &rd) == 3, "bad parent -> 3");
    rd.parent = ROOT; rd.name = "."; rd.buffer_size = 10;
    CHECK(cosmofs_read_directory(&fs, &rd) == -1 && rd.buffer_size == (2 + 12) * sizeof(struct CosmoFSDirEntry), "needed size reported (%u)", rd.buffer_size);

    CHECK(put(dir, "inner", "abc", 3) == 0, "inner");
    CHECK(rm(ROOT, "dir") == 2 && rm(ROOT, "missing") == 1 && rm(file, "x") == 3 && rm(ROOT, "..") == -1, "delete codes");
    CHECK(rm(dir, "inner") == 0 && rm(ROOT, "dir") == 0, "delete ok");
    expect_consistent("crud codes");

    // Data survives a remount
    remount();
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(name, sizeof(name), "f%u", sizes[i]);
        uint8_t *data = make_data(sizes[i], i);
        uint8_t *back = malloc(sizes[i] + 1);
        CHECK(get(ROOT, name, back, sizes[i]) == 0 && memcmp(back, data, sizes[i]) == 0, "after remount %s", name);
        free(back);
        free(data);
    }
    expect_consistent("crud remount");
}

static void test_sorted_listing(void) {
    printf("[dir] children are kept sorted by name\n");
    fresh();
    const char *names[] = {"zeta", "alpha", "mid", "Beta", "a", "alphabet", "z", "0first"};
    for (int i = 0; i < 8; i++)
        CHECK(put(ROOT, names[i], names[i], (uint32_t) strlen(names[i])) == 0, "put %s", names[i]);
    struct CosmoFSDirEntry e[16];
    int n = list(ROOT, ".", e, 16);
    CHECK(n == 10, "count %d", n);
    for (int i = 3; i < n; i++)
        CHECK(strcmp(e[i - 1].name, e[i].name) < 0, "order %s < %s", e[i - 1].name, e[i].name);
    CHECK(cosmofs_rename(&fs, ROOT, "zeta", 4, ROOT, "aaa", 3) == 0, "rename re-sorts");
    n = list(ROOT, ".", e, 16);
    int pos_a = -1;
    for (int i = 2; i < n; i++)
        if (strcmp(e[i].name, "a") == 0)
            pos_a = i;
    // Byte order: "0first" < "Beta" < "a" < "aaa" < "alpha" ...
    CHECK(pos_a == 4 && strcmp(e[pos_a + 1].name, "aaa") == 0 && strcmp(e[3].name, "Beta") == 0, "aaa right after a (%d)", pos_a);
    expect_consistent("sorted");
}

static void test_extents(void) {
    printf("[alloc] extents: best fit, fragmentation, overflow blocks\n");
    fresh();
    char name[16];
    uint8_t *one = make_data(512, 1);
    for (int i = 0; i < 400; i++) {
        snprintf(name, sizeof(name), "s%03d", i);
        CHECK(put(ROOT, name, one, 512) == 0, "small %d", i);
    }
    for (int i = 0; i < 400; i += 2) {
        snprintf(name, sizeof(name), "s%03d", i);
        CHECK(rm(ROOT, name) == 0, "rm %d", i);
    }
    // Free space = 200 single-block holes + the big tail. A 3-block file uses the tail (best fit
    // would need a run >= 3), a 1-block file must reuse a 1-block hole.
    CHECK(put(ROOT, "hole", one, 512) == 0, "one-block file");
    struct CosmoFSStat st;
    cosmofs_lookup(&fs, ROOT, "hole", 4, &st);
    CHECK(st.extent_count == 1, "hole extent");

    // Fill the tail completely, then a file that must be scattered over the holes
    struct CosmoFSStats stats;
    cosmofs_get_stats(&fs, &stats);
    uint32_t tail = stats.free_blocks - 199;
    uint8_t *big = make_data(tail * 512, 2);
    CHECK(put(ROOT, "tailfill", big, tail * 512) == 0, "tail fill (%u blocks)", tail);
    free(big);
    uint8_t *scatter = make_data(150 * 512 + 100, 3);
    CHECK(put(ROOT, "scatter", scatter, 150 * 512 + 100) == 0, "scattered file");
    cosmofs_lookup(&fs, ROOT, "scatter", 7, &st);
    CHECK(st.extent_count == 151, "151 extents (%u) -> needs 3 overflow blocks", st.extent_count);
    uint8_t *back = malloc(150 * 512 + 100);
    CHECK(get(ROOT, "scatter", back, 150 * 512 + 100) == 0 && memcmp(back, scatter, 150 * 512 + 100) == 0, "scattered content");
    expect_consistent("extents");
    remount();
    CHECK(get(ROOT, "scatter", back, 150 * 512 + 100) == 0 && memcmp(back, scatter, 150 * 512 + 100) == 0, "scattered after remount");
    CHECK(rm(ROOT, "scatter") == 0, "free scattered file + overflow blocks");
    expect_consistent("extents freed");
    // Disk full: the failed write must not change anything
    cosmofs_get_stats(&fs, &stats);
    uint32_t before = stats.free_blocks;
    uint8_t *huge = make_data((before + 1) * 512, 4);
    CHECK(put(ROOT, "huge", huge, (before + 1) * 512) == -1, "too big -> -1");
    cosmofs_get_stats(&fs, &stats);
    CHECK(stats.free_blocks == before && node_of(ROOT, "huge") == 0, "failed write is atomic");
    expect_consistent("full");
    free(huge);
    free(back);
    free(scatter);
    free(one);
}

static void test_index(void) {
    printf("[index] find, lookups, tombstone rebuild\n");
    fresh();
    mkdir_(ROOT, "a");
    mkdir_(ROOT, "b");
    uint32_t a = node_of(ROOT, "a"), b = node_of(ROOT, "b");
    mkdir_(b, "c");
    uint32_t c = node_of(b, "c");
    put(ROOT, "needle", "1", 1);
    put(a, "needle", "2", 1);
    put(c, "needle", "3", 1);
    struct CosmoFSFindResult res[8];
    CHECK(cosmofs_find(&fs, "needle", 6, res, 8) == 3, "three needles");
    CHECK(cosmofs_find(&fs, "needl", 5, res, 8) == 0, "no prefix match");
    CHECK(cosmofs_rename(&fs, c, "needle", 6, a, "moved", 5) == 0, "move");
    CHECK(cosmofs_find(&fs, "needle", 6, res, 8) == 2 && cosmofs_find(&fs, "moved", 5, res, 8) == 1 && res[0].parent == a, "index follows rename");
    CHECK(cosmofs_rename(&fs, ROOT, "b", 1, c, "x", 1) == 4, "dir into own subdir -> 4");
    CHECK(cosmofs_rename(&fs, ROOT, "b", 1, ROOT, "a", 1) == 2, "clash -> 2");

    // Churn creates tombstones; the index rebuilds itself once they exceed a quarter of the slots
    char name[32];
    uint32_t max_tombstones = 0;
    for (int round = 0; round < 6; round++) {
        for (int i = 0; i < 100; i++) {
            snprintf(name, sizeof(name), "t%d_%d", round, i);
            put(ROOT, name, "z", 1);
        }
        for (int i = 0; i < 100; i++) {
            snprintf(name, sizeof(name), "t%d_%d", round, i);
            CHECK(rm(ROOT, name) == 0, "rm %s", name);
        }
        struct CosmoFSStats st;
        cosmofs_get_stats(&fs, &st);
        if (st.index_tombstones > max_tombstones)
            max_tombstones = st.index_tombstones;
        CHECK(st.index_tombstones <= st.index_slots / 4 + 1, "tombstones bounded (%u)", st.index_tombstones);
    }
    CHECK(max_tombstones > 100, "tombstones did accumulate before rebuilds (%u)", max_tombstones);
    CHECK(cosmofs_find(&fs, "needle", 6, res, 8) == 2, "lookups still work after rebuilds");
    expect_consistent("index churn");
}

/* --------------------------- crash consistency ------------------------------------ */

// Deterministic text description of the whole tree (names, types, sizes, content checksums)
static void describe(uint32_t dir, int depth, char *out, size_t cap) {
    static struct CosmoFSDirEntry entries[16][600];
    if (depth >= 16)
        return;
    int n = list(dir, ".", entries[depth], 600);
    for (int i = 2; i < n; i++) {
        struct CosmoFSDirEntry *e = &entries[depth][i];
        char line[256];
        uint32_t sum = 0;
        if (e->type == COSMOFS_TYPE_FILE) {
            uint8_t *buf = malloc(e->size + 1);
            if (get(dir, e->name, buf, e->size) == 0)
                for (uint32_t k = 0; k < e->size; k++)
                    sum = sum * 31 + buf[k];
            free(buf);
        }
        snprintf(line, sizeof(line), "%*s%s %c %u %08x\n", depth, "", e->name, e->type == COSMOFS_TYPE_DIR ? 'd' : 'f', e->size, sum);
        strncat(out, line, cap - strlen(out) - 1);
        if (e->type == COSMOFS_TYPE_DIR)
            describe(e->node, depth + 1, out, cap);
    }
}

typedef void (*CrashOp)(void);

static void op_create_big(void)   { uint8_t *d = make_data(40000, 9); put(node_of(ROOT, "docs"), "big.bin", d, 40000); free(d); }
static void op_mkdir(void)        { mkdir_(node_of(ROOT, "docs"), "newdir"); }
static void op_delete_file(void)  { rm(ROOT, "keep.txt"); }
static void op_delete_dir(void)   { rm(ROOT, "empty"); }
static void op_move_dir(void)     { cosmofs_rename(&fs, ROOT, "docs", 4, node_of(ROOT, "empty"), "docs2", 5); }
// 10 free runs of 3 blocks: 25 blocks -> 9 extents (8 inline + 1 in an overflow block) + the overflow block
static void op_scatter(void)      { uint8_t *d = make_data(25 * 512, 11); put(ROOT, "scattered", d, 25 * 512); free(d); }

static void crash_base(bool fragmented) {
    fresh();
    mkdir_(ROOT, "docs");
    mkdir_(ROOT, "empty");
    put(ROOT, "keep.txt", "keep me", 7);
    uint8_t *d = make_data(5000, 5);
    put(node_of(ROOT, "docs"), "data", d, 5000);
    free(d);
    if (fragmented) {
        // Leave only one-block holes so the next file needs > 8 extents (overflow block)
        struct CosmoFSStats st;
        cosmofs_get_stats(&fs, &st);
        char name[16];
        uint8_t blk[512] = {0};
        for (int i = 0; i < 40; i++) {
            snprintf(name, sizeof(name), "h%02d", i);
            put(ROOT, name, blk, 512);
        }
        cosmofs_get_stats(&fs, &st);
        uint8_t *fill = make_data((st.free_blocks) * 512, 3);
        put(ROOT, "filler", fill, st.free_blocks * 512);
        free(fill);
        for (int i = 0; i < 40; i += 1) {
            snprintf(name, sizeof(name), "h%02d", i);
            if (i % 4 != 0)
                rm(ROOT, name);
        }
    }
}

static void crash_test(const char *label, CrashOp op, bool fragmented) {
    static char before[65536], after[65536], now_state[65536];
    crash_base(fragmented);
    memcpy(snapshot.data, disk.data, sizeof(disk.data));
    before[0] = '\0';
    describe(ROOT, 0, before, sizeof(before));

    // Reference run without a crash: count the block writes of the operation
    remount();
    uint64_t start = disk.writes;
    op();
    uint64_t total = disk.writes - start;
    after[0] = '\0';
    describe(ROOT, 0, after, sizeof(after));
    CHECK(strcmp(before, after) != 0, "%s changes the tree", label);

    int replays = 0, olds = 0, news = 0;
    for (uint64_t budget = 0; budget <= total; budget++) {
        memcpy(disk.data, snapshot.data, sizeof(disk.data));
        disk.write_budget = -1;
        if (cosmofs_mount(&fs, &device, fake_clock) != 0) {
            CHECK(false, "%s: base mount", label);
            continue;
        }
        disk.write_budget = (int64_t) budget; // power fails after `budget` block writes
        op();
        disk.write_budget = -1;
        int8_t m = cosmofs_mount(&fs, &device, fake_clock); // reboot
        CHECK(m == 0, "%s: mount after crash at %lu", label, (unsigned long) budget);
        if (m != 0)
            continue;
        struct CosmoFSStats st;
        cosmofs_get_stats(&fs, &st);
        replays += st.replayed_transactions != 0;
        expect_consistent(label);
        now_state[0] = '\0';
        describe(ROOT, 0, now_state, sizeof(now_state));
        bool is_old = strcmp(now_state, before) == 0, is_new = strcmp(now_state, after) == 0;
        olds += is_old;
        news += is_new;
        CHECK(is_old || is_new, "%s: crash after %lu/%lu writes left a mixed state", label, (unsigned long) budget, (unsigned long) total);
    }
    printf("  %-28s %3lu crash points: %d old, %d new, %d journal replays\n", label, (unsigned long) total + 1, olds, news, replays);
    CHECK(olds > 0 && news > 0 && replays > 0, "%s: crash points cover before / after / replay", label);
}

static void test_crash_consistency(void) {
    printf("[journal] power loss at every block write of an operation\n");
    crash_test("create 40 KB file", op_create_big, false);
    crash_test("mkdir", op_mkdir, false);
    crash_test("delete file", op_delete_file, false);
    crash_test("delete empty dir", op_delete_dir, false);
    crash_test("move directory", op_move_dir, false);
    crash_test("create fragmented file", op_scatter, true);
}

/* --------------------------- randomized model ------------------------------------- */

#define MODEL_MAX 600 // must exceed the node count so the model never drops an entry

struct ModelEntry {
    bool     used, is_dir;
    uint32_t parent, node;
    char     name[64];
    uint8_t *data;
    uint32_t size;
};
static struct ModelEntry model[MODEL_MAX];
static uint32_t rng_state = 99;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int model_find(uint32_t parent, const char *name) {
    for (int i = 0; i < MODEL_MAX; i++)
        if (model[i].used && model[i].parent == parent && strcmp(model[i].name, name) == 0)
            return i;
    return -1;
}

static int model_children(uint32_t dir) {
    int n = 0;
    for (int i = 0; i < MODEL_MAX; i++)
        n += model[i].used && model[i].parent == dir;
    return n;
}

static uint32_t model_dir(void) {
    uint32_t dirs[MODEL_MAX + 1];
    int n = 0;
    dirs[n++] = ROOT;
    for (int i = 0; i < MODEL_MAX; i++)
        if (model[i].used && model[i].is_dir)
            dirs[n++] = model[i].node;
    return dirs[rng() % n];
}

static int model_any(void) {
    int idx[MODEL_MAX], n = 0;
    for (int i = 0; i < MODEL_MAX; i++)
        if (model[i].used)
            idx[n++] = i;
    return n ? idx[rng() % n] : -1;
}

static bool model_ancestor(uint32_t anc, uint32_t dir) {
    for (int guard = 0; guard < 500; guard++) {
        if (dir == anc)
            return true;
        if (dir == ROOT)
            return false;
        int f = -1;
        for (int i = 0; i < MODEL_MAX; i++)
            if (model[i].used && model[i].node == dir)
                f = i;
        if (f < 0)
            return false;
        dir = model[f].parent;
    }
    return false;
}

static void test_random_model(void) {
    printf("[model] 3000 random operations against an in-memory model\n");
    fresh();
    memset(model, 0, sizeof(model));
    static const char *names[] = {"a", "b", "c", "notes", "img.png", "dir", "z", "a_long_name_for_testing_sorting"};
    for (int step = 1; step <= 3000; step++) {
        uint32_t op = rng() % 100, parent = model_dir();
        const char *name = names[rng() % 8];
        int existing = model_find(parent, name);
        if (op < 40) {
            uint32_t size = rng() % 4 == 0 ? rng() % 120000 : rng() % 4000;
            uint8_t *d = make_data(size, step);
            int8_t ret = put(parent, name, d, size);
            if (existing >= 0) {
                CHECK(ret == 1, "step %d dup", step);
                free(d);
            } else if (ret == 0) {
                int s = 0;
                while (s < MODEL_MAX && model[s].used) s++;
                if (s == MODEL_MAX) { free(d); continue; }
                model[s] = (struct ModelEntry) {.used = true, .parent = parent, .node = node_of(parent, name), .data = d, .size = size};
                strcpy(model[s].name, name);
            } else {
                struct CosmoFSStats st;
                cosmofs_get_stats(&fs, &st);
                CHECK(ret == -1 && (st.free_nodes == 0 || st.free_blocks < (size + 511) / 512 + 20), "step %d: -1 with space", step);
                free(d);
            }
        } else if (op < 55) {
            int8_t ret = mkdir_(parent, name);
            if (existing >= 0) {
                CHECK(ret == 1, "step %d dup dir", step);
            } else if (ret == 0) {
                int s = 0;
                while (s < MODEL_MAX && model[s].used) s++;
                if (s == MODEL_MAX) continue;
                model[s] = (struct ModelEntry) {.used = true, .is_dir = true, .parent = parent, .node = node_of(parent, name)};
                strcpy(model[s].name, name);
            }
        } else if (op < 80) {
            int v = model_any();
            if (v < 0) continue;
            int8_t ret = rm(model[v].parent, model[v].name);
            if (model[v].is_dir && model_children(model[v].node) > 0) {
                CHECK(ret == 2, "step %d rm non-empty -> %d", step, ret);
            } else {
                CHECK(ret == 0, "step %d rm -> %d", step, ret);
                free(model[v].data);
                memset(&model[v], 0, sizeof(model[v]));
            }
        } else if (op < 90) {
            int t = model_any();
            if (t < 0 || model[t].is_dir) continue;
            uint8_t *back = malloc(model[t].size + 1);
            CHECK(get(model[t].parent, model[t].name, back, model[t].size) == 0 && memcmp(back, model[t].data, model[t].size) == 0, "step %d read", step);
            free(back);
        } else {
            int s = model_any();
            if (s < 0) continue;
            uint32_t dest = model_dir();
            int8_t ret = cosmofs_rename(&fs, model[s].parent, model[s].name, (uint8_t) strlen(model[s].name), dest, name, (uint8_t) strlen(name));
            int clash = model_find(dest, name);
            if (clash == s) {
                CHECK(ret == 0, "step %d self rename", step);
            } else if (clash >= 0) {
                CHECK(ret == 2, "step %d rename clash -> %d", step, ret);
            } else if (model[s].is_dir && model_ancestor(model[s].node, dest)) {
                CHECK(ret == 4, "step %d rename cycle -> %d", step, ret);
            } else {
                CHECK(ret == 0, "step %d rename -> %d", step, ret);
                if (ret == 0) {
                    model[s].parent = dest;
                    strcpy(model[s].name, name);
                }
            }
        }
        if (step % 150 == 0)
            expect_consistent("random");
        if (step % 600 == 0) {
            remount();
            for (int i = 0; i < MODEL_MAX; i++) {
                if (!model[i].used)
                    continue;
                CHECK(node_of(model[i].parent, model[i].name) == model[i].node, "remount lookup %s", model[i].name);
                if (!model[i].is_dir) {
                    uint8_t *back = malloc(model[i].size + 1);
                    CHECK(get(model[i].parent, model[i].name, back, model[i].size) == 0 && memcmp(back, model[i].data, model[i].size) == 0, "remount content");
                    free(back);
                }
            }
        }
    }
    for (int i = 0; i < MODEL_MAX; i++)
        free(model[i].data);
}

static void test_checksum_detection(void) {
    printf("[crc] corrupted metadata is detected\n");
    fresh();
    put(ROOT, "victim", "data", 4);
    struct CosmoFSStat st;
    cosmofs_lookup(&fs, ROOT, "victim", 6, &st);
    // Flip one byte of the node on disk, bypassing the driver
    uint32_t node_block = 44 + (st.node - 1) / 2, offset = ((st.node - 1) % 2) * 256 + 100;
    disk.data[node_block * COSMOFS_BLOCK_SIZE + offset] ^= 0x01;
    remount();
    uint8_t buf[8];
    CHECK(get(ROOT, "victim", buf, sizeof(buf)) == 3, "damaged node is not served");
    struct CosmoFSCheckReport r;
    CHECK(!cosmofs_check(&fs, &r) && r.errors > 0, "fsck reports the damage: %s", r.message);
}

int main(void) {
    test_format_mount();
    test_crud();
    test_sorted_listing();
    test_extents();
    test_index();
    test_crash_consistency();
    test_random_model();
    test_checksum_detection();
    printf("cosmofs: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
