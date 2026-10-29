#ifndef _BOOT_H
#define _BOOT_H

#include <stdint.h>
#include <stdbool.h>
#include "header/filesystem/vfs.h"

/**
 * Multiboot information structure, the part cOSmOS reads (Multiboot 0.6.96 spec, 3.3).
 * GRUB passes its physical address in ebx; kernel-entrypoint.s saves it.
 */
#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002u
#define MULTIBOOT_INFO_MEMORY      0x00000001u // mem_lower / mem_upper are valid
#define MULTIBOOT_INFO_CMDLINE     0x00000004u // cmdline is valid

struct MultibootInfo {
    uint32_t flags;
    uint32_t mem_lower;   // KiB below 1 MiB
    uint32_t mem_upper;   // KiB above 1 MiB
    uint32_t boot_device;
    uint32_t cmdline;     // physical address of a null-terminated string
} __attribute__((packed));

/**
 * Boot options, from the GRUB menu entry (kernel command line):
 *   fs=ext2 | fs=cosmo    root file system (default ext2)
 *   video=text            stay in 80x25 VGA text mode
 *   mem=<MiB>             manage at most this much RAM (swap demo without rebuilding)
 */
struct BootInfo {
    enum VFSKind root_fs;
    bool         text_mode;
    uint32_t     mem_limit_mb;    // 0 = no limit
    uint32_t     detected_ram_kb; // 0 = GRUB did not say
    uint32_t     managed_ram_kb;  // what paging_init() ended up using
    char         cmdline[64];
};

extern struct BootInfo boot_info;

#endif
