#include "header/process/pipe.h"
#include "header/memory/kheap.h"
#include "header/stdlib/string.h"

static struct Pipe pipes[PIPE_COUNT_MAX];
static uint32_t    next_id = 1;

struct Pipe *pipe_create(void) {
    for (uint32_t i = 0; i < PIPE_COUNT_MAX; i++) {
        struct Pipe *p = &pipes[i];
        if (p->used)
            continue;
        uint8_t *data = kmalloc(PIPE_INITIAL_CAPACITY);
        if (data == NULL)
            return NULL;
        memset(p, 0, sizeof(*p));
        p->used     = true;
        p->id       = next_id++;
        p->data     = data;
        p->capacity = PIPE_INITIAL_CAPACITY;
        return p;
    }
    return NULL;
}

static void pipe_release_if_unused(struct Pipe *p) {
    if (p->readers == 0 && p->writers == 0) {
        kfree(p->data);
        memset(p, 0, sizeof(*p));
    }
}

void pipe_add_reader(struct Pipe *p) {
    p->readers++;
}

void pipe_add_writer(struct Pipe *p) {
    p->writers++;
}

void pipe_close_reader(struct Pipe *p) {
    if (p->readers > 0)
        p->readers--;
    if (p->readers == 0) { // nobody will read: drop what is left
        p->length = 0;
        p->head   = 0;
    }
    pipe_release_if_unused(p);
}

void pipe_close_writer(struct Pipe *p) {
    if (p->writers > 0)
        p->writers--;
    pipe_release_if_unused(p);
}

// Double the ring (unwrapping it) so `extra` more bytes fit; false at PIPE_CAPACITY_MAX
static bool pipe_grow(struct Pipe *p, uint32_t extra) {
    uint32_t needed = p->length + extra;
    if (needed <= p->capacity)
        return true;
    if (p->capacity >= PIPE_CAPACITY_MAX)
        return false;
    uint32_t cap = p->capacity;
    while (cap < needed && cap < PIPE_CAPACITY_MAX)
        cap *= 2;
    if (cap > PIPE_CAPACITY_MAX)
        cap = PIPE_CAPACITY_MAX;
    uint8_t *bigger = kmalloc(cap);
    if (bigger == NULL)
        return false;
    for (uint32_t i = 0; i < p->length; i++)
        bigger[i] = p->data[(p->head + i) % p->capacity];
    kfree(p->data);
    p->data     = bigger;
    p->capacity = cap;
    p->head     = 0;
    return true;
}

int32_t pipe_write(struct Pipe *p, const void *src, uint32_t len) {
    if (p->readers == 0)
        return -1;
    if (len == 0)
        return 0;
    pipe_grow(p, len); // best effort: a full pipe at its maximum just accepts less
    uint32_t room = p->capacity - p->length;
    if (room == 0)
        return 0;
    uint32_t n = len < room ? len : room;
    const uint8_t *s = src;
    uint32_t tail = (p->head + p->length) % p->capacity;
    for (uint32_t i = 0; i < n; i++)
        p->data[(tail + i) % p->capacity] = s[i];
    p->length   += n;
    p->total_in += n;
    return (int32_t) n;
}

int32_t pipe_read(struct Pipe *p, void *dst, uint32_t len) {
    if (p->length == 0)
        return p->writers == 0 ? 0 : -1;
    uint32_t n = len < p->length ? len : p->length;
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++)
        d[i] = p->data[(p->head + i) % p->capacity];
    p->head       = (p->head + n) % p->capacity;
    p->length    -= n;
    p->total_out += n;
    if (p->length == 0)
        p->head = 0;
    return (int32_t) n;
}

bool pipe_readable(const struct Pipe *p) {
    return p->length > 0 || p->writers == 0;
}

bool pipe_writable(const struct Pipe *p) {
    return p->readers == 0 || p->length < p->capacity || p->capacity < PIPE_CAPACITY_MAX;
}

uint32_t pipe_count_used(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < PIPE_COUNT_MAX; i++)
        n += pipes[i].used ? 1 : 0;
    return n;
}
