// Host test for src/process/pipe.c: FIFO order, growth, end of file, broken pipe, pool limits.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "header/process/pipe.h"

void *kmalloc(size_t n) { return malloc(n); }
void  kfree(void *p) { free(p); }

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void) {
    printf("[pipe] fifo, growth, eof, broken pipe, pool\n");
    struct Pipe *p = pipe_create();
    CHECK(p != NULL);
    pipe_add_reader(p);
    pipe_add_writer(p);
    char buf[64];
    CHECK(pipe_read(p, buf, sizeof(buf)) == -1); // empty, writer alive: would block
    CHECK(!pipe_readable(p));
    CHECK(pipe_write(p, "hello ", 6) == 6);
    CHECK(pipe_write(p, "world", 5) == 5);
    CHECK(pipe_readable(p));
    CHECK(pipe_read(p, buf, 3) == 3 && memcmp(buf, "hel", 3) == 0);
    CHECK(pipe_read(p, buf, sizeof(buf)) == 8 && memcmp(buf, "lo world", 8) == 0);

    // Growth beyond the initial 4 KiB, with the ring wrapped first; byte order is kept
    unsigned char *big = malloc(300000), *back = malloc(300000);
    for (int i = 0; i < 300000; i++)
        big[i] = (unsigned char) (i * 7 + i / 251);
    CHECK(pipe_write(p, big, 3000) == 3000);
    CHECK(pipe_read(p, back, 2000) == 2000);
    CHECK(pipe_write(p, big + 3000, 297000) == 297000);
    CHECK(p->capacity >= 298000 && p->capacity <= PIPE_CAPACITY_MAX);
    int32_t got = 2000, n;
    while ((n = pipe_read(p, back + got, 7777)) > 0)
        got += n;
    CHECK(got == 300000 && memcmp(big, back, 300000) == 0);

    // Capacity limit: a full pipe accepts nothing more (the writer would block)
    unsigned char *huge = calloc(1, PIPE_CAPACITY_MAX + 10);
    CHECK(pipe_write(p, huge, PIPE_CAPACITY_MAX + 10) == (int32_t) PIPE_CAPACITY_MAX);
    CHECK(pipe_write(p, "x", 1) == 0 && !pipe_writable(p));
    CHECK(pipe_read(p, huge, 10) == 10 && pipe_writable(p));

    // End of file once the writer is gone and the data is drained
    pipe_close_writer(p);
    while (pipe_read(p, huge, PIPE_CAPACITY_MAX) > 0) {
    }
    CHECK(pipe_read(p, buf, 1) == 0 && pipe_readable(p));
    pipe_close_reader(p); // frees it

    // Broken pipe: no reader left
    struct Pipe *q = pipe_create();
    pipe_add_reader(q);
    pipe_add_writer(q);
    pipe_close_reader(q);
    CHECK(pipe_write(q, "x", 1) == -1 && pipe_writable(q));
    pipe_close_writer(q);

    // Pool: PIPE_COUNT_MAX pipes at once, slots are reused after closing
    struct Pipe *all[PIPE_COUNT_MAX];
    for (int i = 0; i < PIPE_COUNT_MAX; i++) {
        all[i] = pipe_create();
        CHECK(all[i] != NULL);
        pipe_add_reader(all[i]);
    }
    CHECK(pipe_create() == NULL && pipe_count_used() == PIPE_COUNT_MAX);
    for (int i = 0; i < PIPE_COUNT_MAX; i++)
        pipe_close_reader(all[i]);
    CHECK(pipe_count_used() == 0);
    free(big);
    free(back);
    free(huge);
    printf("pipe: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
