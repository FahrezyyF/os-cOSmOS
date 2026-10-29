// Host tests for src/stdlib/heap.c (kmalloc in the kernel, malloc in user programs):
// a randomized model test checks contents, alignment, overlap and heap_check() after every step.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "header/stdlib/heap.h"

static int failures = 0, checks = 0;

#define CHECK(cond, ...) do {                                    \
        checks++;                                                \
        if (!(cond)) {                                           \
            failures++;                                          \
            fprintf(stderr, "  FAIL line %d: ", __LINE__);       \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
        }                                                        \
    } while (0)

#define ARENA_SIZE (16u * 1024u * 1024u)

static uint8_t  *memory;          // backing store of the arena
static uint8_t  *committed_top;   // what the "kernel" has handed out so far
static uint32_t  grow_calls, shrink_calls;
static bool      grow_fails = false;

static bool test_grow(struct HeapArena *a, uint32_t bytes) {
    grow_calls++;
    if (grow_fails || a->top != committed_top)
        return false;
    committed_top += bytes;
    return true;
}

static bool test_shrink(struct HeapArena *a, uint32_t bytes) {
    shrink_calls++;
    if (a->top + bytes != committed_top)
        return false;
    committed_top = a->top;
    memset(committed_top, 0xEE, bytes); // poison released memory: any later use shows up
    return true;
}

static uint32_t rng_state = 12345;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

struct Block {
    uint8_t *ptr;
    uint32_t size;
    uint8_t  seed;
};

#define MAX_BLOCKS 3000
static struct Block blocks[MAX_BLOCKS];
static uint32_t     live = 0;

static void fill(struct Block *b) {
    for (uint32_t i = 0; i < b->size; i++)
        b->ptr[i] = (uint8_t) (b->seed + i * 7);
}

static bool intact(const struct Block *b, uint32_t upto) {
    for (uint32_t i = 0; i < upto && i < b->size; i++)
        if (b->ptr[i] != (uint8_t) (b->seed + i * 7))
            return false;
    return true;
}

static uint32_t random_size(void) {
    uint32_t r = rnd() % 100;
    if (r < 60)
        return 1 + rnd() % 64;
    if (r < 90)
        return 1 + rnd() % 2048;
    if (r < 99)
        return 1 + rnd() % 65536;
    return 1 + rnd() % (512 * 1024);
}

static void check_no_overlap(void) {
    uint32_t overlaps = 0;
    for (uint32_t i = 0; i < live; i++)
        for (uint32_t j = i + 1; j < live; j++)
            if (blocks[i].ptr < blocks[j].ptr + blocks[j].size && blocks[j].ptr < blocks[i].ptr + blocks[i].size)
                overlaps++;
    CHECK(overlaps == 0, "%u overlapping block pairs", overlaps);
}

static void random_operations(struct HeapArena *a, uint32_t steps) {
    for (uint32_t step = 0; step < steps; step++) {
        uint32_t op = rnd() % 10;
        if ((op < 5 || live == 0) && live < MAX_BLOCKS) {
            struct Block b = {.size = random_size(), .seed = (uint8_t) rnd()};
            bool zeroed = rnd() % 4 == 0;
            b.ptr = zeroed ? heap_calloc(a, 1, b.size) : heap_alloc(a, b.size);
            if (b.ptr == NULL)
                continue; // arena full: allowed
            CHECK(((uintptr_t) b.ptr & 7) == 0, "pointer %p not 8-byte aligned", (void*) b.ptr);
            CHECK(heap_usable_size(b.ptr) >= b.size, "usable size too small");
            if (zeroed) {
                bool all_zero = true;
                for (uint32_t i = 0; i < b.size; i++)
                    all_zero &= b.ptr[i] == 0;
                CHECK(all_zero, "calloc memory not zeroed");
            }
            fill(&b);
            blocks[live++] = b;
        } else if (op < 8) {
            uint32_t k = rnd() % live;
            CHECK(intact(&blocks[k], blocks[k].size), "block contents changed before free");
            heap_free(a, blocks[k].ptr);
            blocks[k] = blocks[--live];
        } else {
            uint32_t k = rnd() % live;
            struct Block *b = &blocks[k];
            uint32_t new_size = random_size();
            uint8_t *p = heap_realloc(a, b->ptr, new_size);
            if (p == NULL)
                continue; // the old block must still be valid
            uint32_t keep = b->size < new_size ? b->size : new_size;
            b->ptr = p;
            CHECK(intact(b, keep), "realloc lost data (%u -> %u)", b->size, new_size);
            b->size = new_size;
            fill(b);
        }
        if (step % 64 == 0) {
            CHECK(heap_check(a), "heap_check failed at step %u", step);
            struct HeapStats st;
            heap_get_stats(a, &st);
            CHECK(st.used_chunks == live, "used_chunks %u != live %u", st.used_chunks, live);
            CHECK(st.committed_bytes == (uint32_t) (committed_top - a->base), "committed bytes mismatch");
        }
        if (step % 2000 == 0)
            check_no_overlap();
    }
}

