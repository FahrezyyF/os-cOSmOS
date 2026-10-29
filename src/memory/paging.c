#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/memory/paging.h"
#include "header/memory/swap.h"
#include "header/stdlib/string.h"
#include "header/kernel-entrypoint.h"

_Static_assert(sizeof(struct PageDirectoryEntryFlag) == 1, "PDE flags are the low 8 bits");
_Static_assert(sizeof(struct PageTableEntryFlag) == 1, "PTE flags are the low 8 bits");
_Static_assert(sizeof(struct PageDirectoryEntry) == 4, "PDE must be 32-bit");
_Static_assert(sizeof(struct PageTableEntry) == 4, "PTE must be 32-bit");
_Static_assert(sizeof(struct PageDirectory) == PAGE_FRAME_SIZE, "page directory fills one frame");
_Static_assert(SYSTEM_MEMORY_MB >= 5, "user pages live above the 4 MiB kernel region: SYSTEM_MEMORY_MB must be >= 5");

#define PDE_INDEX(addr)     ((uint32_t) (addr) >> 22)
#define PTE_INDEX(addr)     (((uint32_t) (addr) >> 12) & 0x3FFu)
#define KERNEL_PDE_START    PDE_INDEX(KERNEL_VIRTUAL_ADDRESS_BASE) // 768
#define BOOTSTRAP_PDE       1022u                                  // 0xFF800000, used once during init
#define BOOTSTRAP_ADDRESS   (BOOTSTRAP_PDE << 22)
#define ADDRESS_SPACE_MAX   32u

/**
 * _paging_kernel_page_directory and _paging_kernel_page_table is initialized on kernel-entrypoint.s
 */
__attribute__((aligned(0x1000))) struct PageDirectory _paging_kernel_page_directory;

__attribute__((aligned(0x1000))) struct PageTable _paging_kernel_page_table;

// One page table inside the kernel image, used to reach the first physmap tables during init
static __attribute__((aligned(0x1000))) struct PageTable bootstrap_table;

/**
 * Global page frame manager state tracking free/busy frames.
 * (Filled by paging_init(): the kit's static initializer put the whole 32 KiB map in .data.)
 */
struct PageManagerState page_manager_state;

static uint32_t physmap_end = KERNEL_VIRTUAL_ADDRESS_BASE + 0x400000u;

/**
 * Physical frames:
 *   [0, kernel_pool_start)        kernel image (loaded at 1 MiB; BIOS, VGA, GRUB data below)
 *   [kernel_pool_start, 4 MiB)    kernel pool: the unused tail of the kit's reserved 4 MiB, only
 *                                 for kernel structures (page tables, kernel heap)
 *   [4 MiB, total)                frames managed for user pages (Book 3.1.4: 0x400000 -
 *                                 0x7FE0000 with QEMU's default 128 MiB); page_manager_state
 *                                 counts this region
 */
static uint32_t kernel_pool_start = KERNEL_RESERVED_PAGE_FRAME_COUNT;
static uint32_t kernel_pool_free  = 0;
static struct PageDirectory *address_spaces[ADDRESS_SPACE_MAX];
static uint32_t address_space_count = 0;

// Clock hand of the page replacement algorithm
static struct {
    uint32_t space;
    uint32_t pde;
    uint32_t pte;
} clock_hand;

static inline uint32_t *raw_directory(struct PageDirectory *page_dir) {
    return (uint32_t*) page_dir->table;
}

static inline uint32_t *raw_table(uint32_t pde) {
    return (uint32_t*) PHYS_TO_VIRT(pde & PTE_ADDRESS_MASK);
}

static void reload_cr3(void) {
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) : : "memory");
}

void flush_single_tlb(void *virtual_addr) {
    __asm__ volatile("invlpg (%0)" : /* <Empty> */ : "b"(virtual_addr): "memory");
}

struct PageDirectory *paging_get_current_page_directory_addr(void) {
    uint32_t current_page_directory_phys_addr;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_page_directory_phys_addr): /* <Empty> */);
    return (struct PageDirectory*) PHYS_TO_VIRT(current_page_directory_phys_addr);
}

uint32_t paging_kernel_image_frames(void) {
    return kernel_pool_start;
}

uint32_t paging_kernel_pool_free(void) {
    return kernel_pool_free;
}

uint32_t paging_page_directory_physical(struct PageDirectory *page_dir) {
    return (uint32_t) page_dir - KERNEL_VIRTUAL_ADDRESS_BASE;
}

/* ================================ PHYSICAL FRAMES ================================== */

