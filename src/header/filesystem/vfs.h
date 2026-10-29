#ifndef _VFS_H
#define _VFS_H

#include <stdint.h>
#include <stdbool.h>
#include "header/filesystem/ext2.h"
#include "header/kernel/syscall.h"

/**
 * Root file system layer.
 *
 * cOSmOS ships two file systems but runs on exactly ONE at a time (grader feedback after
 * Milestone 2): the GRUB menu entry (kernel parameter fs=ext2 / fs=cosmo, default EXT2)
 * decides which one is mounted as the root file system.
 *
 *   EXT2 (IF2130 Edition) - storage.bin, ATA primary master   (default, mandatory spec)
 *   CosmoFS               - cosmo.bin,   ATA primary slave    (bonus: custom file system)
 *
 * Both disk images are 12 MiB: a 4 MiB file system region followed by an 8 MiB swap
 * partition (swap.h). The other disk is never touched.
 *
 * Every operation below uses the book's EXT2DriverRequest and EXT2 error codes for both
 * file systems; for CosmoFS, read_directory() produces EXT2 directory blocks, so user
 * programs (the shell) do not care which file system is mounted.
 */

enum VFSKind {
    VFS_EXT2    = 0,
    VFS_COSMOFS = 1,
};

#define VFS_FS_REGION_BLOCKS 8192u // 4 MiB file system region at the start of each disk

/**
 * Mount the chosen root file system (formatting a blank disk) and enable swap on the same
 * disk. The log callback receives one human readable line per step.
 * @return true when the root file system is usable
 */
bool vfs_mount_root(enum VFSKind kind, void (*log)(const char *line));

bool         vfs_ready(void);
enum VFSKind vfs_kind(void);
const char  *vfs_name(void);     // "ext2" / "cosmo"
uint32_t     vfs_root(void);     // inode / node number of "/" (1 for both)
uint8_t      vfs_drive(void);    // ATA drive holding the root file system and swap

/* -- Book CRUD interface (EXT2DriverRequest, EXT2 error codes) -- */
int8_t vfs_read(struct EXT2DriverRequest request);
int8_t vfs_read_directory(struct EXT2DriverRequest *request);
int8_t vfs_write(struct EXT2DriverRequest *request);
int8_t vfs_delete(struct EXT2DriverRequest request);

/* -- Extensions used by the shell -- */
int8_t   vfs_rename(uint32_t parent, const char *name, uint8_t name_len,
                    uint32_t new_parent, const char *new_name, uint8_t new_name_len);
// 0 found, 1 not found, 2 invalid parent; fills the out fields of `stat`
int8_t   vfs_stat(struct SyscallStat *stat);
uint32_t vfs_find(const char *name, uint8_t name_len, struct SyscallFindHit *hits, uint32_t max_hits);
bool     vfs_name_of(uint32_t inode, uint32_t *parent, char *name_out);
void     vfs_info(struct SyscallFsInfo *info);
void     vfs_check(struct SyscallFsCheck *result);

#endif
