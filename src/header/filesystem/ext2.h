#ifndef _EXT2_H
#define _EXT2_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/driver/disk.h"

/**
 * EXT2 - IF2130 Edition
 *
 * On-disk layout (512-byte blocks, 4 MiB disk, 8 block groups of 1024 blocks):
 *   block 0                : boot sector, holds fs_signature
 *   block 1                : superblock
 *   block 2                : block group descriptor table
 *   group g (g = 0 starts at block 3, others at g * 1024):
 *     +0 block bitmap, +1 inode bitmap, +2..+17 inode table (16 blocks), then data blocks
 *
 * The layout, the 128-byte inode, the 8-byte directory entry header and the MSB-first
 * bitmap bit order are byte-compatible with the reference implementation that produced
 * .kit/ch2/sample-image.bin (verified with a hex dump of that image).
 */

/* -- IF2130 File System constants -- */
#define BOOT_SECTOR 0 // legacy from FAT32 filesystem IF2130 OS
#define DISK_SPACE 4194304u // 4MB disk space (because our disk or storage.bin is 4MB)
#define EXT2_SUPER_MAGIC 0xEF53 // this indicating that the filesystem used by OS is ext2
#define INODE_SIZE sizeof(struct EXT2Inode) // size of inode (128 bytes)
#define INODES_PER_TABLE (BLOCK_SIZE / INODE_SIZE) // number of inode per block (512 / 128 = 4)
// Kit bug fix: the whole expression must be parenthesized, otherwise
// BLOCKS_PER_GROUP expands to DISK_SPACE / BLOCK_SIZE / 16 / 2 = 256 instead of 1024
#define GROUPS_COUNT ((BLOCK_SIZE / sizeof(struct EXT2BlockGroupDescriptor)) / 2u) // number of groups in the filesystem (8)
#define BLOCKS_PER_GROUP (DISK_SPACE / BLOCK_SIZE / GROUPS_COUNT) // number of blocks per group (1024)
#define INODES_TABLE_BLOCK_COUNT 16u
#define INODES_PER_GROUP (INODES_PER_TABLE * INODES_TABLE_BLOCK_COUNT) // number of inodes per group (64)

/* -- Derived constants -- */
#define EXT2_BLOCKS_COUNT        (DISK_SPACE / BLOCK_SIZE)          // 8192 blocks
#define EXT2_INODES_COUNT        (INODES_PER_GROUP * GROUPS_COUNT)  // 512 inodes
#define EXT2_ROOT_INODE          1u                                 // inode numbering starts at 1
#define EXT2_SUPERBLOCK_LOCATION 1u
#define EXT2_BGD_TABLE_LOCATION  2u
#define EXT2_NAME_MAX            255u
#define EXT2_N_BLOCKS            15u
#define EXT2_NDIR_BLOCKS         12u                                // i_block[0..11] are direct
#define EXT2_IND_BLOCK           12u                                // single indirect
#define EXT2_DIND_BLOCK          13u                                // doubly indirect
#define EXT2_TIND_BLOCK          14u                                // triply indirect
#define EXT2_POINTERS_PER_BLOCK  (BLOCK_SIZE / sizeof(uint32_t))    // 128
#define EXT2_DIR_ENTRY_HEADER    8u                                 // inode + rec_len + name_len + file_type



/**
 * inodes constant
 * - reference: https://www.nongnu.org/ext2-doc/ext2.html#inode-table
 */
#define EXT2_S_IFREG 0x8000 // regular file
#define EXT2_S_IFDIR 0x4000 // directory
#define EXT2_S_IFMT  0xF000 // mask of the file type bits in i_mode


/* FILE TYPE CONSTANT*/
/**
 * reference:
 * - https://www.nongnu.org/ext2-doc/ext2.html#linked-directories
 * - Table 4.2. Defined Inode File Type Values
 */