// Next fit over the user region [4 MiB, total)
static uint32_t frame_alloc_raw(void) {
    struct PageManagerState *s = &page_manager_state;
    if (s->free_page_frame_count == 0)
        return 0;
    uint32_t span = s->total_page_frame_count - KERNEL_RESERVED_PAGE_FRAME_COUNT;
    for (uint32_t k = 0; k < span; k++) {
        uint32_t frame = s->next_fit_hint + k;
        if (frame >= s->total_page_frame_count)
            frame -= span;
        if (!s->page_frame_map[frame]) {
            s->page_frame_map[frame] = true;
            s->free_page_frame_count--;
            s->next_fit_hint = frame + 1 < s->total_page_frame_count ? frame + 1 : KERNEL_RESERVED_PAGE_FRAME_COUNT;
            return frame * PAGE_FRAME_SIZE;
        }
    }
    return 0;
}

static uint32_t kernel_pool_alloc(void) {
    if (kernel_pool_free == 0)
        return 0;
    for (uint32_t frame = kernel_pool_start; frame < KERNEL_RESERVED_PAGE_FRAME_COUNT; frame++) {
        if (!page_manager_state.page_frame_map[frame]) {
            page_manager_state.page_frame_map[frame] = true;
            kernel_pool_free--;
            return frame * PAGE_FRAME_SIZE;
        }
    }
    return 0;
}

uint32_t paging_frame_alloc(void) {
    uint32_t physical = frame_alloc_raw();
    // RAM is full: page replacement frees a frame by swapping a user page out
    if (physical == 0 && swap_reclaim_frame())
        physical = frame_alloc_raw();
    if (physical != 0)
        memset(PHYS_TO_VIRT(physical), 0, PAGE_FRAME_SIZE);
    return physical;
}

uint32_t paging_frame_alloc_kernel(void) {
    uint32_t physical = kernel_pool_alloc();
    if (physical == 0)
        return paging_frame_alloc(); // kernel pool used up: share the user region
    memset(PHYS_TO_VIRT(physical), 0, PAGE_FRAME_SIZE);
    return physical;
}

void paging_frame_free(uint32_t physical_addr) {
    uint32_t frame = physical_addr / PAGE_FRAME_SIZE;
    struct PageManagerState *s = &page_manager_state;
    if (frame < kernel_pool_start || frame >= s->total_page_frame_count || !s->page_frame_map[frame])
        return;
    s->page_frame_map[frame] = false;
    if (frame < KERNEL_RESERVED_PAGE_FRAME_COUNT)
        kernel_pool_free++;
    else
        s->free_page_frame_count++;
}

// Page table frames during paging_init(): the kernel pool first (the physmap does not exist yet,
// frames above 4 MiB are only reachable through the bootstrap window)
static uint32_t init_table_frame(void) {
    uint32_t physical = kernel_pool_alloc();
    return physical != 0 ? physical : frame_alloc_raw();
}

/* ================================ INITIALIZATION =================================== */

// Write a kernel PDE (supervisor, writable) and the PTEs of its table through the bootstrap window
static void install_physmap_table(uint32_t table_phys, uint32_t pde_index, uint32_t total_frames) {
    uint32_t *bootstrap = (uint32_t*) bootstrap_table.table;
    bootstrap[0] = table_phys | PTE_PRESENT | PTE_WRITABLE;
    flush_single_tlb((void*) BOOTSTRAP_ADDRESS);

    uint32_t *table = (uint32_t*) BOOTSTRAP_ADDRESS;
    uint32_t first_frame = (pde_index - KERNEL_PDE_START) * PAGE_ENTRY_COUNT;
    for (uint32_t i = 0; i < PAGE_ENTRY_COUNT; i++) {
        uint32_t frame = first_frame + i;
        table[i] = frame < total_frames ? (frame * PAGE_FRAME_SIZE) | PTE_PRESENT | PTE_WRITABLE : 0;
    }
    raw_directory(&_paging_kernel_page_directory)[pde_index] = table_phys | PTE_PRESENT | PTE_WRITABLE;
}

