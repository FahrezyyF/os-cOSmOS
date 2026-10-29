#ifndef _PAGING_H
#define _PAGING_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * Note: MB often referring to MiB in context of memory management
 * change this to be a small number to test if swap working properly
 * kernel will be tested with a small number of SYSTEM_MEMORY_MB
 * (also overridable at build time: `make run SYSTEM_MEMORY_MB=6`)
 */
#ifndef SYSTEM_MEMORY_MB
#define SYSTEM_MEMORY_MB     128
#endif

// Number of entries in a page directory
#define PAGE_ENTRY_COUNT     1024

// Page Frame (PF) Size: 4 KiB
#define PAGE_FRAME_SIZE      0x1000u

#define KERNEL_RESERVED_PAGE_FRAME_COUNT (0x400000u / PAGE_FRAME_SIZE)
#define RECURSIVE_PAGE_TABLES_VIRTUAL_ADDRESS 0xFFC00000u
#define RECURSIVE_PAGE_DIRECTORY_INDEX 1023u

// Maximum usable page frame. Default count: 128 MiB / 4 KiB = 32768 frames
#define PAGE_FRAME_MAX_COUNT ((SYSTEM_MEMORY_MB << 20) / PAGE_FRAME_SIZE)

/* -- cOSmOS virtual memory layout (see README) -- */
#define KERNEL_VIRTUAL_ADDRESS_BASE   0xC0000000u // physmap: virtual = physical + 0xC0000000 for all RAM
#define KERNEL_PHYSMAP_MAX_SIZE       0x20000000u // (first 4 MiB = kernel image, mapped by the kit table)
#define KERNEL_HEAP_BASE              0xE0000000u // kmalloc() arena (16 MiB of virtual space)
#define KERNEL_HEAP_LIMIT             0xE1000000u
#define KERNEL_LFB_BASE               0xF0000000u // graphics linear framebuffer (up to 8 MiB)
#define KERNEL_LFB_MAX_SIZE           0x800000u

// Kernel-virtual address of a physical frame (valid for every frame of the pool)
#define PHYS_TO_VIRT(addr) ((void*) ((uint32_t) (addr) + KERNEL_VIRTUAL_ADDRESS_BASE))

#define USER_SPACE_LIMIT              0xC0000000u // user addresses are below the kernel
#define USER_PROGRAM_BASE             0x00000000u // flat binary is loaded here (book 3.3.5)
#define USER_PROGRAM_MAX_SIZE         0x00100000u // 1 MiB (book 3.4.2.2)
#define USER_STACK_TOP                0x00400000u // kernel_execute_user_program() sets esp here
#define USER_HEAP_BASE                0x00400000u // sbrk() heap grows up from here
#define USER_HEAP_LIMIT               0x40000000u // 1 GiB of virtual heap (backed by RAM + swap)
#define PAGING_COMMIT_RESERVE_PAGES   64u         // RAM + swap kept free by paging_allocate_check()

// kernel's  page directory, using 4 KiB pages
extern struct PageDirectory _paging_kernel_page_directory;
extern struct PageTable _paging_kernel_page_table;

/**
 * Page Directory Entry Flag, only first 8 bit
 * @param present_bit        Indicate whether this entry is exist or not.
 * @param write_bit          Indicate whether this page is writable
 * @param us_bit             User/supervisor: 1 = ring 3 may access the 4 MiB region
 * @param pwt_bit            Page-level write-through
 * @param pcd_bit            Page-level cache disable
 * @param accessed_bit       Set by the MMU when the entry is used for a translation
 * @param ignored_bit        Ignored for 4 KiB page tables
 * @param use_pagesize_4_mb Indicate whether this entry use 4 MB page size (set false)
 * References: Intel Manual 3a - Ch 4 Paging - Figure 4-4 PDE: 4KB page
 */
struct PageDirectoryEntryFlag {
    uint32_t present_bit        : 1;
    uint32_t write_bit          : 1;
    uint32_t us_bit             : 1;
    uint32_t pwt_bit            : 1;
    uint32_t pcd_bit            : 1;
    uint32_t accessed_bit       : 1;
    uint32_t ignored_bit        : 1;
    uint32_t use_pagesize_4_mb  : 1;
} __attribute__((packed));

/**
 * Page Table Entry Flag, only first 8 bit
 * @param present_bit        Indicate whether this entry is exist or not.
 * @param write_bit          Indicate whether this page is writable
 * @param us_bit             User/supervisor: 1 = ring 3 may access the page
 * @param pwt_bit            Page-level write-through
 * @param pcd_bit            Page-level cache disable
 * @param accessed_bit       Set by the MMU on every access (used by the page replacement clock)
 * @param dirty_bit          Set by the MMU on every write
 * @param pat_bit            Page attribute table
 * References: Intel Manual 3a - Ch 4 Paging - Figure 4-4 PTE: 4KB page
 */
