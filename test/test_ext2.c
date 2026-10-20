// Host tests for src/filesystem/ext2.c (EXT2 - IF2130 Edition).
// Build & run: make test   (compiled with -Dread=ext2_read -Dwrite=ext2_write -Ddelete=ext2_delete
// so the driver's CRUD functions do not collide with the POSIX ones of the host libc)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ramdisk.h"
#include "header/filesystem/ext2.h"

extern const uint8_t fs_signature[BLOCK_SIZE];

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

#define ROOT EXT2_ROOT_INODE

static uint32_t rng_state = 12345;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static uint8_t *make_data(uint32_t size, uint32_t seed) {
    uint8_t *d = malloc(size ? size : 1);
    uint32_t s = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < size; i++) {
        s = s * 1103515245u + 12345u;
        d[i] = (uint8_t) (s >> 16);
    }
    return d;
}

static int8_t write_file(uint32_t parent, const char *name, const void *data, uint32_t size) {
    struct EXT2DriverRequest req = {
        .buf = (void*) data, .name = (char*) name, .name_len = (uint8_t) strlen(name),
        .parent_inode = parent, .buffer_size = size, .is_directory = false,
    };
    return write(&req);
}

static int8_t make_dir(uint32_t parent, const char *name) {
    struct EXT2DriverRequest req = {
        .buf = NULL, .name = (char*) name, .name_len = (uint8_t) strlen(name),
        .parent_inode = parent, .buffer_size = 0, .is_directory = true,
    };
    return write(&req);
}

static int8_t read_file(uint32_t parent, const char *name, void *buf, uint32_t size) {
    struct EXT2DriverRequest req = {
        .buf = buf, .name = (char*) name, .name_len = (uint8_t) strlen(name),
        .parent_inode = parent, .buffer_size = size,
    };
    return read(req);
}

static int8_t remove_entry(uint32_t parent, const char *name) {
    struct EXT2DriverRequest req = {
        .name = (char*) name, .name_len = (uint8_t) strlen(name), .parent_inode = parent,
    };
    return delete(req);
}

static uint32_t lookup(uint32_t parent, const char *name) {
    uint32_t inode = 0;
    if (ext2_lookup(parent, name, (uint8_t) strlen(name), &inode, NULL) != 0)
        return 0;
    return inode;
}

static void expect_consistent(const char *where) {
    struct EXT2CheckReport report;
    bool ok = ext2_check(&report);
    CHECK(ok, "%s: ext2_check found %u error(s): %s", where, report.errors, report.message);
}

static uint32_t free_blocks(void) {
    struct EXT2Stats st;
    ext2_get_stats(&st);
    return st.free_blocks_count;
}

static uint32_t free_inodes(void) {
    struct EXT2Stats st;
    ext2_get_stats(&st);
    return st.free_inodes_count;
}

static void fresh_fs(void) {
    ramdisk_reset();
    initialize_filesystem_ext2();
}

// Collect the live names of a directory through the public read_directory() interface
static int list_dir(uint32_t parent, const char *name, char names[][EXT2_NAME_MAX + 1], uint32_t *inodes, int max) {
    static uint8_t buf[64 * BLOCK_SIZE];
    struct EXT2DriverRequest req = {
        .buf = buf, .name = (char*) name, .name_len = (uint8_t) strlen(name),
        .parent_inode = parent, .buffer_size = sizeof(buf),
    };
    int8_t ret = read_directory(&req);
    if (ret != 0)
        return -100 - ret;
    int count = 0;
    for (uint32_t block = 0; block < sizeof(buf) / BLOCK_SIZE; block++) {
        uint8_t *b = buf + block * BLOCK_SIZE;
        if (get_directory_entry(b, 0)->rec_len == 0)
            break;
        uint32_t off = 0;
        while (off < BLOCK_SIZE) {
            struct EXT2DirectoryEntry *e = get_directory_entry(b, off);
            if (e->rec_len == 0)
                break;
            if (e->inode != 0 && count < max) {
                memcpy(names[count], get_entry_name(e), e->name_len);
                names[count][e->name_len] = '\0';
                if (inodes)
                    inodes[count] = e->inode;
                count++;
            }
            off += e->rec_len;
        }
    }
    return count;
}

/* ---------------------------------------------------------------------------------- */