void paging_init(uint32_t detected_ram_kb) {
    struct PageManagerState *s = &page_manager_state;
    uint32_t total = PAGE_FRAME_MAX_COUNT;
    if (detected_ram_kb != 0 && detected_ram_kb / 4 < total)
        total = detected_ram_kb / 4;
    if (total > KERNEL_PHYSMAP_MAX_SIZE / PAGE_FRAME_SIZE)
        total = KERNEL_PHYSMAP_MAX_SIZE / PAGE_FRAME_SIZE;
    if (total < KERNEL_RESERVED_PAGE_FRAME_COUNT)
        total = KERNEL_RESERVED_PAGE_FRAME_COUNT;

    // The kit reserves the first 4 MiB for the kernel. The image only needs the part up to
    // _linker_kernel_physical_addr_end; the tail becomes the kernel pool (see top of file)
    uint32_t image_end = (uint32_t) &_linker_kernel_physical_addr_end;
    kernel_pool_start = (image_end + PAGE_FRAME_SIZE - 1) / PAGE_FRAME_SIZE;
    if (kernel_pool_start > KERNEL_RESERVED_PAGE_FRAME_COUNT)
        kernel_pool_start = KERNEL_RESERVED_PAGE_FRAME_COUNT;
    kernel_pool_free = KERNEL_RESERVED_PAGE_FRAME_COUNT - kernel_pool_start;

    for (uint32_t i = 0; i < PAGE_FRAME_MAX_COUNT; i++)
        s->page_frame_map[i] = i < kernel_pool_start || i >= total;
    s->total_page_frame_count = total;
    s->free_page_frame_count  = total - KERNEL_RESERVED_PAGE_FRAME_COUNT;
    s->next_fit_hint          = KERNEL_RESERVED_PAGE_FRAME_COUNT;
    s->user_page_count        = 0;
    s->page_table_count       = 0;
    s->kernel_heap_pages      = 0;

    // 1. physmap: map every frame above 4 MiB at 0xC0000000 + physical address
    uint32_t *directory = raw_directory(&_paging_kernel_page_directory);
    directory[BOOTSTRAP_PDE] = ((uint32_t) &bootstrap_table - KERNEL_VIRTUAL_ADDRESS_BASE) | PTE_PRESENT | PTE_WRITABLE;
    uint32_t physmap_tables = (total - KERNEL_RESERVED_PAGE_FRAME_COUNT + PAGE_ENTRY_COUNT - 1) / PAGE_ENTRY_COUNT;
    for (uint32_t k = 0; k < physmap_tables; k++) {
        uint32_t table_phys = init_table_frame();
        install_physmap_table(table_phys, KERNEL_PDE_START + 1 + k, total);
        s->page_table_count++;
    }
    directory[BOOTSTRAP_PDE] = 0;
    reload_cr3();
    physmap_end = KERNEL_VIRTUAL_ADDRESS_BASE + total * PAGE_FRAME_SIZE;

    // 2. kernel heap page tables, installed up front so the kernel half never changes later
    for (uint32_t pde = PDE_INDEX(KERNEL_HEAP_BASE); pde < PDE_INDEX(KERNEL_HEAP_LIMIT); pde++) {
        uint32_t table_phys = init_table_frame();
        memset(PHYS_TO_VIRT(table_phys), 0, PAGE_FRAME_SIZE);
        directory[pde] = table_phys | PTE_PRESENT | PTE_WRITABLE;
        s->page_table_count++;
    }
    reload_cr3();

    address_space_count = 0;
    paging_register_address_space(&_paging_kernel_page_directory);
}

void paging_register_address_space(struct PageDirectory *page_dir) {
    for (uint32_t i = 0; i < address_space_count; i++)
        if (address_spaces[i] == page_dir)
            return;
    if (address_space_count < ADDRESS_SPACE_MAX)
        address_spaces[address_space_count++] = page_dir;
}

void paging_unregister_address_space(struct PageDirectory *page_dir) {
    for (uint32_t i = 0; i < address_space_count; i++) {
        if (address_spaces[i] == page_dir) {
            address_spaces[i] = address_spaces[--address_space_count];
            clock_hand.space = 0;
            return;
        }
    }
}

/* ================================ PAGE TABLE ACCESS ================================ */

uint32_t *paging_get_pte(struct PageDirectory *page_dir, uint32_t virtual_addr, bool create) {
    uint32_t pde_index = PDE_INDEX(virtual_addr);
    uint32_t *pde = &raw_directory(page_dir)[pde_index];
    if (!(*pde & PTE_PRESENT)) {
        if (!create || pde_index >= KERNEL_PDE_START)
            return NULL; // the kernel half is fully installed by paging_init()
        uint32_t table_phys = paging_frame_alloc_kernel(); // page tables are kernel structures
        if (table_phys == 0)
            return NULL;
        *pde = table_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        page_manager_state.page_table_count++;
    }
    return &raw_table(*pde)[PTE_INDEX(virtual_addr)];
}

