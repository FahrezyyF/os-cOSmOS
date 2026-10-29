#include <stdarg.h>
#include "header/filesystem/vfs.h"
#include "header/filesystem/ext2.h"
#include "header/filesystem/cosmofs.h"
#include "header/driver/disk.h"
#include "header/driver/rtc.h"
#include "header/memory/swap.h"
#include "header/memory/kheap.h"
#include "header/stdlib/string.h"
#include "header/stdlib/format.h"

static bool           mounted = false;
static enum VFSKind   root_kind = VFS_EXT2;
static uint8_t        root_drive = ATA_DRIVE_MASTER;
static struct CosmoFS cosmo;

/* -- ATA slave (cosmo.bin) as a CosmoFS block device -- */

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

bool vfs_mount_root(enum VFSKind kind, void (*log)(const char *line)) {
    root_kind  = kind;
    root_drive = kind == VFS_EXT2 ? ATA_DRIVE_MASTER : ATA_DRIVE_SLAVE;
    const char *image = kind == VFS_EXT2 ? "storage.bin" : "cosmo.bin";
    struct ATADriveInfo info;
    ata_identify(root_drive, &info);
    if (!info.present) {
        log_line(log, "%s is not attached: no root file system", image);
        mounted = false;
        return false;
    }

    if (kind == VFS_EXT2) {
        ext2_set_time_source(rtc_unix_time);
        bool fresh = is_empty_storage();
        initialize_filesystem_ext2();
        ata_flush(ATA_DRIVE_MASTER);
        mounted = true;
        log_line(log, "root: EXT2 on %s (%s)", image, fresh ? "blank disk, formatted" : "mounted");
    } else {
        struct CosmoFSDevice dev = {
            .context     = NULL,
            .block_count = info.sector_count < VFS_FS_REGION_BLOCKS ? info.sector_count : VFS_FS_REGION_BLOCKS,
            .read        = slave_read,
            .write       = slave_write,
            .flush       = slave_flush,
        };
        int8_t ret = cosmofs_mount(&cosmo, &dev, rtc_unix_time);
        const char *how = cosmo.replayed_transactions > 0 ? "mounted, journal replayed" : "mounted";
        if (ret == 1) {
            how = "blank disk, formatted";
            if (cosmofs_format(&dev, "cosmos", rtc_unix_time))
                ret = cosmofs_mount(&cosmo, &dev, rtc_unix_time);
        }
        mounted = ret == 0;
        log_line(log, "root: CosmoFS on %s (%s)", image, mounted ? how : "mount FAILED, corrupted volume");
    }

    // Swap partition: everything after the 4 MiB file system region
    if (info.sector_count >= VFS_FS_REGION_BLOCKS + SWAP_SECTORS_PER_PAGE) {
        uint32_t slots = (info.sector_count - VFS_FS_REGION_BLOCKS) / SWAP_SECTORS_PER_PAGE;
        swap_init(root_drive, VFS_FS_REGION_BLOCKS, slots);
        log_line(log, "swap: %u KiB partition on %s", (slots > PAGE_FRAME_ON_SWAP_MAX_COUNT ? PAGE_FRAME_ON_SWAP_MAX_COUNT : slots) * 4, image);
    } else {
        swap_init(root_drive, 0, 0);
        log_line(log, "swap: disabled (%s has no swap partition, run `make disk`)", image);
    }
    return mounted;
}

bool vfs_ready(void) {
    return mounted;
}

enum VFSKind vfs_kind(void) {
    return root_kind;
}

const char *vfs_name(void) {
    return root_kind == VFS_EXT2 ? "ext2" : "cosmo";
}

uint32_t vfs_root(void) {
    return root_kind == VFS_EXT2 ? EXT2_ROOT_INODE : COSMOFS_ROOT_NODE;
}

uint8_t vfs_drive(void) {
    return root_drive;
}

/* ================================ CRUD ============================================= */

static struct CosmoFSRequest to_cosmo(const struct EXT2DriverRequest *r) {
    struct CosmoFSRequest c = {
        .buf          = r->buf,
        .name         = r->name,
        .name_len     = r->name_len,
        .parent       = r->parent_inode,
        .buffer_size  = r->buffer_size,
        .is_directory = r->is_directory,
    };
    return c;
}

int8_t vfs_read(struct EXT2DriverRequest request) {
    if (!mounted)
        return -1;
    if (root_kind == VFS_EXT2)
        return read(request);
    struct CosmoFSRequest c = to_cosmo(&request);
    return cosmofs_read(&cosmo, &c);
}

/**
 * CosmoFS listing -> EXT2 directory blocks, so read_directory() has one format.
 * Same contract as the EXT2 driver: buffer too small -> -1 and buffer_size = bytes needed.
 */