static void test_format_layout(void) {
    printf("[format] fresh image layout\n");
    fresh_fs();
    CHECK(memcmp(ramdisk, fs_signature, BLOCK_SIZE) == 0, "boot sector must hold fs_signature");
    CHECK(!is_empty_storage(), "formatted storage must not be empty");

    struct EXT2Superblock sb;
    memcpy(&sb, ramdisk + BLOCK_SIZE, sizeof(sb));
    CHECK(sb.s_magic == EXT2_SUPER_MAGIC, "magic %x", sb.s_magic);
    CHECK(sb.s_inodes_count == 512 && sb.s_blocks_count == 8192, "counts %u %u", sb.s_inodes_count, sb.s_blocks_count);
    CHECK(sb.s_blocks_per_group == 1024 && sb.s_inodes_per_group == 64, "per group %u %u", sb.s_blocks_per_group, sb.s_inodes_per_group);
    CHECK(sb.s_free_inodes_count == 511, "free inodes %u", sb.s_free_inodes_count);
    // group 0 loses 21 metadata blocks + root dir block, groups 1-7 lose 18 each
    CHECK(sb.s_free_blocks_count == 8192 - 21 - 1 - 7 * 18, "free blocks %u", sb.s_free_blocks_count);

    struct EXT2BlockGroupDescriptor bgd[8];
    memcpy(bgd, ramdisk + 2 * BLOCK_SIZE, sizeof(bgd));
    // Same layout as the reference sample-image.bin
    CHECK(bgd[0].bg_block_bitmap == 3 && bgd[0].bg_inode_bitmap == 4 && bgd[0].bg_inode_table == 5, "bgd0");
    for (uint32_t g = 1; g < 8; g++)
        CHECK(bgd[g].bg_block_bitmap == g * 1024 && bgd[g].bg_inode_bitmap == g * 1024 + 1 &&
              bgd[g].bg_inode_table == g * 1024 + 2 && bgd[g].bg_free_blocks_count == 1006, "bgd%u", g);
    CHECK(bgd[0].bg_used_dirs_count == 1, "root counted as dir");

    // Root inode = 1 -> first slot of group 0's inode table, data block 21
    struct EXT2Inode root;
    memcpy(&root, ramdisk + 5 * BLOCK_SIZE, sizeof(root));
    CHECK(root.i_mode == EXT2_S_IFDIR && root.i_block[0] == 21 && root.i_size == BLOCK_SIZE, "root inode");
    CHECK(root.i_links_count == 2 && root.i_blocks == 1, "root links/blocks");
    struct EXT2DirectoryEntry *dot = get_directory_entry(ramdisk + 21 * BLOCK_SIZE, 0);
    struct EXT2DirectoryEntry *dotdot = get_next_directory_entry(dot);
    CHECK(dot->inode == ROOT && dot->name_len == 1 && dot->rec_len == 12 && get_entry_name(dot)[0] == '.', "'.'");
    CHECK(dotdot->inode == ROOT && dotdot->name_len == 2 && dotdot->rec_len == 500, "'..' spans the block");
    CHECK(get_dir_first_child_offset(ramdisk + 21 * BLOCK_SIZE) == BLOCK_SIZE, "no child yet");
    CHECK(ramdisk[3 * BLOCK_SIZE] == 0xFF && ramdisk[3 * BLOCK_SIZE + 2] == 0xFC, "MSB-first block bitmap: blocks 0..21 used");
    CHECK(ramdisk[4 * BLOCK_SIZE] == 0x80, "inode bitmap: inode 1 used");
    CHECK(is_directory_empty(ROOT), "root starts empty");
    expect_consistent("format");

    // Helper functions from the kit
    CHECK(get_entry_record_len(1) == 12 && get_entry_record_len(4) == 12 && get_entry_record_len(5) == 16, "rec len");
    CHECK(inode_to_bgd(1) == 0 && inode_to_bgd(64) == 0 && inode_to_bgd(65) == 1, "inode_to_bgd");
    CHECK(inode_to_local(1) == 0 && inode_to_local(65) == 0 && inode_to_local(130) == 1, "inode_to_local");
}

static void test_file_sizes(void) {
    printf("[crud] file round trips across block boundaries\n");
    fresh_fs();
    static const uint32_t sizes[] = {
        0, 1, 2, 100, 511, 512, 513, 1023, 1024, 2047, 2048, 2049,
        12 * 512 - 1, 12 * 512, 12 * 512 + 1,        // end of direct blocks
        140 * 512 - 1, 140 * 512, 140 * 512 + 1,     // end of single indirect
        200000, 1048576,                              // doubly indirect
    };
    char name[32];
    uint32_t used_before = free_blocks();
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(name, sizeof(name), "file_%u", sizes[i]);
        uint8_t *data = make_data(sizes[i], i + 1);
        CHECK(write_file(ROOT, name, data, sizes[i]) == 0, "write %s", name);

        uint32_t inode = lookup(ROOT, name);
        struct EXT2Inode node;
        CHECK(ext2_get_inode(inode, &node), "inode of %s", name);
        uint32_t data_blocks = (sizes[i] + 511) / 512;
        CHECK(node.i_size == sizes[i], "%s size %u", name, node.i_size);
        CHECK(node.i_mode == EXT2_S_IFREG && node.i_links_count == 1, "%s mode", name);
        CHECK(node.i_blocks >= data_blocks, "%s i_blocks %u", name, node.i_blocks);

        uint8_t *back = malloc(sizes[i] + 64);
        memset(back, 0xEE, sizes[i] + 64);
        CHECK(read_file(ROOT, name, back, sizes[i]) == 0, "read %s", name);
        CHECK(memcmp(back, data, sizes[i]) == 0, "content of %s", name);
        // read must never write past the file size even with a bigger buffer
        memset(back, 0xEE, sizes[i] + 64);
        CHECK(read_file(ROOT, name, back, sizes[i] + 64) == 0, "read %s with slack", name);
        CHECK(back[sizes[i]] == 0xEE && back[sizes[i] + 63] == 0xEE, "%s overrun", name);
        if (sizes[i] > 0)
            CHECK(read_file(ROOT, name, back, sizes[i] - 1) == 2, "%s small buffer -> 2", name);
        free(back);
        free(data);
    }
    CHECK(free_blocks() < used_before, "blocks consumed");
    expect_consistent("sizes");

    // Data must survive a remount (non-volatile storage)
    initialize_filesystem_ext2();
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(name, sizeof(name), "file_%u", sizes[i]);
        uint8_t *data = make_data(sizes[i], i + 1);
        uint8_t *back = malloc(sizes[i] + 1);
        CHECK(read_file(ROOT, name, back, sizes[i]) == 0 && memcmp(back, data, sizes[i]) == 0, "remount %s", name);
        free(back);
        free(data);
    }
    expect_consistent("sizes after remount");

    // Delete everything: every block must come back
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(name, sizeof(name), "file_%u", sizes[i]);
        CHECK(remove_entry(ROOT, name) == 0, "delete %s", name);
    }
    CHECK(free_blocks() == used_before, "all blocks freed (%u vs %u)", free_blocks(), used_before);
    CHECK(free_inodes() == 511, "all inodes freed");
    expect_consistent("sizes deleted");
}