#define EXT2_FT_UNKNOWN 0 // Unknown File Type
#define EXT2_FT_REG_FILE 1 // Regular File
#define EXT2_FT_DIR 2 // Directory
#define EXT2_FT_NEXT 3 // Character Special File

/**
 * EXT2DriverRequest
 * Derived dand modified from FAT32DriverRequest legacy IF2130 OS
 *
 * @param buf          Data buffer (read: destination, write: source)
 * @param name         Entry name, NOT necessarily null-terminated (name_len is used)
 * @param name_len     Length of name in bytes (1 - 255)
 * @param parent_inode Inode of the directory that contains (or will contain) the entry
 * @param buffer_size  Size of buf in bytes. write(): size of the file to create
 * @param is_directory write(): create a directory instead of a regular file
 */
struct EXT2DriverRequest
{
    void *buf;
    char *name;
    uint8_t name_len;
    uint32_t parent_inode;
    uint32_t buffer_size;

    bool is_directory;
}__attribute__((packed));

/**
 * EXT2Superblock:
 * - https://www.nongnu.org/ext2-doc/ext2.html#superblock
 */
struct EXT2Superblock
{
    uint32_t s_inodes_count;        // 32bit value indicating the total number of inodes, both used and free, in the file system
    uint32_t s_blocks_count;        // 32bit value indicating the total number of blocks in the system including all used, free and reserved

    uint32_t s_r_blocks_count;      // 32bit value indicating the total number of blocks reserved for the usage of the super user. {maybe not used because there is no superuser in our system}
    uint32_t s_free_blocks_count;   // 32bit value indicating the total number of free blocks, including the number of reserved blocks
    uint32_t s_free_inodes_count;   // 32bit value indicating the total number of free inodes. This is a sum of all free inodes of all the block groups.
    uint32_t s_first_data_block;    // 32bit value identifying the first data block, in other word the id of the block containing the superblock structure.
    uint32_t s_first_ino;           // 32bit value indicating the first inode that can be used. Set this to 1, indicating root inode (maybe)

    uint32_t s_blocks_per_group;
    /** 32bit value indicating the total number of blocks per group.
     *  This value in combination with s_first_data_block can be used to determine the block groups boundaries.
     *  Due to volume size boundaries, the last block group might have a smaller number of blocks than what is specified in this field. */

    uint32_t s_frags_per_group;
    /**
     * 32bit value indicating the total number of fragments per group. It is also used to determine the size of the block bitmap of each block group.
     */

    uint32_t s_inodes_per_group;
    /**
     * 32bit value indicating the total number of inodes per group. This is also used to determine the size of the inode bitmap of each block group.
     * Note that you cannot have more than (block size in bytes * 8) inodes per group as the inode bitmap must fit within a single block.
     * This value must be a perfect multiple of the number of inodes that can fit in a block ((1024<<s_log_block_size)/s_inode_size).
     */

    uint16_t s_magic; // 16bit value indicating the file system type. For ext2, this value is 0xEF53.(DEFINE as EXT2_SUPER_MAGIC)

    uint8_t s_prealloc_blocks; // 8bit value indicating the number of blocks to preallocate for files.
    uint8_t s_prealloc_dir_blocks; // 8bit value indicating the number of blocks to preallocate for directories.

    uint32_t s_mtime;       // last mount time (Unix time, 0 if no clock source)
    uint32_t s_wtime;       // last write time
    uint16_t s_mnt_count;   // number of mounts since the file system was created
    uint16_t s_state;       // EXT2_VALID_FS when cleanly committed
}__attribute__((packed));

#define EXT2_VALID_FS 1


/**
 * reference:
 * - https://www.nongnu.org/ext2-doc/ext2.html#block-group-descriptor-table
 */
struct EXT2BlockGroupDescriptor
{
    /**
     * 32bit block id of the first block of the “block bitmap” for the group represented.
     * The actual block bitmap is located within its own allocated blocks starting at the block ID specified by this value.
     */
    uint32_t bg_block_bitmap;

