#include "header/driver/disk.h"
#include "header/cpu/portio.h"
#include "header/stdlib/string.h"

// Kernel space starts here (header/memory/paging.h: KERNEL_VIRTUAL_ADDRESS_BASE)
#define USER_SPACE_END 0xC0000000u

// Upper bound for every polling loop so a missing / broken drive cannot hang the kernel
#define ATA_TIMEOUT 2000000u

static uint8_t ata_status(void) {
    return in(ATA_PRIMARY_IO + ATA_REG_STATUS);
}

// Reading the alternate status register 4 times gives the drive the required 400ns
static void ata_delay_400ns(void) {
    for (int i = 0; i < 4; i++)
        in(ATA_PRIMARY_CONTROL);
}

static bool ATA_busy_wait(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = ata_status();
        if (status == 0xFF)
            return false; // floating bus: no drive at all
        if (!(status & ATA_STATUS_BSY))
            return true;
    }
    return false;
}

/**
 * Wait until the drive is ready to transfer one sector of data.
 * The book's kit waits on ATA_STATUS_RDY, but RDY only means "spun up"; the data
 * port is valid once DRQ is set. ERR / DF abort the wait instead of spinning forever.
 */
static bool ATA_DRQ_wait(void) {
    for (uint32_t i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = ata_status();
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF))
            return false;
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ))
            return true;
    }
    return false;
}

static bool ata_select(uint8_t drive, uint32_t logical_block_address, uint8_t block_count) {
    if (!ATA_busy_wait())
        return false;
    uint8_t drive_bit = (drive == ATA_DRIVE_SLAVE) ? 0x10 : 0x00;
    out(ATA_PRIMARY_IO + ATA_REG_DRIVE_HEAD, 0xE0 | drive_bit | ((logical_block_address >> 24) & 0xF));
    ata_delay_400ns();
    if (!ATA_busy_wait())
        return false;
    out(ATA_PRIMARY_IO + ATA_REG_SECTOR_COUNT, block_count);
    out(ATA_PRIMARY_IO + ATA_REG_LBA_LOW, (uint8_t) logical_block_address);
    out(ATA_PRIMARY_IO + ATA_REG_LBA_MID, (uint8_t) (logical_block_address >> 8));
    out(ATA_PRIMARY_IO + ATA_REG_LBA_HIGH, (uint8_t) (logical_block_address >> 16));
    return true;
}

/**
 * Buffers in user space (system calls, program loading) are not always resident: touching one
 * may page-fault, and the page fault handler may itself use the disk to swap. That must never
 * happen between issuing an ATA command and finishing its data transfer, so user buffers are
 * moved one sector at a time through this kernel bounce buffer, touched only between commands.
 */
static struct BlockBuffer bounce;

static bool is_user_buffer(const void *ptr) {
    return (uint32_t) ptr < USER_SPACE_END;
}

static bool ata_read_direct(uint8_t drive, void *ptr, uint32_t logical_block_address, uint8_t block_count);
static bool ata_write_direct(uint8_t drive, const void *ptr, uint32_t logical_block_address, uint8_t block_count);

bool ata_read(uint8_t drive, void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    if (!is_user_buffer(ptr))
        return ata_read_direct(drive, ptr, logical_block_address, block_count);
    uint8_t *target = (uint8_t*) ptr;
    for (uint32_t i = 0; i < block_count; i++) {
        if (!ata_read_direct(drive, &bounce, logical_block_address + i, 1))
            return false;
        memcpy(target + i * BLOCK_SIZE, &bounce, BLOCK_SIZE); // may page-fault: no command in flight
    }
    return true;
}

bool ata_write(uint8_t drive, const void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    if (!is_user_buffer(ptr))
        return ata_write_direct(drive, ptr, logical_block_address, block_count);
    const uint8_t *source = (const uint8_t*) ptr;
    for (uint32_t i = 0; i < block_count; i++) {
        memcpy(&bounce, source + i * BLOCK_SIZE, BLOCK_SIZE);
        if (!ata_write_direct(drive, &bounce, logical_block_address + i, 1))
            return false;
    }
    return true;
}