static void test_boundary_bytes(void) {
    printf("[crud] boundary bytes and block order\n");
    fresh_fs();
    uint8_t data[2048];
    for (uint32_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t) (i / 512 * 16 + i % 7); // each block has a distinct pattern
    data[0] = 0xA1; data[511] = 0xB2; data[512] = 0xC3; data[1023] = 0xD4;
    data[1024] = 0xE5; data[1535] = 0xF6; data[1536] = 0x17; data[2047] = 0x28;
    CHECK(write_file(ROOT, "boundary", data, sizeof(data)) == 0, "write");
    uint8_t back[2048];
    CHECK(read_file(ROOT, "boundary", back, sizeof(back)) == 0, "read");
    CHECK(memcmp(back, data, sizeof(data)) == 0, "content");
    CHECK(back[0] == 0xA1 && back[511] == 0xB2 && back[512] == 0xC3 && back[2047] == 0x28, "boundaries");

    // Blocks are laid out in order with first fit right after the root directory block
    struct EXT2Inode node;
    ext2_get_inode(lookup(ROOT, "boundary"), &node);
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(node.i_block[i] == 22 + i, "block %u at %u", i, node.i_block[i]);
        CHECK(memcmp(ramdisk + node.i_block[i] * BLOCK_SIZE, data + i * 512, 512) == 0, "raw block %u", i);
    }
}

