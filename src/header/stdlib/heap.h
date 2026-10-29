#ifndef _KERNEL_STDLIB_HEAP_H
#define _KERNEL_STDLIB_HEAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * General purpose heap allocator shared by the kernel (kmalloc) and user programs (malloc).
 *
 *  - Chunks carry an 8-byte header {size | flags, previous chunk size} ("boundary tags"),
 *    so both neighbours of a chunk are found in O(1).
 *  - Free chunks sit in 28 segregated, doubly linked free lists, one per power-of-two size
 *    class; allocation searches its own class (first fit) then any larger class (O(1) pick).
 *  - Splitting on allocation, immediate coalescing with both neighbours on free.
 *  - realloc() grows in place when the next chunk is free or is the top of the arena.
 *  - The arena grows on demand through a callback: the kernel maps new frames, user
 *    programs call the sbrk system call. Pages are demand-paged and can be swapped out.
 *  - Trimming: when the free chunk at the top grows past HEAP_TRIM_THRESHOLD, the excess is
 *    handed back through the optional shrink callback (user programs: sbrk(-n)).
 *  - Corruption checks: magic in every header, double free detection, heap_check() walks
 *    and validates the whole arena (used by the host unit tests).
 */

#define HEAP_ALIGNMENT   8u
#define HEAP_BIN_COUNT   28u
// header + the two free-list links: 16 bytes on i386 (24 when the host unit test runs on x86-64)
#define HEAP_MIN_CHUNK   ((uint32_t) ((8u + 2u * sizeof(void*) + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1)))
#define HEAP_TRIM_THRESHOLD (128u * 1024u)

struct HeapArena;

/**
 * Grow callback: make [arena->top, arena->top + bytes) usable memory.
 * @return true on success
 */
typedef bool (*HeapGrowFunction)(struct HeapArena *arena, uint32_t bytes);

/**
 * Shrink callback: [arena->top, arena->top + bytes) is no longer used (top already lowered).
 * @return true if the memory was released (false: the arena keeps it)
 */
typedef bool (*HeapShrinkFunction)(struct HeapArena *arena, uint32_t bytes);

struct HeapFreeChunk;

struct HeapArena {
    uint8_t              *base;        // first chunk
    uint8_t              *top;         // end of the committed region (chunks tile [base, top))
    uint8_t              *limit;       // the arena never grows past this address
    uint8_t              *last;        // chunk that ends at `top` (NULL while the arena is empty)
    uint32_t              grow_granularity;
    HeapGrowFunction      grow;
    HeapShrinkFunction    shrink;      // optional (NULL: never trim)
    struct HeapFreeChunk *bins[HEAP_BIN_COUNT];
    uint32_t              used_bytes;  // payload bytes handed out (chunk sizes)
    uint32_t              used_chunks;
    uint32_t              free_bytes;  // bytes in free chunks
    uint32_t              free_chunks;
    uint32_t              alloc_count; // statistics
    uint32_t              free_count;
    uint32_t              grow_count;
    uint32_t              trim_count;
    uint32_t              errors;      // invalid / double free attempts detected
};

struct HeapStats {
    uint32_t committed_bytes;
    uint32_t used_bytes;
    uint32_t used_chunks;
    uint32_t free_bytes;
    uint32_t free_chunks;
    uint32_t largest_free;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t grow_count;
    uint32_t trim_count;
    uint32_t errors;
};

/**
 * @param base             Start of the arena (rounded up to HEAP_ALIGNMENT)
 * @param limit            Highest address the arena may reach
 * @param grow_granularity Grow requests are rounded up to this many bytes (e.g. one page)
 * @param grow             Callback that commits memory at the current top
 */
void  heap_init(struct HeapArena *arena, void *base, void *limit, uint32_t grow_granularity, HeapGrowFunction grow);

// Enable trimming (see HEAP_TRIM_THRESHOLD)
void  heap_set_shrink(struct HeapArena *arena, HeapShrinkFunction shrink);
void *heap_alloc(struct HeapArena *arena, size_t size);
void *heap_calloc(struct HeapArena *arena, size_t count, size_t size);
void *heap_realloc(struct HeapArena *arena, void *ptr, size_t size);
void  heap_free(struct HeapArena *arena, void *ptr);

// Usable size of an allocation (>= requested size)
size_t heap_usable_size(void *ptr);

void heap_get_stats(struct HeapArena *arena, struct HeapStats *stats);

/**
 * Walk the arena and verify every invariant (headers, neighbour sizes, no two adjacent
 * free chunks, free-list membership and counters).
 * @return true when the heap is consistent
 */
bool heap_check(struct HeapArena *arena);

#endif