    /**
     * 32bit block id of the first block of the “inode bitmap” for the group represented.
     */
    uint32_t bg_inode_bitmap;

    /**
     * 32bit block id of the first block of the "inode table" for the group represented.
     */
    uint32_t bg_inode_table;

    /**
     * 16bit value indicating the total number of free blocks for the represented group.
     */
    uint16_t bg_free_blocks_count;

    /**
     * 16bit value indicating the total number of free inodes for the represented group.
     */
    uint16_t bg_free_inodes_count;

    /**
     * 16bit value indicating the number of inodes allocated to directories for the represented group.
     */
    uint16_t bg_used_dirs_count;

    /**
     * 16bit value used for padding the structure on a 32bit boundary.
     */
    uint16_t bg_pad;

    /**
     * 12 bytes of reserved space for future revisions.
     */
    uint32_t bg_reserved[3]; // 12 bytes of reserved space for future revisions.
}__attribute__((packed));

/**
 * reference:
 * - https://www.nongnu.org/ext2-doc/ext2.html#block-group-descriptor-table
 * Note: this struct is only GROUPS_COUNT * 32 = 256 bytes, never read_blocks() a whole
 * 512-byte block directly into it (use a BlockBuffer and copy).
 */
struct EXT2BlockGroupDescriptorTable
{
    struct EXT2BlockGroupDescriptor table[GROUPS_COUNT]; // can be change with fixed size array
};


/**
 * EXT2Inode
 * Inode stands for index node, it is a data structure in a Unix-style file system that describes a file-system object such as a file or a directory.
 * 128 bytes, field offsets identical to the reference implementation (sample-image.bin).
 */

struct EXT2Inode
{
    uint16_t i_mode;        // 0:  16bit value indicating the file type and the access rights.
    uint32_t i_size;        // 2:  32bit value indicating the size of the file in bytes.
    uint32_t i_atime;       // 6:  last access time (Unix time)
    uint32_t i_ctime;       // 10: creation time
    uint32_t i_mtime;       // 14: last modification time
    uint32_t i_dtime;       // 18: deletion time
    uint16_t i_gid;         // 22: group id (unused)
    uint16_t i_uid;         // 24: user id (unused)
    uint16_t i_links_count; // 26: hard links: 1 for files, 2 + subdirectories for directories
    uint32_t i_blocks;      // 28: 32bit value indicating the number of 512-byte blocks reserved for the node, including indirect blocks

    uint32_t i_flags;       // 32: unused
    uint32_t i_osd1;        // 36: unused

    /**
     * 15 x 32bit block numbers pointing to the blocks containing the data for this inode
     *
     * - The first 12 blocks are direct blocks
     * - The 13th entry in this array is the block number of the first indirect block which is a block containing an array of block ID containing the data
     * Therefore, the 13th block of the file will be the first block ID contained in the indirect block. With a 512 byte block size, blocks 13 to 140 of the file data are contained in this indirect block.
     * - The 14th entry in this array is the block number of the first doubly-indirect block
     * - The 15th entry in this array is the block number of the triply-indirect block
     *
     * maybe this video will help
     * - https://www.youtube.com/watch?v=tMVj22EWg6A
     *
     */
    uint32_t i_block[15];   // 40

    uint32_t i_generation;  // 100: unused
    uint32_t i_file_acl;    // 104: unused
    uint32_t i_dir_acl;     // 108: unused
    uint32_t i_faddr;       // 112: unused
    uint8_t  i_osd2[12];    // 116: unused
}__attribute__((packed));

struct EXT2InodeTable
{
    struct EXT2Inode table[INODES_PER_GROUP]; // can be change with fixed size array
};

/**
 * EXT2DirectoryEntry
 * Linked List Directory
 * reference:
 * - https://www.nongnu.org/ext2-doc/ext2.html#linked-directories
 *
 * The name (name_len bytes, not null-terminated) directly follows this 8-byte header.
 * Entries are 4-byte aligned and never span blocks; the last entry of a block
 * extends its rec_len to the end of the block.
 */