static void test_error_codes(void) {
    printf("[crud] error codes from the book's tables\n");
    fresh_fs();
    uint8_t buf[4096] = {0};
    CHECK(make_dir(ROOT, "docs") == 0, "mkdir docs");
    CHECK(write_file(ROOT, "note", "hello", 5) == 0, "write note");
    uint32_t docs = lookup(ROOT, "docs"), note = lookup(ROOT, "note");
    CHECK(docs != 0 && note != 0 && docs != note, "lookups");

    // write(): 0 ok, 1 exists, 2 invalid parent, -1 other
    CHECK(write_file(ROOT, "note", "x", 1) == 1, "file exists");
    CHECK(make_dir(ROOT, "note") == 1, "folder with a file's name");
    CHECK(write_file(ROOT, "docs", "x", 1) == 1, "file with a folder's name");
    CHECK(write_file(note, "child", "x", 1) == 2, "parent is a file");
    CHECK(write_file(400, "child", "x", 1) == 2, "parent inode free");
    CHECK(write_file(0, "child", "x", 1) == 2, "parent inode 0");
    CHECK(write_file(9999, "child", "x", 1) == 2, "parent out of range");
    CHECK(write_file(ROOT, ".", "x", 1) == -1 && write_file(ROOT, "..", "x", 1) == -1, "dot names");
    CHECK(write_file(ROOT, "a/b", "x", 1) == -1, "slash in name");
    struct EXT2DriverRequest empty = {.name = "", .name_len = 0, .parent_inode = ROOT};
    CHECK(write(&empty) == -1, "empty name");
    CHECK(write(NULL) == -1, "null request");

    // read(): 0 ok, 1 not a file, 2 buffer, 3 not found, 4 invalid parent
    CHECK(read_file(ROOT, "note", buf, 5) == 0 && memcmp(buf, "hello", 5) == 0, "read ok");
    CHECK(read_file(ROOT, "docs", buf, sizeof(buf)) == 1, "read folder");
    CHECK(read_file(ROOT, "note", buf, 4) == 2, "small buffer");
    CHECK(read_file(ROOT, "missing", buf, sizeof(buf)) == 3, "not found");
    CHECK(read_file(note, "x", buf, sizeof(buf)) == 4, "parent is a file");
    CHECK(read_file(77, "x", buf, sizeof(buf)) == 4, "parent free");

    // read_directory(): 0 ok, 1 not a folder, 2 not found, 3 invalid parent, -1 small buffer
    struct EXT2DriverRequest rd = {.buf = buf, .name = "docs", .name_len = 4, .parent_inode = ROOT, .buffer_size = sizeof(buf)};
    CHECK(read_directory(&rd) == 0, "read_directory ok");
    struct EXT2DirectoryEntry *e = get_directory_entry(buf, 0);
    CHECK(e->inode == docs && get_next_directory_entry(e)->inode == ROOT, "docs . and ..");
    CHECK(get_directory_entry(buf, BLOCK_SIZE)->rec_len == 0, "rest of buffer zeroed");
    rd.name = "note"; rd.name_len = 4;
    CHECK(read_directory(&rd) == 1, "read_directory on file");
    rd.name = "nope"; rd.name_len = 4;
    CHECK(read_directory(&rd) == 2, "read_directory missing");
    rd.parent_inode = note; rd.name = "x"; rd.name_len = 1;
    CHECK(read_directory(&rd) == 3, "read_directory invalid parent");
    rd.parent_inode = ROOT; rd.name = "docs"; rd.name_len = 4; rd.buffer_size = 100;
    CHECK(read_directory(&rd) == -1 && rd.buffer_size == BLOCK_SIZE, "too small buffer reports needed size");
    rd.buffer_size = sizeof(buf); rd.name = "."; rd.name_len = 1;
    CHECK(read_directory(&rd) == 0 && get_directory_entry(buf, 0)->inode == ROOT, "read '.' = parent itself");

    // delete(): 0 ok, 1 not found, 2 not empty, 3 invalid parent, -1 other
    CHECK(write_file(docs, "inner", "abc", 3) == 0, "inner file");
    CHECK(remove_entry(ROOT, "docs") == 2, "non-empty folder");
    CHECK(remove_entry(ROOT, "nothing") == 1, "delete missing");
    CHECK(remove_entry(note, "x") == 3, "delete with file as parent");
    CHECK(remove_entry(ROOT, ".") == -1 && remove_entry(ROOT, "..") == -1, "delete dots");
    CHECK(remove_entry(docs, "inner") == 0, "delete inner");
    CHECK(is_directory_empty(docs), "docs empty again");
    CHECK(remove_entry(ROOT, "docs") == 0, "delete empty folder");
    CHECK(remove_entry(ROOT, "docs") == 1, "deleted twice");
    CHECK(read_file(ROOT, "note", buf, 5) == 0, "unrelated file untouched");
    expect_consistent("error codes");
}

static void test_directories(void) {
    printf("[dir] nesting, listing, links\n");
    fresh_fs();
    CHECK(make_dir(ROOT, "a") == 0, "a");
    uint32_t a = lookup(ROOT, "a");
    CHECK(make_dir(a, "b") == 0, "a/b");
    uint32_t b = lookup(a, "b");
    CHECK(make_dir(b, "c") == 0, "a/b/c");
    uint32_t c = lookup(b, "c");
    CHECK(write_file(c, "deep.txt", "deep", 4) == 0, "a/b/c/deep.txt");
    char buf[8] = {0};
    CHECK(read_file(c, "deep.txt", buf, 4) == 0 && memcmp(buf, "deep", 4) == 0, "deep read");
    CHECK(lookup(c, "..") == b && lookup(b, "..") == a && lookup(a, "..") == ROOT && lookup(ROOT, "..") == ROOT, "parents");

    struct EXT2Inode node;
    ext2_get_inode(ROOT, &node);
    CHECK(node.i_links_count == 3, "root links %u", node.i_links_count);
    ext2_get_inode(a, &node);
    CHECK(node.i_links_count == 3, "a links %u", node.i_links_count);

    char names[16][EXT2_NAME_MAX + 1];
    int n = list_dir(ROOT, "a", names, NULL, 16);
    CHECK(n == 3 && strcmp(names[0], ".") == 0 && strcmp(names[1], "..") == 0 && strcmp(names[2], "b") == 0, "list a (%d)", n);
    expect_consistent("dirs");

    CHECK(remove_entry(b, "c") == 2, "c not empty");
    CHECK(remove_entry(c, "deep.txt") == 0 && remove_entry(b, "c") == 0, "remove c");
    ext2_get_inode(b, &node);
    CHECK(node.i_links_count == 2, "b links back to 2");
    expect_consistent("dirs after delete");
}

