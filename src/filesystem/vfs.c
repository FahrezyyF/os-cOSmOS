#include "header/filesystem/vfs.h"
#include "header/filesystem/ext2.h"
#include "header/filesystem/cosmofs.h"
#include "header/driver/disk.h"
#include "header/driver/rtc.h"
#include "header/stdlib/string.h"
#include "header/stdlib/format.h"
#include <stdarg.h>

static bool           mounted[VFS_COUNT];
static struct CosmoFS cosmo;
static uint8_t        dir_buffer[128 * BLOCK_SIZE]; // EXT2 read_directory() scratch (64 KiB)

/* -- ATA slave as a CosmoFS block device -- */

static bool slave_read(void *ctx, uint32_t lba, uint32_t count, void *buf) {
    (void) ctx;
    uint8_t *p = (uint8_t*) buf;
    while (count > 0) {
        uint8_t chunk = count > 128 ? 128 : (uint8_t) count;
        if (!ata_read(ATA_DRIVE_SLAVE, p, lba, chunk))
            return false;
        p += chunk * BLOCK_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

static bool slave_write(void *ctx, uint32_t lba, uint32_t count, const void *buf) {
    (void) ctx;
    const uint8_t *p = (const uint8_t*) buf;
    while (count > 0) {
        uint8_t chunk = count > 128 ? 128 : (uint8_t) count;
        if (!ata_write(ATA_DRIVE_SLAVE, p, lba, chunk))
            return false;
        p += chunk * BLOCK_SIZE;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

static bool slave_flush(void *ctx) {
    (void) ctx;
    return ata_flush(ATA_DRIVE_SLAVE);
}

static void log_line(void (*log)(const char*), const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_line(void (*log)(const char*), const char *fmt, ...) {
    if (log == NULL)
        return;
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log(line);
}

void vfs_mount_all(void (*log)(const char *line)) {
    struct ATADriveInfo master, slave;
    ata_identify(ATA_DRIVE_MASTER, &master);
    ata_identify(ATA_DRIVE_SLAVE, &slave);

    if (master.present) {
        ext2_set_time_source(rtc_unix_time);
        bool fresh = is_empty_storage();
        initialize_filesystem_ext2();
        ata_flush(ATA_DRIVE_MASTER);
        mounted[VFS_EXT2] = true;
        log_line(log, "storage.bin: %u KiB, EXT2 %s", master.sector_count / 2,
                 fresh ? "(blank disk, formatted)" : "(mounted existing volume)");
    } else {
        log_line(log, "storage.bin: not attached - EXT2 unavailable");
    }

    if (slave.present) {
        struct CosmoFSDevice dev = {
            .context = NULL, .block_count = slave.sector_count,
            .read = slave_read, .write = slave_write, .flush = slave_flush,
        };
        int8_t ret = cosmofs_mount(&cosmo, &dev, rtc_unix_time);
        const char *how = "mounted existing volume";
        if (ret == 1) {
            how = "blank disk, formatted";
            if (cosmofs_format(&dev, "cosmos", rtc_unix_time))
                ret = cosmofs_mount(&cosmo, &dev, rtc_unix_time);
        }
        mounted[VFS_COSMOFS] = ret == 0;
        if (ret == 0 && cosmo.replayed_transactions > 0)
            how = "mounted, journal replayed";
        log_line(log, "cosmo.bin  : %u KiB, CosmoFS (%s)", slave.sector_count / 2,
                 ret == 0 ? how : "mount FAILED, corrupted volume");
    } else {
        log_line(log, "cosmo.bin  : not attached - CosmoFS unavailable");
    }
}

bool vfs_available(enum VFSKind kind) {
    return kind < VFS_COUNT && mounted[kind];
}

const char *vfs_name(enum VFSKind kind) {
    return kind == VFS_EXT2 ? "ext2" : "cosmo";
}

uint32_t vfs_root(enum VFSKind kind) {
    return kind == VFS_EXT2 ? EXT2_ROOT_INODE : COSMOFS_ROOT_NODE;
}

static void copy_name(struct VFSEntry *e, const char *name, uint8_t len) {
    memcpy(e->name, name, len);
    e->name[len] = '\0';
    e->name_len  = len;
}

int8_t vfs_lookup(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len, struct VFSEntry *out) {
    if (!vfs_available(kind))
        return 2;
    if (kind == VFS_EXT2) {
        uint32_t inode;
        uint8_t  type;
        int8_t ret = ext2_lookup(dir, name, len, &inode, &type);
        if (ret != 0)
            return ret;
        struct EXT2Inode node;
        ext2_get_inode(inode, &node);
        out->id   = inode;
        out->size = node.i_size;
        out->type = type == EXT2_FT_DIR ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        copy_name(out, name, len);
        return 0;
    }
    struct CosmoFSStat st;
    int8_t ret = cosmofs_lookup(&cosmo, dir, name, len, &st);
    if (ret != 0)
        return ret;
    out->id   = st.node;
    out->size = st.size;
    out->type = st.type == COSMOFS_TYPE_DIR ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    copy_name(out, name, len);
    return 0;
}

bool vfs_stat(enum VFSKind kind, uint32_t id, struct VFSStat *out) {
    memset(out, 0, sizeof(*out));
    if (!vfs_available(kind))
        return false;
    if (kind == VFS_EXT2) {
        struct EXT2Inode node;
        if (!ext2_get_inode(id, &node))
            return false;
        out->id     = id;
        out->type   = (node.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        out->size   = node.i_size;
        out->blocks = node.i_blocks;
        out->links  = node.i_links_count;
        out->ctime  = node.i_ctime;
        out->mtime  = node.i_mtime;
        char name[EXT2_NAME_MAX + 1];
        ext2_get_name(id, &out->parent, name);
        return true;
    }
    struct CosmoFSStat st;
    if (!cosmofs_stat(&cosmo, id, &st))
        return false;
    out->id      = id;
    out->parent  = st.parent;
    out->type    = st.type == COSMOFS_TYPE_DIR ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    out->size    = st.size;
    out->blocks  = st.block_count;
    out->extents = st.extent_count;
    out->ctime   = st.ctime;
    out->mtime   = st.mtime;
    return true;
}

int32_t vfs_list(enum VFSKind kind, uint32_t dir, struct VFSEntry *out, uint32_t max) {
    if (!vfs_available(kind))
        return -13;
    uint32_t count = 0;
    if (kind == VFS_EXT2) {
        struct EXT2DriverRequest req = {
            .buf = dir_buffer, .name = ".", .name_len = 1, .parent_inode = dir, .buffer_size = sizeof(dir_buffer),
        };
        int8_t ret = read_directory(&req);
        if (ret != 0)
            return ret - 10;
        for (uint32_t block = 0; block < sizeof(dir_buffer) / BLOCK_SIZE; block++) {
            uint8_t *b = dir_buffer + block * BLOCK_SIZE;
            if (get_directory_entry(b, 0)->rec_len == 0)
                break;
            for (uint32_t off = 0; off + 8 <= BLOCK_SIZE;) {
                struct EXT2DirectoryEntry *e = get_directory_entry(b, off);
                if (e->rec_len == 0)
                    break;
                if (e->inode != 0 && count < max) {
                    uint8_t len = e->name_len;
                    while (len > 0 && get_entry_name(e)[len - 1] == '\0')
                        len--;
                    struct EXT2Inode node;
                    ext2_get_inode(e->inode, &node);
                    out[count].id   = e->inode;
                    out[count].size = node.i_size;
                    out[count].type = (node.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                    copy_name(&out[count], get_entry_name(e), len);
                    count++;
                }
                off += e->rec_len;
            }
        }
        return (int32_t) count;
    }

    static struct CosmoFSDirEntry entries[300];
    struct CosmoFSRequest req = {.buf = entries, .name = ".", .name_len = 1, .parent = dir, .buffer_size = sizeof(entries)};
    int8_t ret = cosmofs_read_directory(&cosmo, &req);
    if (ret != 0)
        return ret - 10;
    uint32_t n = req.buffer_size / sizeof(struct CosmoFSDirEntry);
    for (uint32_t i = 0; i < n && count < max; i++, count++) {
        out[count].id   = entries[i].node;
        out[count].size = entries[i].size;
        out[count].type = entries[i].type == COSMOFS_TYPE_DIR ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        copy_name(&out[count], entries[i].name, entries[i].name_len);
    }
    return (int32_t) count;
}

int8_t vfs_read(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len, void *buf, uint32_t buf_size) {
    if (!vfs_available(kind))
        return -1;
    if (kind == VFS_EXT2) {
        struct EXT2DriverRequest req = {
            .buf = buf, .name = (char*) name, .name_len = len, .parent_inode = dir, .buffer_size = buf_size,
        };
        return read(req);
    }
    struct CosmoFSRequest req = {.buf = buf, .name = name, .name_len = len, .parent = dir, .buffer_size = buf_size};
    return cosmofs_read(&cosmo, &req);
}

int8_t vfs_write(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len, const void *buf, uint32_t size, bool is_dir) {
    if (!vfs_available(kind))
        return -1;
    if (kind == VFS_EXT2) {
        struct EXT2DriverRequest req = {
            .buf = (void*) buf, .name = (char*) name, .name_len = len, .parent_inode = dir,
            .buffer_size = size, .is_directory = is_dir,
        };
        int8_t ret = write(&req);
        ata_flush(ATA_DRIVE_MASTER);
        return ret;
    }
    struct CosmoFSRequest req = {.buf = (void*) buf, .name = name, .name_len = len, .parent = dir,
                                 .buffer_size = size, .is_directory = is_dir};
    return cosmofs_write(&cosmo, &req);
}

int8_t vfs_delete(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len) {
    if (!vfs_available(kind))
        return -1;
    if (kind == VFS_EXT2) {
        struct EXT2DriverRequest req = {.name = (char*) name, .name_len = len, .parent_inode = dir};
        int8_t ret = delete(req);
        ata_flush(ATA_DRIVE_MASTER);
        return ret;
    }
    struct CosmoFSRequest req = {.name = name, .name_len = len, .parent = dir};
    return cosmofs_delete(&cosmo, &req);
}

int8_t vfs_rename(enum VFSKind kind, uint32_t dir, const char *name, uint8_t len,
                  uint32_t new_dir, const char *new_name, uint8_t new_len) {
    if (!vfs_available(kind))
        return -1;
    if (kind == VFS_EXT2) {
        int8_t ret = ext2_rename(dir, name, len, new_dir, new_name, new_len);
        ata_flush(ATA_DRIVE_MASTER);
        return ret;
    }
    return cosmofs_rename(&cosmo, dir, name, len, new_dir, new_name, new_len);
}

uint32_t vfs_find(enum VFSKind kind, const char *name, uint8_t len, struct VFSFindHit *hits, uint32_t max) {
    if (!vfs_available(kind))
        return 0;
    if (kind == VFS_EXT2) {
        static struct EXT2FindResult results[64];
        uint32_t total = ext2_find(name, len, results, 64);
        for (uint32_t i = 0; i < total && i < max && i < 64; i++) {
            hits[i].id     = results[i].inode;
            hits[i].parent = results[i].parent_inode;
            hits[i].type   = results[i].file_type == EXT2_FT_DIR ? VFS_TYPE_DIR : VFS_TYPE_FILE;
        }
        return total;
    }
    static struct CosmoFSFindResult results[64];
    uint32_t total = cosmofs_find(&cosmo, name, len, results, 64);
    for (uint32_t i = 0; i < total && i < max && i < 64; i++) {
        hits[i].id     = results[i].node;
        hits[i].parent = results[i].parent;
        hits[i].type   = results[i].type == COSMOFS_TYPE_DIR ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    }
    return total;
}

bool vfs_name_of(enum VFSKind kind, uint32_t id, uint32_t *parent, char *name_out) {
    if (!vfs_available(kind) || id == vfs_root(kind))
        return false;
    if (kind == VFS_EXT2)
        return ext2_get_name(id, parent, name_out);
    struct CosmoFSStat st;
    if (!cosmofs_stat(&cosmo, id, &st))
        return false;
    *parent = st.parent;
    memcpy(name_out, st.name, sizeof(st.name));
    return true;
}

void vfs_stats(enum VFSKind kind, struct VFSStats *out) {
    memset(out, 0, sizeof(*out));
    out->fs_name    = kind == VFS_EXT2 ? "EXT2 (IF2130 Edition)" : "CosmoFS v1 (journaled)";
    out->device     = kind == VFS_EXT2 ? "storage.bin (ATA master)" : "cosmo.bin (ATA slave)";
    out->block_size = BLOCK_SIZE;
    if (!vfs_available(kind))
        return;
    if (kind == VFS_EXT2) {
        struct EXT2Stats st;
        ext2_get_stats(&st);
        out->total_blocks = st.blocks_count;
        out->free_blocks  = st.free_blocks_count;
        out->total_nodes  = st.inodes_count;
        out->free_nodes   = st.free_inodes_count;
        out->mount_count  = st.mount_count;
        return;
    }
    struct CosmoFSStats st;
    cosmofs_get_stats(&cosmo, &st);
    out->total_blocks     = st.total_blocks;
    out->free_blocks      = st.free_blocks;
    out->total_nodes      = st.node_count;
    out->free_nodes       = st.free_nodes;
    out->mount_count      = st.mount_count;
    out->journal_sequence = st.journal_sequence;
    out->replayed         = st.replayed_transactions;
}

bool vfs_check(enum VFSKind kind, uint32_t *errors, char *message, uint32_t message_size) {
    *errors = 0;
    if (!vfs_available(kind)) {
        snprintf(message, message_size, "volume not mounted");
        return false;
    }
    if (kind == VFS_EXT2) {
        struct EXT2CheckReport r;
        bool ok = ext2_check(&r);
        *errors = r.errors;
        if (ok)
            snprintf(message, message_size, "clean: %u files, %u dirs, %u blocks in use", r.files, r.dirs, r.used_blocks);
        else
            snprintf(message, message_size, "%s", r.message);
        return ok;
    }
    struct CosmoFSCheckReport r;
    bool ok = cosmofs_check(&cosmo, &r);
    *errors = r.errors;
    if (ok)
        snprintf(message, message_size, "clean: %u files, %u dirs, %u extents, %u blocks in use",
                 r.files, r.dirs, r.extents, r.used_blocks);
    else
        snprintf(message, message_size, "%s", r.message);
    return ok;
}
