#ifndef _SWAP_H
#define _SWAP_H

#include <stdint.h>
#include <stdbool.h>
#include "header/memory/paging.h"

/**
 * Swap space (book 3.2): a raw partition appended after the 4 MiB file system region of
 * the active disk image (storage.bin / cosmo.bin are 12 MiB: 4 MiB file system + 8 MiB swap).
 *
 * A swapped-out page is recorded in its own page table entry: present = 0, PTE_SWAPPED
 * (OS-available bit 9) = 1, bits 31:12 = swap slot, R/W and U/S keep the old permissions.
 * An all-zero entry therefore means "never allocated", which the page fault handler
 * satisfies with a fresh zero page (demand paging).
 */

#define PAGE_FRAME_ON_SWAP_MAX_COUNT 2048 //can be changed to be as many as you want. (2048 x 4 KiB = 8 MiB)
#define SWAP_SECTORS_PER_PAGE        (PAGE_FRAME_SIZE / 512u)
#define SWAP_PARTITION_START_LBA     8192u // right after the 4 MiB file system region

struct SwapManager {
    bool     swap_used[PAGE_FRAME_ON_SWAP_MAX_COUNT];
    uint32_t free_count;
} __attribute__((packed));

extern struct SwapManager swap_manager;

/**
 * SwapStats, counters shown by `free` / `meminfo` in the shell
 */
struct SwapStats {
    bool     available;
    uint32_t total_slots;
    uint32_t used_slots;
    uint32_t page_faults;      // every vector 14 handled
    uint32_t demand_zero;      // fresh zero pages mapped on first touch
    uint32_t swap_ins;
    uint32_t swap_outs;
    uint32_t failed;           // faults that could not be resolved (bad address / out of memory)
};

/**
 * Read and put frame back to memory
 * @param page_dir        Page directory to read
 * @param virtual_addr    Virtual Address that caused page fault
 */
bool swap_in(struct PageDirectory *page_dir, void *virtual_addr);

/**
 * write frame to swap
 * @param page_dir        Page directory to read
 * @param virtual_addr    Virtual Address that caused page fault
 */
void swap_out(struct PageDirectory *page_dir, void *virtual_addr);

/**
 * read page fault address from cr2
 * (static inline: the book defines this function in the header, which would otherwise
 * produce one definition per translation unit)
 */
static inline uint32_t read_cr2(void) {
    uint32_t fault_address;
    __asm__ volatile ("mov %%cr2, %0": "=r"(fault_address));
    return fault_address;
}

/**
 * handle page fault
 * @param error_code error code read from cpu state
 * make sure to handle every cases including page replacement
 * @return true when the faulting instruction can be restarted
 *         (false = invalid access or out of memory: the caller kills the program / panics)
 */
bool handle_page_fault(uint32_t error_code);

/* -- cOSmOS extensions -- */

/**
 * Use [start_lba, start_lba + slot_count * 8) of an ATA drive as swap space.
 * slot_count is clamped to PAGE_FRAME_ON_SWAP_MAX_COUNT; 0 disables swapping.
 */
void swap_init(uint8_t drive, uint32_t start_lba, uint32_t slot_count);

/**
 * Same as swap_out() but reports whether the page really left RAM
 */
bool swap_out_page(struct PageDirectory *page_dir, uint32_t virtual_addr);

/**
 * Page replacement: choose a resident user page with the clock algorithm and swap it out,
 * freeing one physical frame. Called by paging_frame_alloc() when RAM is exhausted.
 */
bool swap_reclaim_frame(void);

// Give a slot back (used when a swapped page is freed without being swapped in)
void swap_release_slot(uint32_t slot);

void swap_get_stats(struct SwapStats *stats);

// Did the last failed page fault fail for lack of memory (RAM and swap full)?
bool swap_last_fault_was_oom(void);

#endif