static void test_first_fit_fragmentation(void) {
    printf("[alloc] first fit fills holes in order\n");
    fresh_fs();
    char name[16];
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof(name), "s%02d", i);
        uint8_t *d = make_data(512, i);
        CHECK(write_file(ROOT, name, d, 512) == 0, "write %s", name);
        free(d);
    }
    for (int i = 0; i < 20; i += 2) {
        snprintf(name, sizeof(name), "s%02d", i);
        CHECK(remove_entry(ROOT, name) == 0, "delete %s", name);
    }
    // 10 one-block holes at 22, 24, ..., 40; a 15-block file uses all holes then continues after
    uint8_t *big = make_data(15 * 512, 99);
    CHECK(write_file(ROOT, "frag", big, 15 * 512) == 0, "write frag");
    struct EXT2Inode node;
    ext2_get_inode(lookup(ROOT, "frag"), &node);
    for (uint32_t i = 0; i < 10; i++)
        CHECK(node.i_block[i] == 22 + 2 * i, "block %u = %u (hole)", i, node.i_block[i]);
    CHECK(node.i_block[10] == 42 && node.i_block[11] == 43, "after holes: %u %u", node.i_block[10], node.i_block[11]);
    uint8_t *back = malloc(15 * 512);
    CHECK(read_file(ROOT, "frag", back, 15 * 512) == 0 && memcmp(back, big, 15 * 512) == 0, "fragmented content");
    free(back);
    free(big);
    expect_consistent("fragmentation");
}

static void test_exhaustion(void) {
    printf("[alloc] disk full and inode exhaustion are atomic\n");
    fresh_fs();
    uint32_t initial = free_blocks();
    uint8_t *chunk = make_data(300 * 1024, 5);
    int written = 0;
    char name[16];
    for (;;) {
        snprintf(name, sizeof(name), "big%02d", written);
        uint32_t before_b = free_blocks(), before_i = free_inodes();
        int8_t ret = write_file(ROOT, name, chunk, 300 * 1024);
        if (ret != 0) {
            CHECK(ret == -1, "disk full -> -1 (%d)", ret);
            CHECK(free_blocks() == before_b && free_inodes() == before_i, "failed write must not leak");
            CHECK(lookup(ROOT, name) == 0, "failed write leaves no entry");
            break;
        }
        written++;
    }
    CHECK(written == 13, "13 x 300 KiB fit in 4 MiB (%d)", written);
    expect_consistent("full");
    // Small files still fit in the remaining space
    CHECK(write_file(ROOT, "small", chunk, 1000) == 0, "small after full");
    for (int i = 0; i < written; i++) {
        snprintf(name, sizeof(name), "big%02d", i);
        CHECK(remove_entry(ROOT, name) == 0, "delete %s", name);
    }
    CHECK(remove_entry(ROOT, "small") == 0, "delete small");
    CHECK(free_blocks() == initial, "space recovered");
    free(chunk);

    // Inodes: 511 free after format
    int files = 0;
    for (;; files++) {
        snprintf(name, sizeof(name), "i%03d", files);
        int8_t ret = write_file(ROOT, name, "z", 1);
        if (ret != 0) {
            CHECK(ret == -1, "no inode -> -1");
            break;
        }
    }
    CHECK(files == 511, "511 files until inodes run out (%d)", files);
    expect_consistent("inodes full");
    char names[600][EXT2_NAME_MAX + 1];
    CHECK(list_dir(ROOT, ".", names, NULL, 600) == 513, "root lists . .. and 511 files");
    for (int i = 0; i < files; i++) {
        snprintf(name, sizeof(name), "i%03d", i);
        CHECK(remove_entry(ROOT, name) == 0, "delete %s", name);
    }
    // Like real ext2, a directory never shrinks: root keeps the blocks it grew to
    struct EXT2Inode root;
    ext2_get_inode(ROOT, &root);
    CHECK(free_inodes() == 511, "inodes back");
    CHECK(free_blocks() == initial - (root.i_blocks - 1), "only root's growth stays allocated (%u vs %u)", free_blocks(), initial - (root.i_blocks - 1));
    expect_consistent("inodes freed");
}

static void test_large_directory(void) {
    printf("[dir] directory spanning indirect blocks\n");
    fresh_fs();
    CHECK(make_dir(ROOT, "many") == 0, "mkdir");
    uint32_t many = lookup(ROOT, "many");
    char name[128];
    // 60-byte names -> 68-byte records -> 7 per block -> 420 entries need 61 blocks (indirect)
    for (int i = 0; i < 420; i++) {
        snprintf(name, sizeof(name), "entry_%04d_padding_padding_padding_padding_padding_padding__", i);
        CHECK(write_file(many, name, name, 16) == 0, "write %d", i);
    }
    struct EXT2Inode node;
    ext2_get_inode(many, &node);
    CHECK(node.i_size / BLOCK_SIZE > 12 && node.i_block[EXT2_IND_BLOCK] != 0, "directory uses indirect block (%u blocks)", node.i_size / BLOCK_SIZE);
    expect_consistent("large dir");
    initialize_filesystem_ext2();
    char buf[16];
    for (int i = 0; i < 420; i += 37) {
        snprintf(name, sizeof(name), "entry_%04d_padding_padding_padding_padding_padding_padding__", i);
        CHECK(read_file(many, name, buf, 16) == 0 && memcmp(buf, name, 16) == 0, "read %d", i);
    }
    for (int i = 0; i < 420; i++) {
        snprintf(name, sizeof(name), "entry_%04d_padding_padding_padding_padding_padding_padding__", i);
        CHECK(remove_entry(many, name) == 0, "delete %d", i);
    }
    CHECK(is_directory_empty(many), "empty after deleting all");
    CHECK(remove_entry(ROOT, "many") == 0, "rmdir");
    CHECK(free_inodes() == 511, "inodes");
    expect_consistent("large dir removed");
}

