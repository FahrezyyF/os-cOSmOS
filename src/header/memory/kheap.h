#ifndef _KHEAP_H
#define _KHEAP_H

#include <stddef.h>
#include "header/stdlib/heap.h"

/**
 * Kernel heap (bonus: HEAP Memory). Arena at KERNEL_HEAP_BASE that grows by mapping
 * fresh frames (paging_map_kernel_heap), managed by the shared allocator in heap.c.
 */
void  kheap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);
void  kheap_get_stats(struct HeapStats *stats);
bool  kheap_check(void);

#endif
