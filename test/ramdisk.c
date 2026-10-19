#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ramdisk.h"
#include "header/driver/disk.h"

uint8_t  ramdisk[RAMDISK_SIZE];
uint64_t ramdisk_reads  = 0;
uint64_t ramdisk_writes = 0;

static void check_range(uint32_t lba, uint8_t count) {
    if ((uint64_t) (lba + count) * BLOCK_SIZE > RAMDISK_SIZE) {
        fprintf(stderr, "ramdisk: access beyond the disk (lba=%u count=%u)\n", lba, count);
        abort();
    }
}

void read_blocks(void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    check_range(logical_block_address, block_count);
    memcpy(ptr, ramdisk + (size_t) logical_block_address * BLOCK_SIZE, (size_t) block_count * BLOCK_SIZE);
    ramdisk_reads += block_count;
}

void write_blocks(const void *ptr, uint32_t logical_block_address, uint8_t block_count) {
    check_range(logical_block_address, block_count);
    memcpy(ramdisk + (size_t) logical_block_address * BLOCK_SIZE, ptr, (size_t) block_count * BLOCK_SIZE);
    ramdisk_writes += block_count;
}

void ramdisk_reset(void) {
    memset(ramdisk, 0, sizeof(ramdisk));
}

bool ramdisk_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    size_t n = fread(ramdisk, 1, sizeof(ramdisk), f);
    fclose(f);
    if (n < sizeof(ramdisk))
        memset(ramdisk + n, 0, sizeof(ramdisk) - n);
    return true;
}

bool ramdisk_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return false;
    size_t n = fwrite(ramdisk, 1, sizeof(ramdisk), f);
    fclose(f);
    return n == sizeof(ramdisk);
}