static int8_t cosmo_read_directory(struct EXT2DriverRequest *request) {
    struct CosmoFSRequest probe = to_cosmo(request);
    probe.buf = NULL;
    probe.buffer_size = 0;
    int8_t ret = cosmofs_read_directory(&cosmo, &probe);
    if (ret != -1)
        return ret; // 1 not a folder, 2 not found, 3 invalid parent
    uint32_t bytes = probe.buffer_size;
    struct CosmoFSDirEntry *entries = (struct CosmoFSDirEntry*) kmalloc(bytes);
    if (entries == NULL)
        return -1;
    struct CosmoFSRequest list = to_cosmo(request);
    list.buf = entries;
    list.buffer_size = bytes;
    if (cosmofs_read_directory(&cosmo, &list) != 0) {
        kfree(entries);
        return -1;
    }
    uint32_t count = list.buffer_size / sizeof(struct CosmoFSDirEntry);

    // Pass 1: size of the packed table
    uint32_t blocks = 1, offset = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t rec = get_entry_record_len(entries[i].name_len);
        if (offset + rec > BLOCK_SIZE) {
            blocks++;
            offset = 0;
        }
        offset += rec;
    }
    if (request->buf == NULL || request->buffer_size < blocks * BLOCK_SIZE) {
        request->buffer_size = blocks * BLOCK_SIZE;
        kfree(entries);
        return -1;
    }

    // Pass 2: build each block in kernel memory, then copy it out
    struct BlockBuffer block;
    uint8_t *out = (uint8_t*) request->buf;
    uint32_t block_index = 0, last = 0;
    offset = 0;
    memset(&block, 0, sizeof(block));
    for (uint32_t i = 0; i <= count; i++) {
        uint16_t rec = i < count ? get_entry_record_len(entries[i].name_len) : 0;
        if (i == count || offset + rec > BLOCK_SIZE) {
            // close the block: the last record extends to the end of the block
            struct EXT2DirectoryEntry *tail = get_directory_entry(block.buf, last);
            tail->rec_len = (uint16_t) (BLOCK_SIZE - last);
            memcpy(out + block_index * BLOCK_SIZE, block.buf, BLOCK_SIZE);
            block_index++;
            memset(&block, 0, sizeof(block));
            offset = 0;
            if (i == count)
                break;
        }
        struct EXT2DirectoryEntry *e = get_directory_entry(block.buf, offset);
        e->inode     = entries[i].node;
        e->rec_len   = rec;
        e->name_len  = entries[i].name_len;
        e->file_type = entries[i].type == COSMOFS_TYPE_DIR ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
        memcpy(get_entry_name(e), entries[i].name, entries[i].name_len);
        last    = offset;
        offset += rec;
    }
    memset(out + block_index * BLOCK_SIZE, 0, request->buffer_size - block_index * BLOCK_SIZE);
    kfree(entries);
    return 0;
}

int8_t vfs_read_directory(struct EXT2DriverRequest *request) {
    if (!mounted || request == NULL)
        return -1;
    if (root_kind == VFS_EXT2)
        return read_directory(request);
    return cosmo_read_directory(request);
}

int8_t vfs_write(struct EXT2DriverRequest *request) {
    if (!mounted || request == NULL)
        return -1;
    if (root_kind == VFS_EXT2) {
        int8_t ret = write(request);
        ata_flush(ATA_DRIVE_MASTER);
        return ret;
    }
    struct CosmoFSRequest c = to_cosmo(request);
    return cosmofs_write(&cosmo, &c);
}

int8_t vfs_delete(struct EXT2DriverRequest request) {
    if (!mounted)
        return -1;
    if (root_kind == VFS_EXT2) {
        int8_t ret = delete(request);
        ata_flush(ATA_DRIVE_MASTER);
        return ret;
    }
    struct CosmoFSRequest c = to_cosmo(&request);
    return cosmofs_delete(&cosmo, &c);
}

/* ================================ EXTENSIONS ======================================= */

int8_t vfs_rename(uint32_t parent, const char *name, uint8_t name_len,
                  uint32_t new_parent, const char *new_name, uint8_t new_name_len) {
    if (!mounted)
        return -1;
    if (root_kind == VFS_EXT2) {
        int8_t ret = ext2_rename(parent, name, name_len, new_parent, new_name, new_name_len);
        ata_flush(ATA_DRIVE_MASTER);
        return ret;
    }
    return cosmofs_rename(&cosmo, parent, name, name_len, new_parent, new_name, new_name_len);
}

int8_t vfs_stat(struct SyscallStat *stat) {
    if (!mounted)
        return 2;
    if (root_kind == VFS_EXT2) {
        uint32_t inode;
        uint8_t type;
        int8_t ret = ext2_lookup(stat->parent, stat->name, stat->name_len, &inode, &type);
        if (ret != 0)
            return ret;
        struct EXT2Inode node;
        ext2_get_inode(inode, &node);
        stat->inode   = inode;
        stat->type    = type == EXT2_FT_DIR ? SYSCALL_TYPE_DIR : SYSCALL_TYPE_FILE;
        stat->size    = node.i_size;
        stat->blocks  = node.i_blocks;
        stat->extents = 0;
        stat->links   = node.i_links_count;
        stat->ctime   = node.i_ctime;
        stat->mtime   = node.i_mtime;
        return 0;
    }
    struct CosmoFSStat st;
    int8_t ret = cosmofs_lookup(&cosmo, stat->parent, stat->name, stat->name_len, &st);
    if (ret != 0)
        return ret;
    stat->inode   = st.node;
    stat->type    = st.type == COSMOFS_TYPE_DIR ? SYSCALL_TYPE_DIR : SYSCALL_TYPE_FILE;
    stat->size    = st.size;
    stat->blocks  = st.block_count;
    stat->extents = st.extent_count;
    stat->links   = 1;
    stat->ctime   = st.ctime;
    stat->mtime   = st.mtime;
    return 0;
}