struct EXT2DirectoryEntry
{
    uint32_t inode; // 32bit value indicating the inode number of the file entry. A value of 0 indicate that the entry is not used.
    /**
     * 16bit unsigned displacement to the next directory entry from the start of the current directory entry.
     * This field must have a value at least equal to the length of the current record.
     * The directory entries must be aligned on 4 bytes boundaries and there cannot be any directory entry spanning multiple data blocks.
     * If an entry cannot completely fit in one block, it must be pushed to the next data block and the rec_len of the previous entry properly adjusted.
     */
    uint16_t rec_len;

    /**
     * 8bit value indicating the length of the file name.
     * (Kit bug fix: was declared uint16_t, which made the header 9 bytes and
     * disagreed with both the ext2 specification and sample-image.bin)
     */
    uint8_t name_len;

    /**
     * 8bit unsigned value used to indicate file type.
     */
    uint8_t file_type;

}__attribute__((packed));

/**
 * EXT2FindResult, one hit of ext2_find() (file system indexing bonus)
 *
 * @param inode        Inode of the matching entry
 * @param parent_inode Directory that contains it
 * @param file_type    EXT2_FT_REG_FILE / EXT2_FT_DIR
 */
struct EXT2FindResult
{
    uint32_t inode;
    uint32_t parent_inode;
    uint8_t  file_type;
};

/**
 * EXT2Stats, usage summary for `df`-like commands
 */
struct EXT2Stats
{
    uint32_t blocks_count;
    uint32_t free_blocks_count;
    uint32_t inodes_count;
    uint32_t free_inodes_count;
    uint32_t used_dirs_count;
    uint16_t mount_count;
};

/**
 * EXT2CheckReport, result of ext2_check() (fsck-like consistency check)
 *
 * @param errors        Number of inconsistencies found (0 = consistent)
 * @param files, dirs   Number of reachable regular files / directories
 * @param used_blocks   Blocks referenced by reachable inodes (data + indirect)
 * @param message       First inconsistency found, human readable
 */
struct EXT2CheckReport
{
    uint32_t errors;
    uint32_t files;
    uint32_t dirs;
    uint32_t used_blocks;
    char     message[96];
};

/**
 *  REGULAR function
 */

/**
 * get the name of the entry
 * @param entry the directory entry
 * @return the name of the entry
 */
char *get_entry_name(void *entry);

/**
 * get the directory entry from the buffer
 * @param ptr the buffer that contains the directory table
 * @param offset the offset of the entry
 * @return the directory entry
 */
struct EXT2DirectoryEntry *get_directory_entry(void *ptr, uint32_t offset);

/**
 * get the next directory entry from the current entry
 * @param entry the current entry
 * @return the next directory entry
 */
struct EXT2DirectoryEntry *get_next_directory_entry(struct EXT2DirectoryEntry *entry);

/**
 * get the record length of the entry
 * @param name_len the length of the name of the entry
 * @return the record length of the entry (header + name, rounded up to 4 bytes)
 */
uint16_t get_entry_record_len(uint8_t name_len);

/**
 * get the offset of the first child of the directory
 * @param ptr the buffer that contains the directory table
 * @return the offset of the first child of the directory (right after "." and "..")
 */
uint32_t get_dir_first_child_offset(void *ptr);


/* =================== MAIN FUNCTION OF EXT32 FILESYSTEM ============================*/

/**
 * @brief get bgd index from inode, inode will starts at index 1
 * @param inode 1 to INODES_PER_GROUP * GROUP_COUNT
 * @return bgd index (0 to GROUP_COUNT - 1)
 */
uint32_t inode_to_bgd(uint32_t inode);

/**
 * @brief get inode local index in the corrresponding bgd
 * @param inode 1 to INODES_PER_GROUP * GROUP_COUNT
 * @return local index
 */