struct PageTableEntryFlag {
    uint32_t present_bit        : 1;
    uint32_t write_bit          : 1;
    uint32_t us_bit             : 1;
    uint32_t pwt_bit            : 1;
    uint32_t pcd_bit            : 1;
    uint32_t accessed_bit       : 1;
    uint32_t dirty_bit          : 1;
    uint32_t pat_bit            : 1;
} __attribute__((packed));

/**
 * Page Directory Entry, for page size 4 KB.
 * can also be used as Page Table Entry
 * References: Intel Manual 3a - Ch 4 Paging - Figure 4-4 PDE: 4KB page
 *
 * @param flag                    Contain 8-bit page directory entry flag
 * @param global_page             G flag (ignored when the entry points to a page table)
 * @param ignored                 Ignored bit (3-bit), available to the OS
 * @param page_table_base_address Bits 31:12 of the page table physical address (20-bit)
 */
struct PageDirectoryEntry {
    struct PageDirectoryEntryFlag flag;
    uint16_t global_page             : 1;
    uint16_t ignored                 : 3;
    uint32_t page_table_base_address : 20;
}__attribute__((packed));

/**
 * Page Table Entry, for page size 4 KB.
 * References: Intel Manual 3a - Ch 4 Paging - Figure 4-4 PTE: 4KB page
 *
 * @param flag                       Contain 8-bit page table entry flag
 * @param global_page                G flag
 * @param ignored                    Ignored bit (3-bit), available to the OS
 * @param physical_page_base_address Bits 31:12 of the page frame physical address (20-bit)
 */
struct PageTableEntry {
    struct PageTableEntryFlag flag;
    uint16_t global_page                : 1;
    uint16_t ignored                    : 3;
    uint32_t physical_page_base_address : 20;
}__attribute__((packed));

/**
 * Page Directory, contain array of PageDirectoryEntry.
 * Note: This data structure is volatile (can be modified from outside this code, check "C volatile keyword").
 * MMU operation, TLB hit & miss also affecting this data structure (dirty, accessed bit, etc).
 *
 * Warning: Address must be aligned in 4 KB (listed on Intel Manual), use __attribute__((aligned(0x1000))),
 * unaligned definition of PageDirectory will cause triple fault
 *
 * @param table Fixed-width array of PageDirectoryEntry with size PAGE_ENTRY_COUNT
 */
struct PageDirectory {
    struct PageDirectoryEntry table[PAGE_ENTRY_COUNT];
}__attribute__((packed));

/**
 *  Page Table, contain array of PageTableEntry
 */
struct PageTable {
    struct PageTableEntry table[PAGE_ENTRY_COUNT];
}__attribute__((packed));

/**
 * Containing page manager states.
 *
 * @param page_frame_map        Keeping track empty space. True when the page frame is currently used
 * @param free_page_frame_count Number of free frames (user pages, page tables and kernel heap all come from here)
 * @param total_page_frame_count Frames that physically exist: min(SYSTEM_MEMORY_MB, RAM reported by GRUB)
 * @param next_fit_hint         Next-fit allocation cursor
 * @param user_page_count       Frames currently holding user pages (candidates for swap out)
 * @param page_table_count      Frames used for page tables (user tables + physmap / heap tables)
 * @param kernel_heap_pages     Frames mapped into the kernel heap
 */
struct PageManagerState {
    bool     page_frame_map[PAGE_FRAME_MAX_COUNT];
    uint32_t free_page_frame_count;
    uint32_t total_page_frame_count;
    uint32_t next_fit_hint;
    uint32_t user_page_count;
    uint32_t page_table_count;
    uint32_t kernel_heap_pages;
} __attribute__((packed));

extern struct PageManagerState page_manager_state;

/**
 * Raw view of a page table entry. When the present bit is 0 the remaining 31 bits are
 * ignored by the MMU, cOSmOS stores the swap slot of a swapped-out page there (swap.h).
 */
#define PTE_PRESENT      0x001u
#define PTE_WRITABLE     0x002u
#define PTE_USER         0x004u
#define PTE_ACCESSED     0x020u
#define PTE_DIRTY        0x040u
#define PTE_SWAPPED      0x200u   // OS-available bit 9: page lives in the swap partition
#define PTE_ADDRESS_MASK 0xFFFFF000u



/**
 * Edit page directory with respective parameter
 *
 * @param page_dir      Page directory to update
 * @param physical_addr Physical address to map
 * @param virtual_addr  Virtual address to map
 * @param flag          Page entry flags
 */
void update_page_directory_entry(
    struct PageDirectory *page_dir,
    void *physical_addr,
    void *virtual_addr,
    struct PageTableEntryFlag flag
);

