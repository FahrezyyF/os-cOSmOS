#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "header/stdlib/heap.h"
#include "header/stdlib/string.h"

/**
 * Chunk layout (every chunk starts 8-byte aligned and tiles the arena [base, top)):
 *
 *   +--------------------+------------------+--------------------------------------+
 *   | size | tag | used  | prev_size        | payload (used) or next/prev links    |
 *   +--------------------+------------------+--------------------------------------+
 *   0                    4                  8
 *
 * size includes the 8-byte header and is a multiple of 8, so the low 3 bits are flags:
 * bit 0 = in use, bit 2 = tag (always 1, catches frees of pointers that are not chunks).
 * prev_size = size of the chunk right before (0 for the first chunk), which makes the
 * left neighbour reachable in O(1) for coalescing without a footer.
 */

#define FLAG_IN_USE   0x1u
#define FLAG_TAG      0x4u
#define FLAG_MASK     0x7u
#define HEADER_SIZE   8u

struct HeapChunk {
    uint32_t size_flags;
    uint32_t prev_size;
};

struct HeapFreeChunk {
    struct HeapChunk      header;
    struct HeapFreeChunk *next;
    struct HeapFreeChunk *prev;
};

_Static_assert(sizeof(struct HeapChunk) == HEADER_SIZE, "chunk header is 8 bytes");
_Static_assert(sizeof(struct HeapFreeChunk) <= HEAP_MIN_CHUNK, "a free chunk must fit its links");

static inline uint32_t chunk_size(const struct HeapChunk *c) {
    return c->size_flags & ~FLAG_MASK;
}

static inline bool chunk_used(const struct HeapChunk *c) {
    return (c->size_flags & FLAG_IN_USE) != 0;
}

static inline void set_header(struct HeapChunk *c, uint32_t size, bool used) {
    c->size_flags = size | FLAG_TAG | (used ? FLAG_IN_USE : 0);
}

static inline struct HeapChunk *next_chunk(struct HeapChunk *c) {
    return (struct HeapChunk*) ((uint8_t*) c + chunk_size(c));
}

static inline struct HeapChunk *prev_chunk(struct HeapChunk *c) {
    return (struct HeapChunk*) ((uint8_t*) c - c->prev_size);
}

static inline bool in_arena(struct HeapArena *a, const void *p) {
    return (const uint8_t*) p >= a->base && (const uint8_t*) p < a->top;
}

static inline uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Bin b holds free chunks with size in [16 << b, 16 << (b + 1)); the last bin holds the rest
static uint32_t bin_index(uint32_t size) {
    uint32_t bin = 0;
    while (bin < HEAP_BIN_COUNT - 1 && (size >> (bin + 5)) != 0)
        bin++;
    return bin;
}

static void bin_insert(struct HeapArena *a, struct HeapChunk *chunk) {
    struct HeapFreeChunk *c = (struct HeapFreeChunk*) chunk;
    uint32_t bin = bin_index(chunk_size(chunk));
    c->prev = NULL;
    c->next = a->bins[bin];
    if (c->next != NULL)
        c->next->prev = c;
    a->bins[bin] = c;
    a->free_bytes += chunk_size(chunk);
    a->free_chunks++;
}

static void bin_remove(struct HeapArena *a, struct HeapChunk *chunk) {
    struct HeapFreeChunk *c = (struct HeapFreeChunk*) chunk;
    uint32_t bin = bin_index(chunk_size(chunk));
    if (c->prev != NULL)
        c->prev->next = c->next;
    else
        a->bins[bin] = c->next;
    if (c->next != NULL)
        c->next->prev = c->prev;
    a->free_bytes -= chunk_size(chunk);
    a->free_chunks--;
}

// Keep prev_size of the following chunk in sync after `c` changed size
static void fix_next_prev_size(struct HeapArena *a, struct HeapChunk *c) {
    struct HeapChunk *n = next_chunk(c);
    if (in_arena(a, n))
        n->prev_size = chunk_size(c);
}