uint32_t inode_to_local(uint32_t inode);

/**
 * @brief create a new directory using given node
 * first item of directory table is its node location (name will be .)
 * second item of directory is its parent location (name will be ..)
 * Allocates one data block for the directory table (and writes it).
 * @param node pointer of inode
 * @param inode inode that already allocated
 * @param parent_inode inode of parent directory (if root directory, the parent is itself)
 */
void init_directory_table(struct EXT2Inode *node, uint32_t inode, uint32_t parent_inode);
/**
 * @brief check whether filesystem signature is missing or not in boot sector
 *
 * @return true if memcmp(boot_sector, fs_signature) returning inequality
 */
bool is_empty_storage(void);

/**
 * @brief create a new EXT2 filesystem. Will write fs_signature into boot sector,
 * initialize super block, bgd table, block and inode bitmap, and create root directory
 */
void create_ext2(void);

/**
 * @brief Initialize file system driver state, if is_empty_storage() then create_ext2()
 * Else, read and cache super block (located at block 1) and bgd table (located at block 2) into state
 */
void initialize_filesystem_ext2(void);

/**
 * @brief check whether a directory table has children or not
 * @param inode of a directory table
 * @return true if the directory only contains "." and ".."
 */
bool is_directory_empty(uint32_t inode);



/* =============================== CRUD FUNC ======================================== */

/**
 * @brief EXT2 Folder / Directory read
 * Copies the directory table (all data blocks of the directory, a sequence of
 * EXT2DirectoryEntry records) of request->name inside request->parent_inode into request->buf.
 * Use name "." (or name_len 0) to read the parent directory itself.
 * On success the bytes of buf after the directory table (up to buffer_size) are zeroed,
 * so a block whose first entry has rec_len == 0 marks the end of the table.
 * @param request buf point to struct EXT2 Directory
 * @return Error code: 0 success - 1 not a folder - 2 not found - 3 parent folder invalid - -1 unknown
 *         (-1 also when buffer_size is too small; request->buffer_size then holds the size needed)
 */
int8_t read_directory(struct EXT2DriverRequest *prequest);

/**
 * @brief EXT2 read, read a file from file system
 * @param request All attribute will be used except is_dir for read, buffer_size will limit reading count
 * @return Error code: 0 success - 1 not a file - 2 not enough buffer - 3 not found - 4 parent folder invalid - -1 unknown
 */
int8_t read(struct EXT2DriverRequest request);

/**
 * @brief EXT2 write, write a file or a folder to file system
 *
 * @param All attribute will be used for write. is_directory == true creates an empty folder
 *        (buf / buffer_size ignored), otherwise a regular file of buffer_size bytes (0 allowed).
 *        Names are unique inside a directory regardless of type (book 2.6.2).
 * @return Error code: 0 success - 1 file/folder already exist - 2 invalid parent folder - -1 unknown
 *         (-1 also for invalid names, no free inode, or not enough free blocks; nothing is modified then)
 */
int8_t write(struct EXT2DriverRequest *request);

/**
 * @brief EXT2 delete, delete a file or empty directory in file system
 *  @param request buf and buffer_size is unused. The entry is selected by name (any type);
 *         "." and ".." can never be deleted.
 * @return Error code: 0 success - 1 not found - 2 folder is not empty - 3 parent folder invalid -1 unknown
 */
int8_t delete(struct EXT2DriverRequest request);

/* =============================== MEMORY ==========================================*/

/**
 * @brief get a free inode from the disk (first fit over all groups)
 * @return new inode, 0 if no inode is free
 */
uint32_t allocate_node(void);

/**
 * @brief deallocate node from the disk, will also deallocate its used blocks
 * also all of the blocks of indirect blocks if necessary
 * @param inode that needs to be deallocated
 */
void deallocate_node(uint32_t inode);

