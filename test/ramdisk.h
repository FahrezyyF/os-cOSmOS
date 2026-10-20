#ifndef _TEST_RAMDISK_H
#define _TEST_RAMDISK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * In-memory disk that implements the kernel's read_blocks()/write_blocks()
 * interface so the real ext2.c can be tested on the host.
 */

#define RAMDISK_SIZE (4u * 1024u * 1024u)

extern uint8_t ramdisk[RAMDISK_SIZE];
extern uint64_t ramdisk_reads;
extern uint64_t ramdisk_writes;

// Fill the disk with zeros (like `qemu-img create`)
void ramdisk_reset(void);

// Load / save an image file, returns false on I/O error
bool ramdisk_load(const char *path);
bool ramdisk_save(const char *path);

#endif