static bool ata_read_direct(uint8_t drive, void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    if (block_count == 0) // ATA treats 0 as 256 sectors; callers never mean that
        return true;
    if (!ata_select(drive, logical_block_address, block_count))
        return false;
    out(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    uint16_t *target = (uint16_t*) ptr;
    for (uint32_t i = 0; i < block_count; i++) {
        ata_delay_400ns();
        if (!ATA_busy_wait() || !ATA_DRQ_wait())
            return false;
        for (uint32_t j = 0; j < HALF_BLOCK_SIZE; j++)
            target[j] = in16(ATA_PRIMARY_IO + ATA_REG_DATA);
        target += HALF_BLOCK_SIZE;
    }
    return true;
}

static bool ata_write_direct(uint8_t drive, const void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    if (block_count == 0)
        return true;
    if (!ata_select(drive, logical_block_address, block_count))
        return false;
    out(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    const uint16_t *source = (const uint16_t*) ptr;
    for (uint32_t i = 0; i < block_count; i++) {
        ata_delay_400ns();
        if (!ATA_busy_wait() || !ATA_DRQ_wait())
            return false;
        for (uint32_t j = 0; j < HALF_BLOCK_SIZE; j++)
            out16(ATA_PRIMARY_IO + ATA_REG_DATA, source[HALF_BLOCK_SIZE*i + j]);
    }

    ata_delay_400ns();
    return ATA_busy_wait() && !(ata_status() & (ATA_STATUS_ERR | ATA_STATUS_DF));
}

bool ata_flush(uint8_t drive) {
    if (!ATA_busy_wait())
        return false;
    out(ATA_PRIMARY_IO + ATA_REG_DRIVE_HEAD, (drive == ATA_DRIVE_SLAVE) ? 0xF0 : 0xE0);
    ata_delay_400ns();
    if (!ATA_busy_wait())
        return false;
    out(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_delay_400ns();
    return ATA_busy_wait() && !(ata_status() & (ATA_STATUS_ERR | ATA_STATUS_DF));
}

void read_blocks(void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    ata_read(ATA_DRIVE_MASTER, ptr, logical_block_address, block_count);
}

void write_blocks(const void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    ata_write(ATA_DRIVE_MASTER, ptr, logical_block_address, block_count);
}

void ata_identify(uint8_t drive, struct ATADriveInfo *info) {
    info->present      = false;
    info->sector_count = 0;
    info->model[0]     = '\0';

    if (ata_status() == 0xFF)
        return; // no controller / floating bus
    out(ATA_PRIMARY_IO + ATA_REG_DRIVE_HEAD, (drive == ATA_DRIVE_SLAVE) ? 0xB0 : 0xA0);
    ata_delay_400ns();
    out(ATA_PRIMARY_IO + ATA_REG_SECTOR_COUNT, 0);
    out(ATA_PRIMARY_IO + ATA_REG_LBA_LOW, 0);
    out(ATA_PRIMARY_IO + ATA_REG_LBA_MID, 0);
    out(ATA_PRIMARY_IO + ATA_REG_LBA_HIGH, 0);
    out(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns();

    if (ata_status() == 0)
        return; // drive does not exist
    if (!ATA_busy_wait())
        return;
    // ATAPI (CD-ROM) / SATA devices put a signature here and are not ATA disks
    if (in(ATA_PRIMARY_IO + ATA_REG_LBA_MID) != 0 || in(ATA_PRIMARY_IO + ATA_REG_LBA_HIGH) != 0)
        return;
    if (!ATA_DRQ_wait())
        return;

    uint16_t identify[256];
    for (int i = 0; i < 256; i++)
        identify[i] = in16(ATA_PRIMARY_IO + ATA_REG_DATA);

    // Words 27-46: model string, bytes swapped inside each word
    for (int i = 0; i < 20; i++) {
        info->model[2 * i]     = (char) (identify[27 + i] >> 8);
        info->model[2 * i + 1] = (char) (identify[27 + i] & 0xFF);
    }
    info->model[40] = '\0';
    for (int i = 39; i >= 0 && info->model[i] == ' '; i--)
        info->model[i] = '\0';

    // Words 60-61: total number of LBA28 addressable sectors
    info->sector_count = (uint32_t) identify[60] | ((uint32_t) identify[61] << 16);
    info->present      = true;
}