struct PageTable *paging_get_page_table(struct PageDirectory *page_dir, uint32_t page_directory_index) {
    if (page_directory_index >= PAGE_ENTRY_COUNT)
        return NULL;
    uint32_t *pte = paging_get_pte(page_dir, page_directory_index << 22, true);
    if (pte == NULL)
        return NULL;
    return (struct PageTable*) raw_table(raw_directory(page_dir)[page_directory_index]);
}

void update_page_directory_entry(
    struct PageDirectory *page_dir,
    void *physical_addr,
    void *virtual_addr,
    struct PageTableEntryFlag flag
) {
    uint32_t page_directory_index = PDE_INDEX(virtual_addr);
    uint32_t page_table_index     = PTE_INDEX(virtual_addr);
    struct PageTable *page_table = paging_get_page_table(page_dir, page_directory_index);
    if (page_table == NULL) return;
    page_table->table[page_table_index].flag = flag;
    page_table->table[page_table_index].physical_page_base_address = ((uint32_t) physical_addr >> 12);
    flush_single_tlb(virtual_addr);
}

uint32_t paging_virtual_to_physical(struct PageDirectory *page_dir, uint32_t virtual_addr) {
    if (virtual_addr >= KERNEL_VIRTUAL_ADDRESS_BASE && virtual_addr < physmap_end)
        return virtual_addr - KERNEL_VIRTUAL_ADDRESS_BASE;
    uint32_t pde = raw_directory(page_dir)[PDE_INDEX(virtual_addr)];
    if (!(pde & PTE_PRESENT))
        return 0;
    uint32_t pte = raw_table(pde)[PTE_INDEX(virtual_addr)];
    if (!(pte & PTE_PRESENT))
        return 0;
    return (pte & PTE_ADDRESS_MASK) | (virtual_addr & (PAGE_FRAME_SIZE - 1));
}

/* ================================ KERNEL MAPPINGS ================================== */

bool paging_map_kernel_physical(uint32_t virtual_addr, uint32_t physical_addr, uint32_t size) {
    uint32_t *directory = raw_directory(&_paging_kernel_page_directory);
    uint32_t pages = (size + PAGE_FRAME_SIZE - 1) / PAGE_FRAME_SIZE;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t v = virtual_addr + i * PAGE_FRAME_SIZE;
        uint32_t *pde = &directory[PDE_INDEX(v)];
        if (!(*pde & PTE_PRESENT)) {
            uint32_t table_phys = paging_frame_alloc_kernel();
            if (table_phys == 0)
                return false;
            *pde = table_phys | PTE_PRESENT | PTE_WRITABLE;
            page_manager_state.page_table_count++;
        }
        raw_table(*pde)[PTE_INDEX(v)] = ((physical_addr + i * PAGE_FRAME_SIZE) & PTE_ADDRESS_MASK) | PTE_PRESENT | PTE_WRITABLE;
        flush_single_tlb((void*) v);
    }
    return true;
}

bool paging_map_kernel_heap(uint32_t virtual_addr, uint32_t size) {
    uint32_t *directory = raw_directory(&_paging_kernel_page_directory);
    for (uint32_t v = virtual_addr; v < virtual_addr + size; v += PAGE_FRAME_SIZE) {
        if (v < KERNEL_HEAP_BASE || v >= KERNEL_HEAP_LIMIT)
            return false;
        uint32_t *pte = &raw_table(directory[PDE_INDEX(v)])[PTE_INDEX(v)];
        if (*pte & PTE_PRESENT)
            continue;
        uint32_t frame = paging_frame_alloc_kernel();
        if (frame == 0)
            return false;
        *pte = frame | PTE_PRESENT | PTE_WRITABLE;
        flush_single_tlb((void*) v);
        page_manager_state.kernel_heap_pages++;
    }
    return true;
}

/* ================================ USER MEMORY ====================================== */

bool paging_allocate_check(uint32_t amount) {
    // Swap space backs user pages too, so it counts as available memory. A small reserve is
    // never promised away: page tables, the stack and swapping code pages back in need it.
    uint32_t required_frames = (amount + PAGE_FRAME_SIZE - 1) / PAGE_FRAME_SIZE;
    uint32_t available = page_manager_state.free_page_frame_count + swap_manager.free_count;
    return available >= PAGING_COMMIT_RESERVE_PAGES && required_frames <= available - PAGING_COMMIT_RESERVE_PAGES;
}

