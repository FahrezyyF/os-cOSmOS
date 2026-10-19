#ifndef _DISK_H
#define _DISK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -- ATA PIO status codes -- */
#define ATA_STATUS_BSY   0x80
#define ATA_STATUS_RDY   0x40
#define ATA_STATUS_DRQ   0x08
#define ATA_STATUS_DF    0x20
#define ATA_STATUS_ERR   0x01

/* -- ATA PIO primary bus ports & commands -- */
#define ATA_PRIMARY_IO        0x1F0
#define ATA_PRIMARY_CONTROL   0x3F6
#define ATA_REG_DATA          0x0
#define ATA_REG_ERROR         0x1
#define ATA_REG_SECTOR_COUNT  0x2
#define ATA_REG_LBA_LOW       0x3
#define ATA_REG_LBA_MID       0x4
#define ATA_REG_LBA_HIGH      0x5
#define ATA_REG_DRIVE_HEAD    0x6
#define ATA_REG_STATUS        0x7
#define ATA_REG_COMMAND       0x7
#define ATA_CMD_READ_PIO      0x20
#define ATA_CMD_WRITE_PIO     0x30
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_IDENTIFY      0xEC

#define ATA_DRIVE_MASTER 0
#define ATA_DRIVE_SLAVE  1

#define BLOCK_SIZE      512
#define HALF_BLOCK_SIZE (BLOCK_SIZE/2)



// Block buffer data type - @param buf Byte buffer with size of BLOCK_SIZE
struct BlockBuffer {
    uint8_t buf[BLOCK_SIZE];
} __attribute__((packed));

/**
 * ATADriveInfo, result of the ATA IDENTIFY command
 *
 * @param present      Whether a (non-ATAPI) ATA disk answered
 * @param sector_count Addressable LBA28 sectors (disk size = sector_count * BLOCK_SIZE)
 * @param model        Model string reported by the drive, null-terminated
 */
struct ATADriveInfo {
    bool     present;
    uint32_t sector_count;
    char     model[41];
};



/**
 * ATA PIO logical block address read blocks. Will blocking until read is completed.
 * Note: ATA PIO will use 2-bytes per read/write operation.
 * Recommended to use struct BlockBuffer
 *
 * @param ptr                   Pointer for storing reading data, this pointer should point to already allocated memory location.
 *                              With allocated size positive integer multiple of BLOCK_SIZE, ex: buf[1024]
 * @param logical_block_address Block address to read data from. Use LBA addressing
 * @param block_count           How many block to read, starting from block logical_block_address to lba-1
 */
void read_blocks(void *ptr, uint32_t logical_block_address, uint8_t block_count);

/**
 * ATA PIO logical block address write blocks. Will blocking until write is completed.
 * Note: ATA PIO will use 2-bytes per read/write operation.
 * Recommended to use struct BlockBuffer
 *
 * @param ptr                   Pointer to data that to be written into disk. Memory pointed should be positive integer multiple of BLOCK_SIZE
 * @param logical_block_address Block address to write data into. Use LBA addressing
 * @param block_count           How many block to write, starting from block logical_block_address to lba-1
 */
void write_blocks(const void *ptr, uint32_t logical_block_address, uint8_t block_count);

/* -- Extended driver interface (multiple drives, error reporting) -- */

/**
 * Read sectors from a specific drive on the primary ATA bus.
 * read_blocks() is ata_read(ATA_DRIVE_MASTER, ...).
 *
 * @param drive ATA_DRIVE_MASTER (storage.bin) or ATA_DRIVE_SLAVE (cosmo.bin)
 * @return true on success, false on device error / timeout (buffer content undefined)
 */
bool ata_read(uint8_t drive, void *ptr, uint32_t logical_block_address, uint8_t block_count);

/**
 * Write sectors to a specific drive on the primary ATA bus.
 * The drive may keep the data in its write cache until ata_flush() is called.
 *
 * @return true on success, false on device error / timeout
 */
bool ata_write(uint8_t drive, const void *ptr, uint32_t logical_block_address, uint8_t block_count);

/**
 * ATA FLUSH CACHE: returns once every previously written sector is durable.
 * File systems call it at commit points (QEMU turns it into fdatasync on the image).
 *
 * @return true on success
 */
bool ata_flush(uint8_t drive);

/**
 * Send ATA IDENTIFY to detect a drive and its size. Safe to call when no drive
 * is attached (returns info->present = false instead of hanging).
 */
void ata_identify(uint8_t drive, struct ATADriveInfo *info);

#endif