void heap_init(struct HeapArena *arena, void *base, void *limit, uint32_t grow_granularity, HeapGrowFunction grow) {
    memset(arena, 0, sizeof(*arena));
    uintptr_t aligned = ((uintptr_t) base + HEAP_ALIGNMENT - 1) & ~(uintptr_t) (HEAP_ALIGNMENT - 1);
    arena->base             = (uint8_t*) aligned;
    arena->top              = arena->base;
    arena->limit            = (uint8_t*) limit;
    arena->last             = NULL;
    arena->grow_granularity = align_up(grow_granularity < HEAP_ALIGNMENT ? HEAP_ALIGNMENT : grow_granularity, HEAP_ALIGNMENT);
    arena->grow             = grow;
    arena->shrink           = NULL;
}

void heap_set_shrink(struct HeapArena *arena, HeapShrinkFunction shrink) {
    arena->shrink = shrink;
}

/**
 * Merge a free chunk (not in any bin) with free neighbours.
 * @return the start of the merged chunk, still not in a bin
 */
static struct HeapChunk *coalesce(struct HeapArena *a, struct HeapChunk *c) {
    struct HeapChunk *n = next_chunk(c);
    if (in_arena(a, n) && !chunk_used(n)) {
        bin_remove(a, n);
        if ((uint8_t*) n == a->last)
            a->last = (uint8_t*) c;
        set_header(c, chunk_size(c) + chunk_size(n), false);
    }
    if (c->prev_size != 0) {
        struct HeapChunk *p = prev_chunk(c);
        if (!chunk_used(p)) {
            bin_remove(a, p);
            if ((uint8_t*) c == a->last)
                a->last = (uint8_t*) p;
            set_header(p, chunk_size(p) + chunk_size(c), false);
            c = p;
        }
    }
    fix_next_prev_size(a, c);
    return c;
}

// Commit at least `needed` more bytes at the top; the new space becomes a free chunk
static bool grow_arena(struct HeapArena *a, uint32_t needed) {
    uint32_t bytes = align_up(needed, a->grow_granularity);
    if (a->grow == NULL || bytes < needed || (uint32_t) (a->limit - a->top) < bytes)
        return false;
    if (!a->grow(a, bytes))
        return false;
    struct HeapChunk *c = (struct HeapChunk*) a->top;
    c->prev_size = a->last != NULL ? chunk_size((struct HeapChunk*) a->last) : 0;
    set_header(c, bytes, false);
    a->top  += bytes;
    a->last  = (uint8_t*) c;
    a->grow_count++;
    bin_insert(a, coalesce(a, c));
    return true;
}

static struct HeapChunk *find_fit(struct HeapArena *a, uint32_t need) {
    uint32_t bin = bin_index(need);
    // Own size class: first fit (sizes inside one class differ)
    for (struct HeapFreeChunk *c = a->bins[bin]; c != NULL; c = c->next)
        if (chunk_size(&c->header) >= need)
            return &c->header;
    // Larger classes: every chunk fits, take the head
    for (uint32_t b = bin + 1; b < HEAP_BIN_COUNT; b++)
        if (a->bins[b] != NULL)
            return &a->bins[b]->header;
    return NULL;
}

// Mark `c` (size >= need, not in a bin) used and give the excess back as a free chunk
static void take_chunk(struct HeapArena *a, struct HeapChunk *c, uint32_t need) {
    uint32_t size = chunk_size(c);
    if (size - need >= HEAP_MIN_CHUNK) {
        set_header(c, need, true);
        struct HeapChunk *rest = next_chunk(c);
        rest->prev_size = need;
        set_header(rest, size - need, false);
        if ((uint8_t*) c == a->last)
            a->last = (uint8_t*) rest;
        fix_next_prev_size(a, rest);
        bin_insert(a, coalesce(a, rest));
    } else {
        set_header(c, size, true);
    }
    a->used_bytes += chunk_size(c);
    a->used_chunks++;
}

static uint32_t request_size(size_t size) {
    if (size > 0x7FFFFF00u)
        return 0;
    uint32_t need = align_up((uint32_t) size + HEADER_SIZE, HEAP_ALIGNMENT);
    return need < HEAP_MIN_CHUNK ? HEAP_MIN_CHUNK : need;
}