uint32_t vfs_find(const char *name, uint8_t name_len, struct SyscallFindHit *hits, uint32_t max_hits) {
    if (!mounted)
        return 0;
    if (root_kind == VFS_EXT2) {
        struct EXT2FindResult *results = kmalloc((max_hits ? max_hits : 1) * sizeof(*results));
        if (results == NULL)
            return 0;
        uint32_t total = ext2_find(name, name_len, results, max_hits);
        for (uint32_t i = 0; i < total && i < max_hits; i++) {
            hits[i].inode  = results[i].inode;
            hits[i].parent = results[i].parent_inode;
            hits[i].type   = results[i].file_type == EXT2_FT_DIR ? SYSCALL_TYPE_DIR : SYSCALL_TYPE_FILE;
        }
        kfree(results);
        return total;
    }
    struct CosmoFSFindResult *results = kmalloc((max_hits ? max_hits : 1) * sizeof(*results));
    if (results == NULL)
        return 0;
    uint32_t total = cosmofs_find(&cosmo, name, name_len, results, max_hits);
    for (uint32_t i = 0; i < total && i < max_hits; i++) {
        hits[i].inode  = results[i].node;
        hits[i].parent = results[i].parent;
        hits[i].type   = results[i].type == COSMOFS_TYPE_DIR ? SYSCALL_TYPE_DIR : SYSCALL_TYPE_FILE;
    }
    kfree(results);
    return total;
}

bool vfs_name_of(uint32_t inode, uint32_t *parent, char *name_out) {
    if (!mounted || inode == vfs_root())
        return false;
    if (root_kind == VFS_EXT2)
        return ext2_get_name(inode, parent, name_out);
    struct CosmoFSStat st;
    if (!cosmofs_stat(&cosmo, inode, &st))
        return false;
    *parent = st.parent;
    memcpy(name_out, st.name, sizeof(st.name));
    return true;
}

void vfs_info(struct SyscallFsInfo *info) {
    memset(info, 0, sizeof(*info));
    snprintf(info->fs_name, sizeof(info->fs_name), "%s", root_kind == VFS_EXT2 ? "EXT2 (IF2130 Edition)" : "CosmoFS v1 (journaled)");
    snprintf(info->device, sizeof(info->device), "%s", root_kind == VFS_EXT2 ? "storage.bin (ATA master)" : "cosmo.bin (ATA slave)");
    info->block_size = BLOCK_SIZE;
    if (!mounted)
        return;
    if (root_kind == VFS_EXT2) {
        struct EXT2Stats st;
        ext2_get_stats(&st);
        info->total_blocks = st.blocks_count;
        info->free_blocks  = st.free_blocks_count;
        info->total_nodes  = st.inodes_count;
        info->free_nodes   = st.free_inodes_count;
        info->mount_count  = st.mount_count;
        return;
    }
    struct CosmoFSStats st;
    cosmofs_get_stats(&cosmo, &st);
    info->total_blocks     = st.total_blocks;
    info->free_blocks      = st.free_blocks;
    info->total_nodes      = st.node_count;
    info->free_nodes       = st.free_nodes;
    info->mount_count      = st.mount_count;
    info->journal_sequence = st.journal_sequence;
}

void vfs_check(struct SyscallFsCheck *result) {
    memset(result, 0, sizeof(*result));
    if (!mounted) {
        snprintf(result->message, sizeof(result->message), "no root file system mounted");
        return;
    }
    if (root_kind == VFS_EXT2) {
        struct EXT2CheckReport r;
        result->clean  = ext2_check(&r);
        result->errors = r.errors;
        if (result->clean)
            snprintf(result->message, sizeof(result->message), "clean: %u files, %u dirs, %u blocks in use", r.files, r.dirs, r.used_blocks);
        else
            snprintf(result->message, sizeof(result->message), "%s", r.message);
        return;
    }
    struct CosmoFSCheckReport r;
    result->clean  = cosmofs_check(&cosmo, &r);
    result->errors = r.errors;
    if (result->clean)
        snprintf(result->message, sizeof(result->message), "clean: %u files, %u dirs, %u extents, %u blocks in use", r.files, r.dirs, r.extents, r.used_blocks);
    else
        snprintf(result->message, sizeof(result->message), "%s", r.message);
}