static void test_rename(void) {
    printf("[ext] rename / move\n");
    fresh_fs();
    make_dir(ROOT, "src");
    make_dir(ROOT, "dst");
    uint32_t src = lookup(ROOT, "src"), dst = lookup(ROOT, "dst");
    write_file(src, "f", "payload", 7);
    make_dir(src, "sub");
    uint32_t sub = lookup(src, "sub");
    write_file(sub, "g", "xyz", 3);

    CHECK(ext2_rename(src, "f", 1, src, "renamed", 7) == 0, "rename in place");
    char buf[16] = {0};
    CHECK(read_file(src, "renamed", buf, 7) == 0 && memcmp(buf, "payload", 7) == 0 && lookup(src, "f") == 0, "renamed");
    CHECK(ext2_rename(src, "renamed", 7, dst, "moved", 5) == 0, "move file");
    CHECK(read_file(dst, "moved", buf, 7) == 0 && lookup(src, "renamed") == 0, "moved file");
    CHECK(ext2_rename(ROOT, "src", 3, sub, "loop", 4) == 4, "folder into its own subfolder");
    CHECK(ext2_rename(ROOT, "src", 3, src, "self", 4) == 4, "folder into itself");
    CHECK(ext2_rename(src, "sub", 3, ROOT, "dst", 3) == 2, "destination exists");
    CHECK(ext2_rename(src, "nope", 4, ROOT, "x", 1) == 1, "source missing");
    CHECK(ext2_rename(dst, "moved", 5, 400, "x", 1) == 3, "invalid destination");
    CHECK(ext2_rename(ROOT, "..", 2, dst, "x", 1) == -1, "cannot move ..");
    CHECK(ext2_rename(src, "sub", 3, dst, "sub2", 4) == 0, "move folder");
    CHECK(lookup(dst, "sub2") == sub && lookup(sub, "..") == dst, "'..' rewritten");
    CHECK(read_file(sub, "g", buf, 3) == 0 && memcmp(buf, "xyz", 3) == 0, "contents follow");
    struct EXT2Inode node;
    ext2_get_inode(src, &node);
    CHECK(node.i_links_count == 2, "src lost a subdir");
    ext2_get_inode(dst, &node);
    CHECK(node.i_links_count == 3, "dst gained a subdir");
    expect_consistent("rename");
}

static void test_find_index(void) {
    printf("[index] find by name without walking the tree\n");
    fresh_fs();
    make_dir(ROOT, "x");
    make_dir(ROOT, "y");
    uint32_t x = lookup(ROOT, "x"), y = lookup(ROOT, "y");
    make_dir(y, "z");
    uint32_t z = lookup(y, "z");
    write_file(ROOT, "target", "1", 1);
    write_file(x, "target", "2", 1);
    write_file(z, "target", "3", 1);
    make_dir(y, "target");

    struct EXT2FindResult results[8];
    uint32_t n = ext2_find("target", 6, results, 8);
    CHECK(n == 4, "4 matches (%u)", n);
    uint32_t parents = 0;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        if (results[i].parent_inode == ROOT) parents |= 1;
        if (results[i].parent_inode == x) parents |= 2;
        if (results[i].parent_inode == z) parents |= 4;
        if (results[i].parent_inode == y && results[i].file_type == EXT2_FT_DIR) parents |= 8;
    }
    CHECK(parents == 15, "every location reported (%x)", parents);
    CHECK(ext2_find("targe", 5, results, 8) == 0 && ext2_find("nothing", 7, results, 8) == 0, "no false positives");

    remove_entry(x, "target");
    CHECK(ext2_find("target", 6, results, 8) == 3, "delete updates index");
    ext2_rename(z, "target", 6, x, "other", 5);
    CHECK(ext2_find("target", 6, results, 8) == 2 && ext2_find("other", 5, results, 8) == 1, "rename updates index");

    initialize_filesystem_ext2(); // index is rebuilt from disk on mount
    CHECK(ext2_find("target", 6, results, 8) == 2 && ext2_find("other", 5, results, 8) == 1, "index rebuilt on mount");

    char name[EXT2_NAME_MAX + 1];
    uint32_t parent = 0;
    CHECK(ext2_get_name(z, &parent, name) && parent == y && strcmp(name, "z") == 0, "get_name");
    CHECK(!ext2_get_name(ROOT, &parent, name), "root has no name");
}