bool paging_allocate_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr) {
    uint32_t page = (uint32_t) virtual_addr & PTE_ADDRESS_MASK;
    if (page >= USER_SPACE_LIMIT)
        return false;
    uint32_t *pte = paging_get_pte(page_dir, page, true);
    if (pte == NULL)
        return false;
    if (*pte & PTE_PRESENT)
        return true;
    if (*pte & PTE_SWAPPED)
        return swap_in(page_dir, (void*) page);
    uint32_t frame = paging_frame_alloc();
    if (frame == 0)
        return false;
    // Present + writable + user (the kit TODO also said "pagesize 4 mb", which no longer applies)
    // ACCESSED is preset so the fresh page gets a second chance from the clock hand: an
    // instruction touching two pages must not evict the first while faulting in the second
    *pte = frame | PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_ACCESSED;
    flush_single_tlb((void*) page);
    page_manager_state.user_page_count++;
    return true;
}

bool paging_free_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr) {
    uint32_t page = (uint32_t) virtual_addr & PTE_ADDRESS_MASK;
    if (page >= USER_SPACE_LIMIT)
        return false;
    uint32_t *pte = paging_get_pte(page_dir, page, false);
    if (pte == NULL)
        return false;
    if (*pte & PTE_PRESENT) {
        paging_frame_free(*pte & PTE_ADDRESS_MASK);
        page_manager_state.user_page_count--;
        *pte = 0;
        flush_single_tlb((void*) page);
        return true;
    }
    if (*pte & PTE_SWAPPED) {
        swap_release_slot(*pte >> 12);
        *pte = 0;
        return true;
    }
    return false;
}

void paging_free_user_space(struct PageDirectory *page_dir) {
    uint32_t *directory = raw_directory(page_dir);
    for (uint32_t pde_index = 0; pde_index < KERNEL_PDE_START; pde_index++) {
        uint32_t pde = directory[pde_index];
        if (!(pde & PTE_PRESENT))
            continue;
        uint32_t *table = raw_table(pde);
        for (uint32_t i = 0; i < PAGE_ENTRY_COUNT; i++) {
            if (table[i] & PTE_PRESENT) {
                paging_frame_free(table[i] & PTE_ADDRESS_MASK);
                page_manager_state.user_page_count--;
            } else if (table[i] & PTE_SWAPPED) {
                swap_release_slot(table[i] >> 12);
            }
        }
        paging_frame_free(pde & PTE_ADDRESS_MASK);
        page_manager_state.page_table_count--;
        directory[pde_index] = 0;
    }
    if (page_dir == paging_get_current_page_directory_addr())
        reload_cr3();
}

/* ================================ PAGE REPLACEMENT ================================= */

bool paging_choose_victim(struct PageDirectory **victim_dir, uint32_t *victim_virtual) {
    if (address_space_count == 0 || page_manager_state.user_page_count == 0)
        return false;
    struct PageDirectory *current = paging_get_current_page_directory_addr();
    // Two sweeps are enough: the first clears accessed bits, the second finds a victim
    uint32_t examine_limit = 2 * page_manager_state.user_page_count + 2;
    uint32_t steps_limit   = 2 * address_space_count * KERNEL_PDE_START * PAGE_ENTRY_COUNT;
    uint32_t examined = 0;
    for (uint32_t step = 0; step < steps_limit && examined < examine_limit; step++) {
        // advance the hand
        if (++clock_hand.pte >= PAGE_ENTRY_COUNT) {
            clock_hand.pte = 0;
            if (++clock_hand.pde >= KERNEL_PDE_START) {
                clock_hand.pde = 0;
                if (++clock_hand.space >= address_space_count)
                    clock_hand.space = 0;
            }
        }
        if (clock_hand.space >= address_space_count)
            clock_hand.space = 0;
        struct PageDirectory *dir = address_spaces[clock_hand.space];
        uint32_t pde = raw_directory(dir)[clock_hand.pde];
        if (!(pde & PTE_PRESENT)) {
            clock_hand.pte = PAGE_ENTRY_COUNT - 1; // skip the whole empty 4 MiB region
            step += PAGE_ENTRY_COUNT - 1;
            continue;
        }
        uint32_t *entry = &raw_table(pde)[clock_hand.pte];
        if (!(*entry & PTE_PRESENT))
            continue;
        examined++;
        uint32_t virtual_addr = (clock_hand.pde << 22) | (clock_hand.pte << 12);
        if (*entry & PTE_ACCESSED) {
            *entry &= ~PTE_ACCESSED; // second chance
            if (dir == current)
                flush_single_tlb((void*) virtual_addr);
            continue;
        }
        *victim_dir     = dir;
        *victim_virtual = virtual_addr;
        return true;
    }
    return false;
}