void *heap_alloc(struct HeapArena *a, size_t size) {
    uint32_t need = request_size(size == 0 ? 1 : size);
    if (need == 0)
        return NULL;
    struct HeapChunk *c = find_fit(a, need);
    if (c == NULL) {
        if (!grow_arena(a, need))
            return NULL;
        c = find_fit(a, need);
        if (c == NULL)
            return NULL;
    }
    bin_remove(a, c);
    take_chunk(a, c, need);
    a->alloc_count++;
    return (uint8_t*) c + HEADER_SIZE;
}

void *heap_calloc(struct HeapArena *a, size_t count, size_t size) {
    if (size != 0 && count > 0xFFFFFFFFu / size)
        return NULL;
    void *p = heap_alloc(a, count * size);
    if (p != NULL)
        memset(p, 0, count * size);
    return p;
}

// Validate a pointer handed back by the user; NULL if it is not a live chunk
static struct HeapChunk *checked_chunk(struct HeapArena *a, void *ptr) {
    struct HeapChunk *c = (struct HeapChunk*) ((uint8_t*) ptr - HEADER_SIZE);
    if (!in_arena(a, c) || ((uintptr_t) c & (HEAP_ALIGNMENT - 1)) != 0 ||
        !(c->size_flags & FLAG_TAG) || chunk_size(c) < HEAP_MIN_CHUNK ||
        chunk_size(c) > (uint32_t) (a->top - (uint8_t*) c) || !chunk_used(c)) {
        a->errors++; // invalid pointer or double free
        return NULL;
    }
    return c;
}

// Give the tail of a large free top chunk back (keeps one grow granule as slack)
static void trim_top(struct HeapArena *a) {
    struct HeapChunk *last = (struct HeapChunk*) a->last;
    if (a->shrink == NULL || last == NULL || chunk_used(last) || chunk_size(last) < HEAP_TRIM_THRESHOLD)
        return;
    uint32_t keep    = a->grow_granularity < HEAP_MIN_CHUNK ? HEAP_MIN_CHUNK : a->grow_granularity;
    uint32_t size    = chunk_size(last);
    uint32_t release = (size - keep) / a->grow_granularity * a->grow_granularity;
    if (release == 0)
        return;
    bin_remove(a, last);
    set_header(last, size - release, false);
    a->top -= release;
    if (a->shrink(a, release)) {
        a->trim_count++;
    } else {
        a->top += release; // the memory stays committed
        set_header(last, size, false);
    }
    bin_insert(a, last);
}

void heap_free(struct HeapArena *a, void *ptr) {
    if (ptr == NULL)
        return;
    struct HeapChunk *c = checked_chunk(a, ptr);
    if (c == NULL)
        return;
    a->used_bytes -= chunk_size(c);
    a->used_chunks--;
    a->free_count++;
    set_header(c, chunk_size(c), false);
    bin_insert(a, coalesce(a, c));
    trim_top(a);
}

void *heap_realloc(struct HeapArena *a, void *ptr, size_t size) {
    if (ptr == NULL)
        return heap_alloc(a, size);
    if (size == 0) {
        heap_free(a, ptr);
        return NULL;
    }
    struct HeapChunk *c = checked_chunk(a, ptr);
    uint32_t need = request_size(size);
    if (c == NULL || need == 0)
        return NULL;

    // Grow in place: absorb a free right neighbour, extending the arena first when c is the
    // top chunk or is followed only by the free top chunk
    if (chunk_size(c) < need) {
        struct HeapChunk *n = next_chunk(c);
        bool free_top_next = in_arena(a, n) && !chunk_used(n) && (uint8_t*) n == a->last;
        uint32_t have = chunk_size(c) + (free_top_next ? chunk_size(n) : 0);
        if (((uint8_t*) c == a->last || free_top_next) && have < need)
            grow_arena(a, need - have); // on failure the copy below still works
    }
    if (chunk_size(c) < need) {
        struct HeapChunk *n = next_chunk(c);
        if (in_arena(a, n) && !chunk_used(n) && chunk_size(c) + chunk_size(n) >= need) {
            bin_remove(a, n);
            if ((uint8_t*) n == a->last)
                a->last = (uint8_t*) c;
            a->used_bytes += chunk_size(n);
            set_header(c, chunk_size(c) + chunk_size(n), true);
            fix_next_prev_size(a, c);
        }
    }
    if (chunk_size(c) >= need) {
        // Shrink (or trim the absorbed excess): split off the tail as a free chunk
        uint32_t size_now = chunk_size(c);
        if (size_now - need >= HEAP_MIN_CHUNK) {
            a->used_bytes -= size_now;
            a->used_chunks--;
            set_header(c, size_now, false);
            take_chunk(a, c, need);
        }
        return ptr;
    }

    void *fresh = heap_alloc(a, size);
    if (fresh == NULL)
        return NULL;
    memcpy(fresh, ptr, chunk_size(c) - HEADER_SIZE);
    heap_free(a, ptr);
    return fresh;
}

