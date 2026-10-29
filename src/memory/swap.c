#include <stdint.h>
#include <stdbool.h>
#include "header/memory/swap.h"
#include "header/memory/paging.h"
#include "header/driver/disk.h"
#include "header/kernel/user.h"

struct SwapManager swap_manager;

static uint8_t  swap_drive      = ATA_DRIVE_MASTER;
static uint32_t swap_start_lba  = SWAP_PARTITION_START_LBA;
static uint32_t swap_slot_count = 0;
static uint32_t swap_next_hint  = 0;
static struct SwapStats stats;
static bool             last_fault_oom  = false;

void swap_init(uint8_t drive, uint32_t start_lba, uint32_t slot_count) {
    if (slot_count > PAGE_FRAME_ON_SWAP_MAX_COUNT)
        slot_count = PAGE_FRAME_ON_SWAP_MAX_COUNT;
    swap_drive      = drive;
    swap_start_lba  = start_lba;
    swap_slot_count = slot_count;
    swap_next_hint  = 0;
    for (uint32_t i = 0; i < PAGE_FRAME_ON_SWAP_MAX_COUNT; i++)
        swap_manager.swap_used[i] = i >= slot_count; // slots past the partition are never handed out
    swap_manager.free_count = slot_count;
    stats.available   = slot_count > 0;
    stats.total_slots = slot_count;
}

static bool slot_alloc(uint32_t *slot_out) {
    if (swap_manager.free_count == 0)
        return false;
    for (uint32_t k = 0; k < swap_slot_count; k++) {
        uint32_t slot = (swap_next_hint + k) % swap_slot_count;
        if (!swap_manager.swap_used[slot]) {
            swap_manager.swap_used[slot] = true;
            swap_manager.free_count--;
            swap_next_hint = slot + 1;
            *slot_out = slot;
            return true;
        }
    }
    return false;
}

void swap_release_slot(uint32_t slot) {
    if (slot >= swap_slot_count || !swap_manager.swap_used[slot])
        return;
    swap_manager.swap_used[slot] = false;
    swap_manager.free_count++;
}

static uint32_t slot_lba(uint32_t slot) {
    return swap_start_lba + slot * SWAP_SECTORS_PER_PAGE;
}

bool swap_out_page(struct PageDirectory *page_dir, uint32_t virtual_addr) {
    uint32_t *pte = paging_get_pte(page_dir, virtual_addr, false);
    if (pte == NULL || !(*pte & PTE_PRESENT))
        return false;
    uint32_t slot;
    if (!slot_alloc(&slot))
        return false;
    uint32_t frame = *pte & PTE_ADDRESS_MASK;
    // 1. Save the page to the swap partition (the frame is read through the physmap)
    if (!ata_write(swap_drive, PHYS_TO_VIRT(frame), slot_lba(slot), SWAP_SECTORS_PER_PAGE)) {
        swap_release_slot(slot);
        return false;
    }
    // 2. Record the slot in the PTE and 3. mark the page as not present
    *pte = (slot << 12) | PTE_SWAPPED | (*pte & (PTE_WRITABLE | PTE_USER));
    flush_single_tlb((void*) virtual_addr);
    // 4. Free the physical frame
    paging_frame_free(frame);
    page_manager_state.user_page_count--;
    stats.swap_outs++;
    return true;
}

void swap_out(struct PageDirectory *page_dir, void *virtual_addr) {
    swap_out_page(page_dir, (uint32_t) virtual_addr & PTE_ADDRESS_MASK);
}

bool swap_in(struct PageDirectory *page_dir, void *virtual_addr) {
    uint32_t page = (uint32_t) virtual_addr & PTE_ADDRESS_MASK;
    uint32_t *pte = paging_get_pte(page_dir, page, false);
    if (pte == NULL || (*pte & PTE_PRESENT) || !(*pte & PTE_SWAPPED))
        return false;
    uint32_t slot  = *pte >> 12;
    uint32_t flags = *pte & (PTE_WRITABLE | PTE_USER);
    // 1. Provide a physical frame (may itself evict another page)
    uint32_t frame = paging_frame_alloc();
    if (frame == 0)
        return false;
    // 2. Read the page back from swap
    if (!ata_read(swap_drive, PHYS_TO_VIRT(frame), slot_lba(slot), SWAP_SECTORS_PER_PAGE)) {
        paging_frame_free(frame);
        return false;
    }
    // 3. Make the page reachable again and release its slot
    *pte = frame | PTE_PRESENT | PTE_ACCESSED | flags; // ACCESSED: second chance, see paging.c
    flush_single_tlb((void*) page);
    swap_release_slot(slot);
    page_manager_state.user_page_count++;
    stats.swap_ins++;
    return true;
}

bool swap_reclaim_frame(void) {
    if (swap_slot_count == 0 || swap_manager.free_count == 0)
        return false;
    struct PageDirectory *victim_dir;
    uint32_t victim_virtual;
    if (!paging_choose_victim(&victim_dir, &victim_virtual))
        return false;
    return swap_out_page(victim_dir, victim_virtual);
}

bool handle_page_fault(uint32_t error_code) {
    uint32_t address = read_cr2();
    stats.page_faults++;
    last_fault_oom = false;
    // Bit 0 = protection violation on a present page (e.g. ring 3 touching kernel memory):
    // never fixable. Otherwise the page is simply not present.
    if ((error_code & 0x1) || address >= USER_SPACE_LIMIT || !user_address_is_valid(address)) {
        stats.failed++;
        return false;
    }
    struct PageDirectory *page_dir = paging_get_current_page_directory_addr();
    uint32_t page = address & PTE_ADDRESS_MASK;
    uint32_t *pte = paging_get_pte(page_dir, page, true);
    if (pte == NULL) {
        stats.failed++;
        last_fault_oom = true; // no frame for the page table
        return false;
    }
    if (*pte & PTE_PRESENT) {
        flush_single_tlb((void*) page); // stale TLB entry
        return true;
    }
    if (*pte & PTE_SWAPPED) {
        if (swap_in(page_dir, (void*) page))
            return true;
        stats.failed++;
        last_fault_oom = true;
        return false;
    }
    // Never touched before: demand paging maps a fresh zero page
    if (!paging_allocate_user_page_frame(page_dir, (void*) page)) {
        stats.failed++;
        last_fault_oom = true; // RAM full and nothing could be swapped out
        return false;
    }
    stats.demand_zero++;
    return true;
}

bool swap_last_fault_was_oom(void) {
    return last_fault_oom;
}

void swap_get_stats(struct SwapStats *out) {
    *out = stats;
    out->used_slots = swap_slot_count - swap_manager.free_count;
}