static void test_sample_image(void) {
    printf("[compat] reading the reference sample-image.bin\n");
    if (!ramdisk_load(".kit/ch2/sample-image.bin")) {
        printf("  (skipped: .kit/ch2/sample-image.bin not found)\n");
        return;
    }
    uint8_t laur_raw[44], sasakure_raw[117];
    memcpy(laur_raw, ramdisk + 27 * BLOCK_SIZE, sizeof(laur_raw));
    memcpy(sasakure_raw, ramdisk + 28 * BLOCK_SIZE, sizeof(sasakure_raw));
    // Book 2.6.4: use our own fs_signature so the driver mounts instead of formatting
    memcpy(ramdisk, fs_signature, BLOCK_SIZE);
    initialize_filesystem_ext2();

    char names[16][EXT2_NAME_MAX + 1];
    uint32_t inodes[16];
    int n = list_dir(ROOT, ".", names, inodes, 16);
    CHECK(n == 6, "root of sample has 6 entries (%d)", n);
    bool saw_laur = false, saw_folder2 = false;
    for (int i = 0; i < n; i++) {
        saw_laur    |= strcmp(names[i], "laur") == 0 && inodes[i] == 7;
        saw_folder2 |= strncmp(names[i], "folder2", 7) == 0 && inodes[i] == 6;
    }
    CHECK(saw_laur && saw_folder2, "laur / folder2 listed");

    uint8_t buf[512];
    CHECK(read_file(ROOT, "laur", buf, sizeof(buf)) == 0 && memcmp(buf, laur_raw, 44) == 0, "read laur");
    CHECK(read_file(ROOT, "sasakure", buf, sizeof(buf)) == 0 && memcmp(buf, sasakure_raw, 117) == 0, "read sasakure");
    // folder2 is stored with name_len 8 (counts the NUL), the driver trims the padding
    uint32_t folder2 = lookup(ROOT, "folder2");
    CHECK(folder2 == 6, "lookup folder2 (%u)", folder2);
    CHECK(lookup(2, "nestedf1") == 3 && lookup(2, "tpazolite") == 4 && lookup(2, "xi") == 5, "folder1 children");
    CHECK(read_file(2, "xi", buf, sizeof(buf)) == 0, "read xi");

    // Writing into the reference image must keep its existing files intact
    uint8_t *data = make_data(3000, 77);
    CHECK(write_file(folder2, "added", data, 3000) == 0, "write into sample");
    uint8_t back[3000];
    CHECK(read_file(folder2, "added", back, 3000) == 0 && memcmp(back, data, 3000) == 0, "read back");
    CHECK(read_file(ROOT, "laur", buf, sizeof(buf)) == 0 && memcmp(buf, laur_raw, 44) == 0, "laur intact");
    free(data);
    struct EXT2FindResult results[4];
    CHECK(ext2_find("added", 5, results, 4) == 1 && results[0].parent_inode == folder2, "index on sample image");
}

/* --------------------------- randomized model check ------------------------------- */

#define MODEL_MAX 600

struct ModelEntry {
    bool     used;
    bool     is_dir;
    uint32_t parent;
    uint32_t inode;
    char     name[64];
    uint8_t *data;
    uint32_t size;
};

static struct ModelEntry model[MODEL_MAX];

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

static uint32_t model_random_dir(void) {
    uint32_t dirs[MODEL_MAX + 1];
    int n = 0;
    dirs[n++] = ROOT;
    for (int i = 0; i < MODEL_MAX; i++)
        if (model[i].used && model[i].is_dir)
            dirs[n++] = model[i].inode;
    return dirs[rng() % n];
}

static int model_random_entry(void) {
    int idx[MODEL_MAX], n = 0;
    for (int i = 0; i < MODEL_MAX; i++)
        if (model[i].used)
            idx[n++] = i;
    return n ? idx[rng() % n] : -1;
}

static bool model_is_ancestor(uint32_t maybe_ancestor, uint32_t dir) {
    for (int depth = 0; depth < 600 && dir != ROOT; depth++) {
        if (dir == maybe_ancestor)
            return true;
        int found = -1;
        for (int i = 0; i < MODEL_MAX; i++)
            if (model[i].used && model[i].inode == dir)
                found = i;
        if (found < 0)
            return false;
        dir = model[found].parent;
    }
    return dir == maybe_ancestor;
}

static void model_verify_all(const char *where) {
    for (int i = 0; i < MODEL_MAX; i++) {
        if (!model[i].used)
            continue;
        CHECK(lookup(model[i].parent, model[i].name) == model[i].inode, "%s: lookup %s", where, model[i].name);
        if (!model[i].is_dir) {
            uint8_t *back = malloc(model[i].size + 1);
            int8_t ret = read_file(model[i].parent, model[i].name, back, model[i].size);
            CHECK(ret == 0 && memcmp(back, model[i].data, model[i].size) == 0, "%s: content of %s (%d)", where, model[i].name, ret);
            free(back);
        } else {
            static char names[MODEL_MAX + 2][EXT2_NAME_MAX + 1];
            int n = list_dir(model[i].parent, model[i].name, names, NULL, MODEL_MAX + 2);
            CHECK(n == 2 + model_children(model[i].inode), "%s: %s lists %d entries, model %d", where, model[i].name, n, 2 + model_children(model[i].inode));
        }
    }
    expect_consistent(where);
}

