#include "header/memory/kheap.h"
#include "header/memory/paging.h"

static struct HeapArena kernel_heap;

static bool kheap_grow(struct HeapArena *arena, uint32_t bytes) {
    return paging_map_kernel_heap((uint32_t) arena->top, bytes);
}

void kheap_init(void) {
    heap_init(&kernel_heap, (void*) KERNEL_HEAP_BASE, (void*) KERNEL_HEAP_LIMIT, PAGE_FRAME_SIZE, kheap_grow);
}

void *kmalloc(size_t size) {
    return heap_alloc(&kernel_heap, size);
}

void *kcalloc(size_t count, size_t size) {
    return heap_calloc(&kernel_heap, count, size);
}

void *krealloc(void *ptr, size_t size) {
    return heap_realloc(&kernel_heap, ptr, size);
}

void kfree(void *ptr) {
    heap_free(&kernel_heap, ptr);
}

void kheap_get_stats(struct HeapStats *stats) {
    heap_get_stats(&kernel_heap, stats);
}

bool kheap_check(void) {
    return heap_check(&kernel_heap);
}