/**
 * Get the requested page table from respective parameter, allocating it on demand.
 * (Kit note: the kit implemented this with fractal / recursive mapping. cOSmOS keeps every
 * page table inside the kernel page pool instead, so tables of ANY page directory can be
 * edited, not only the currently loaded one - required for swapping and for Chapter 4.)
 *
 * @param page_dir             Page directory to read
 * @param page_directory_index Page table's position/index on page directory
 * @return Kernel-virtual pointer to the page table, NULL if it cannot be allocated
 */
struct PageTable *paging_get_page_table(struct PageDirectory *page_dir, uint32_t page_directory_index);

/**
 * Invalidate page that contain virtual address in parameter
 *
 * @param virtual_addr Virtual address to flush
 */
void flush_single_tlb(void *virtual_addr);

// Physical address of the currently loaded page directory as a kernel virtual pointer
struct PageDirectory *paging_get_current_page_directory_addr(void);




/* --- Memory Management --- */
/**
 * Check whether a certain amount of physical memory is available
 *
 * @param amount Requested amount of physical memory in bytes
 * @return       Return true when there's enough free memory available
 */
bool paging_allocate_check(uint32_t amount);

/**
 * Allocate single user page frame in page directory
 *
 * @param page_dir     Page directory to update
 * @param virtual_addr Virtual address to be allocated
 * @return             True when a zero-filled frame is mapped (user, writable)
 */
bool paging_allocate_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr);

/**
 * Deallocate single user page frame in page directory
 *
 * @param page_dir      Page directory to update
 * @param virtual_addr  Virtual address to be allocated
 * @return              Will return true if success, false otherwise
 */
bool paging_free_user_page_frame(struct PageDirectory *page_dir, void *virtual_addr);

/* --- cOSmOS extensions --- */

/**
 * Finish paging setup after kernel-entrypoint.s enabled it: size the frame pool from
 * SYSTEM_MEMORY_MB and the RAM reported by GRUB, map all of that RAM at
 * 0xC0000000 + physical (physmap) and pre-install the kernel heap page tables, so the
 * kernel half of every page directory never changes afterwards (Chapter 4 copies it).
 *
 * @param detected_ram_kb Total RAM from multiboot (mem_upper + 1024 KiB), 0 = unknown
 */
void paging_init(uint32_t detected_ram_kb);

/**
 * Register a page directory whose user pages may be chosen for swap out
 * (M3: the kernel page directory shared with the shell; M4: every process)
 */
void paging_register_address_space(struct PageDirectory *page_dir);
void paging_unregister_address_space(struct PageDirectory *page_dir);

/**
 * Allocate one zeroed physical frame for a user page from the region above 4 MiB (next fit).
 * When RAM is full, a user page is swapped out first (page replacement).
 * @return physical address, 0 when nothing can be freed
 */
uint32_t paging_frame_alloc(void);

// Frame for a kernel structure (page table, kernel heap): the kernel pool inside the reserved
// 4 MiB first, the user region when that is used up
uint32_t paging_frame_alloc_kernel(void);

// Return a frame to the pool
void paging_frame_free(uint32_t physical_addr);

/**
 * Raw PTE for a user virtual address.
 * @param create Allocate the page table when it does not exist yet
 * @return Pointer to the entry, NULL if the table is missing (and create == false) or out of memory
 */
uint32_t *paging_get_pte(struct PageDirectory *page_dir, uint32_t virtual_addr, bool create);

/**
 * Map physical memory into kernel space at a fixed virtual address (used for the
 * graphics linear framebuffer). Size is rounded up to whole pages.
 */
bool paging_map_kernel_physical(uint32_t virtual_addr, uint32_t physical_addr, uint32_t size);

/**
 * Grow the kernel heap region by mapping fresh frames at [virtual_addr, virtual_addr + size).
 * Used by kmalloc().
 */
bool paging_map_kernel_heap(uint32_t virtual_addr, uint32_t size);

/**
 * Release every user page, swap slot and user page table of a page directory
 * (used before (re)loading a user program).
 */
void paging_free_user_space(struct PageDirectory *page_dir);

/**
 * Walk the user page tables with a clock hand and pick a resident user page for
 * replacement (second chance: pages with the accessed bit set get their bit cleared
 * and are skipped once).
 * @param victim_virtual Virtual address of the chosen page
 * @return true if a victim was found
 */
bool paging_choose_victim(struct PageDirectory **victim_dir, uint32_t *victim_virtual);

// Virtual -> physical through a page directory, 0 if the page is not present
uint32_t paging_virtual_to_physical(struct PageDirectory *page_dir, uint32_t virtual_addr);

// Frames holding the kernel image (from physical 0), and free frames left in the kernel pool
uint32_t paging_kernel_image_frames(void);
uint32_t paging_kernel_pool_free(void);

// Physical address of a page directory (static in the image or in the physmap)
uint32_t paging_page_directory_physical(struct PageDirectory *page_dir);

#endif