size_t heap_usable_size(void *ptr) {
    if (ptr == NULL)
        return 0;
    struct HeapChunk *c = (struct HeapChunk*) ((uint8_t*) ptr - HEADER_SIZE);
    return chunk_size(c) - HEADER_SIZE;
}

void heap_get_stats(struct HeapArena *a, struct HeapStats *stats) {
    memset(stats, 0, sizeof(*stats));
    stats->committed_bytes = (uint32_t) (a->top - a->base);
    stats->used_bytes      = a->used_bytes;
    stats->used_chunks     = a->used_chunks;
    stats->free_bytes      = a->free_bytes;
    stats->free_chunks     = a->free_chunks;
    stats->alloc_count     = a->alloc_count;
    stats->free_count      = a->free_count;
    stats->grow_count      = a->grow_count;
    stats->trim_count      = a->trim_count;
    stats->errors          = a->errors;
    for (uint32_t b = 0; b < HEAP_BIN_COUNT; b++)
        for (struct HeapFreeChunk *c = a->bins[b]; c != NULL; c = c->next)
            if (chunk_size(&c->header) > stats->largest_free)
                stats->largest_free = chunk_size(&c->header);
}

bool heap_check(struct HeapArena *a) {
    uint32_t used_bytes = 0, used_chunks = 0, free_bytes = 0, free_chunks = 0;
    uint32_t prev_size = 0;
    bool prev_free = false;
    struct HeapChunk *last = NULL;
    for (struct HeapChunk *c = (struct HeapChunk*) a->base; (uint8_t*) c < a->top; c = next_chunk(c)) {
        uint32_t size = chunk_size(c);
        if (!(c->size_flags & FLAG_TAG) || size < HEAP_MIN_CHUNK || (size & (HEAP_ALIGNMENT - 1)) != 0 ||
            size > (uint32_t) (a->top - (uint8_t*) c) || c->prev_size != prev_size)
            return false;
        if (chunk_used(c)) {
            used_bytes += size;
            used_chunks++;
            prev_free = false;
        } else {
            if (prev_free)
                return false; // two neighbouring free chunks must have been merged
            free_bytes += size;
            free_chunks++;
            prev_free = true;
        }
        prev_size = size;
        last = c;
    }
    if ((uint8_t*) last != a->last)
        return false;
    uint32_t listed_bytes = 0, listed_chunks = 0;
    for (uint32_t b = 0; b < HEAP_BIN_COUNT; b++) {
        struct HeapFreeChunk *prev = NULL;
        for (struct HeapFreeChunk *c = a->bins[b]; c != NULL; c = c->next) {
            if (!in_arena(a, c) || chunk_used(&c->header) || bin_index(chunk_size(&c->header)) != b || c->prev != prev)
                return false;
            listed_bytes += chunk_size(&c->header);
            if (++listed_chunks > free_chunks)
                return false; // cycle in a free list
            prev = c;
        }
    }
    return used_bytes == a->used_bytes && used_chunks == a->used_chunks &&
           free_bytes == a->free_bytes && free_chunks == a->free_chunks &&
           listed_bytes == free_bytes && listed_chunks == free_chunks;
}