static void test_random_model(void) {
    printf("[model] 4000 random operations checked against an in-memory model\n");
    fresh_fs();
    memset(model, 0, sizeof(model));
    static const char *names[] = {"a", "b", "c", "d", "data.bin", "notes.txt", "dir", "x", "y",
                                  "a_rather_long_file_name_that_needs_more_space_in_the_record"};
    for (int step = 1; step <= 4000; step++) {
        uint32_t op = rng() % 100;
        uint32_t parent = model_random_dir();
        const char *name = names[rng() % (sizeof(names) / sizeof(names[0]))];
        int existing = model_find(parent, name);

        if (op < 40) { // create file
            uint32_t size = (rng() % 4 == 0) ? rng() % 90000 : rng() % 3000;
            uint8_t *data = make_data(size, step);
            uint32_t fb = free_blocks(), fi = free_inodes();
            int8_t ret = write_file(parent, name, data, size);
            if (existing >= 0) {
                CHECK(ret == 1, "step %d: duplicate create -> %d", step, ret);
                free(data);
            } else if (ret == -1) {
                CHECK(free_blocks() == fb && free_inodes() == fi, "step %d: failed create leaked", step);
                CHECK(fi == 0 || fb < (size + 511) / 512 + 8, "step %d: unexpected -1 (free %u)", step, fb);
                free(data);
            } else {
                CHECK(ret == 0, "step %d: create -> %d", step, ret);
                int slot = 0;
                while (slot < MODEL_MAX && model[slot].used) slot++;
                if (slot == MODEL_MAX) { free(data); continue; }
                model[slot] = (struct ModelEntry) {.used = true, .is_dir = false, .parent = parent,
                                                   .inode = lookup(parent, name), .data = data, .size = size};
                strcpy(model[slot].name, name);
            }
        } else if (op < 55) { // mkdir
            int8_t ret = make_dir(parent, name);
            if (existing >= 0) {
                CHECK(ret == 1, "step %d: duplicate mkdir -> %d", step, ret);
            } else if (ret == 0) {
                int slot = 0;
                while (slot < MODEL_MAX && model[slot].used) slot++;
                if (slot == MODEL_MAX) continue;
                model[slot] = (struct ModelEntry) {.used = true, .is_dir = true, .parent = parent, .inode = lookup(parent, name)};
                strcpy(model[slot].name, name);
            } else {
                CHECK(ret == -1 && (free_inodes() == 0 || free_blocks() < 4), "step %d: mkdir -> %d", step, ret);
            }
        } else if (op < 80) { // delete
            int victim = (rng() % 4 == 0) ? existing : model_random_entry();
            if (victim < 0) {
                CHECK(remove_entry(parent, name) == 1, "step %d: delete missing", step);
                continue;
            }
            int8_t ret = remove_entry(model[victim].parent, model[victim].name);
            if (model[victim].is_dir && model_children(model[victim].inode) > 0) {
                CHECK(ret == 2, "step %d: delete non-empty -> %d", step, ret);
            } else {
                CHECK(ret == 0, "step %d: delete -> %d", step, ret);
                free(model[victim].data);
                model[victim].used = false;
                model[victim].data = NULL;
            }
        } else if (op < 92) { // read
            int target = model_random_entry();
            if (target < 0) continue;
            uint8_t *back = malloc(model[target].size + 1);
            int8_t ret = read_file(model[target].parent, model[target].name, back, model[target].size);
            if (model[target].is_dir)
                CHECK(ret == 1, "step %d: read dir -> %d", step, ret);
            else
                CHECK(ret == 0 && memcmp(back, model[target].data, model[target].size) == 0, "step %d: read -> %d", step, ret);
            free(back);
        } else { // rename / move
            int src = model_random_entry();
            if (src < 0) continue;
            uint32_t dest = model_random_dir();
            int8_t ret = ext2_rename(model[src].parent, model[src].name, (uint8_t) strlen(model[src].name),
                                     dest, name, (uint8_t) strlen(name));
            int clash = model_find(dest, name);
            bool same = clash == src;
            // Precedence documented in ext2.h: same entry (0), name taken (2), cycle (4)
            if (same) {
                CHECK(ret == 0, "step %d: rename to itself -> %d", step, ret);
            } else if (clash >= 0) {
                CHECK(ret == 2, "step %d: rename onto existing -> %d", step, ret);
            } else if (model[src].is_dir && model_is_ancestor(model[src].inode, dest)) {
                CHECK(ret == 4, "step %d: move into own subtree -> %d", step, ret);
            } else if (ret == 0) {
                model[src].parent = dest;
                strcpy(model[src].name, name);
            } else {
                CHECK(ret == -1 && free_blocks() < 4, "step %d: rename -> %d", step, ret);
            }
        }

        if (step % 100 == 0)
            expect_consistent("random ops");
        if (step % 500 == 0) {
            initialize_filesystem_ext2();
            model_verify_all("random remount");
        }
    }
    for (int i = 0; i < MODEL_MAX; i++)
        free(model[i].data);
}

int main(void) {
    test_format_layout();
    test_file_sizes();
    test_boundary_bytes();
    test_error_codes();
    test_directories();
    test_first_fit_fragmentation();
    test_exhaustion();
    test_large_directory();
    test_rename();
    test_find_index();
    test_sample_image();
    test_random_model();
    printf("ext2: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