/**
 * @brief deallocate node blocks: every data block plus single / doubly / triply
 * indirect pointer blocks referenced by an inode's i_block array
 * @param loc node->i_block (15 entries)
 * @param blocks number of data blocks of the node (ceil(i_size / BLOCK_SIZE))
 */
void deallocate_blocks(void *loc, uint32_t blocks);

/**
 * @brief write node->block in the given node, will allocate
 * ceil(node->i_size / BLOCK_SIZE) data blocks with first fit; if first 12 item of node->block
 * is not enough, will use indirect blocks (single, doubly, and triply indirect are supported)
 * @param ptr the buffer that needs to be written (node->i_size bytes)
 * @param node pointer of the node
 * @param prefered_bgd it is located at the node inode bgd (allocation starts at this group)
 */
void allocate_node_blocks(void *ptr, struct EXT2Inode *node, uint32_t prefered_bgd);

/**
 * @brief update the node to the disk
 * @param node pointer of node
 * @param inode location of the node
 */
void sync_node(struct EXT2Inode *node, uint32_t inode);

/* ============================ EXTENSIONS ========================================= */

/**
 * @brief Set the function used to timestamp inodes (Unix time). The kernel passes
 * rtc_unix_time; host tools may pass a wrapper around time(NULL). NULL = timestamps 0.
 * (ext2.c may only include disk.h, ext2.h, stdint, stdbool and string.h, so the
 * clock is injected instead of included.)
 */
void ext2_set_time_source(uint32_t (*now)(void));

/**
 * @brief Read an inode by number
 * @return false if inode is out of range or not allocated
 */
bool ext2_get_inode(uint32_t inode, struct EXT2Inode *out);

/**
 * @brief Look up `name` inside directory `parent_inode`
 * @param out_inode     Inode of the entry (may be NULL)
 * @param out_file_type EXT2_FT_* of the entry (may be NULL)
 * @return 0 found - 1 not found - 2 parent folder invalid
 */
int8_t ext2_lookup(uint32_t parent_inode, const char *name, uint8_t name_len, uint32_t *out_inode, uint8_t *out_file_type);

/**
 * @brief Rename and/or move an entry without copying its data (`mv` support).
 * Moving a directory also rewrites its ".." entry.
 * @return 0 success - 1 source not found - 2 destination name already exists -
 *         3 source/destination folder invalid - 4 cannot move a folder into itself / its subfolder -
 *         -1 other error ("." / "..", invalid name, disk full).
 *         Checked in this order: -1 (bad names), 3, 1, (same entry -> 0), 2, 4, -1 (disk full).
 */
int8_t ext2_rename(uint32_t parent_inode, const char *name, uint8_t name_len,
                   uint32_t new_parent_inode, const char *new_name, uint8_t new_name_len);

/**
 * @brief Find every entry named `name` in the whole file system using the in-memory
 * name index (file system indexing bonus) instead of a DFS over all directories.
 * The index is built once at mount time and kept in sync by write / delete / rename.
 * @param results     Output array
 * @param max_results Capacity of results
 * @return Total number of matches (may be larger than max_results)
 */
uint32_t ext2_find(const char *name, uint8_t name_len, struct EXT2FindResult *results, uint32_t max_results);

/**
 * @brief Find the parent and the name of an inode (used to build absolute paths)
 * @param name_buf Receives the name (null-terminated), at least EXT2_NAME_MAX + 1 bytes
 * @return false if the inode is not in the index (e.g. root has no name)
 */
bool ext2_get_name(uint32_t inode, uint32_t *parent_inode, char *name_buf);

// Usage statistics from the cached superblock / group descriptors
void ext2_get_stats(struct EXT2Stats *stats);

/**
 * @brief Full consistency check (like e2fsck -n): walks the directory tree from the root,
 * verifies every reachable inode and block pointer, compares the rebuilt block / inode
 * usage against the on-disk bitmaps and the free counters.
 * @return true if no inconsistency was found
 */
bool ext2_check(struct EXT2CheckReport *report);

#endif