int main(void) {
    memory = malloc(ARENA_SIZE + 64);
    if (memory == NULL)
        return 1;
    struct HeapArena arena;

    // 1. Randomized model test (arena with trimming, like the user heap)
    heap_init(&arena, memory + 3, memory + ARENA_SIZE, 16 * 1024, test_grow); // unaligned base on purpose
    heap_set_shrink(&arena, test_shrink);
    committed_top = arena.base;
    CHECK(arena.base == memory + 8, "base not rounded up to 8");
    random_operations(&arena, 40000);
    check_no_overlap();
    CHECK(heap_check(&arena), "heap_check after random test");
    while (live > 0) {
        CHECK(intact(&blocks[live - 1], blocks[live - 1].size), "contents changed before final free");
        heap_free(&arena, blocks[--live].ptr);
    }
    struct HeapStats st;
    heap_get_stats(&arena, &st);
    CHECK(heap_check(&arena), "heap_check after freeing everything");
    CHECK(st.used_chunks == 0 && st.used_bytes == 0, "leak: %u chunks / %u bytes still used", st.used_chunks, st.used_bytes);
    CHECK(st.free_chunks <= 1, "free memory not coalesced into one chunk (%u chunks)", st.free_chunks);
    CHECK(st.trim_count > 0 && shrink_calls > 0, "trimming never happened");
    CHECK(st.committed_bytes <= HEAP_TRIM_THRESHOLD + 16 * 1024, "top not trimmed (%u bytes committed)", st.committed_bytes);
    printf("  random test: %u grows, %u trims, %u allocations\n", st.grow_count, st.trim_count, st.alloc_count);

    // 2. Coalescing both ways: A B C, free A and C, then B -> one free chunk
    heap_init(&arena, memory, memory + ARENA_SIZE, 4096, test_grow);
    committed_top = arena.base;
    void *x = heap_alloc(&arena, 100), *y = heap_alloc(&arena, 100), *z = heap_alloc(&arena, 100);
    void *guard = heap_alloc(&arena, 100);
    heap_free(&arena, x);
    heap_free(&arena, z);
    heap_get_stats(&arena, &st);
    CHECK(st.free_chunks == 3, "expected 3 free chunks (x, z, tail), got %u", st.free_chunks);
    heap_free(&arena, y);
    heap_get_stats(&arena, &st);
    CHECK(st.free_chunks == 2, "x y z should merge into one chunk, got %u free chunks", st.free_chunks);
    void *xyz = heap_alloc(&arena, 300);
    CHECK(xyz == x, "merged chunk should be reused for a 300-byte request");
    heap_free(&arena, xyz);
    heap_free(&arena, guard);
    CHECK(heap_check(&arena), "heap_check after coalescing test");

    // 3. Double free and wild pointers are detected, not fatal
    void *p = heap_alloc(&arena, 40);
    heap_free(&arena, p);
    uint32_t errors_before = arena.errors;
    heap_free(&arena, p);
    heap_free(&arena, (uint8_t*) p + 3);
    heap_free(&arena, memory + ARENA_SIZE - 16);
    CHECK(arena.errors == errors_before + 3, "double / invalid frees not detected (%u)", arena.errors - errors_before);
    CHECK(heap_check(&arena), "heap_check after invalid frees");

    // 4. In-place realloc: the top chunk grows without moving
    void *top = heap_alloc(&arena, 1000);
    void *grown = heap_realloc(&arena, top, 50000);
    CHECK(grown == top, "realloc of the top chunk should grow in place");
    void *shrunk = heap_realloc(&arena, grown, 10);
    CHECK(shrunk == grown, "shrinking realloc should stay in place");
    CHECK(heap_check(&arena), "heap_check after in-place realloc");
    heap_free(&arena, shrunk);

    // 5. Out of memory: the grow callback refuses -> NULL, heap still consistent
    grow_fails = true;
    void *huge = heap_alloc(&arena, ARENA_SIZE * 2u);
    CHECK(huge == NULL, "allocation larger than the arena must fail");
    void *refused = heap_alloc(&arena, 1u << 20);
    CHECK(refused == NULL, "allocation must fail when the grow callback refuses");
    grow_fails = false;
    CHECK(heap_alloc(&arena, 0x7FFFFFFFu) == NULL, "absurd size must fail");
    CHECK(heap_calloc(&arena, 0x10000u, 0x10000u) == NULL, "calloc overflow must fail");
    CHECK(heap_check(&arena), "heap_check after OOM");

    free(memory);
    printf("heap: %d/%d checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
