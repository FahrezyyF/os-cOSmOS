#ifndef _PIPE_H
#define _PIPE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Kernel pipes: a byte FIFO shared by file descriptors of one or more processes. Used by
 * the shell to connect separate command programs (`ls | grep x`, `> file` captures).
 * The buffer lives in the kernel heap and grows on demand up to PIPE_CAPACITY_MAX, so a
 * pipeline stage can be fed completely before the next one starts.
 */

#define PIPE_COUNT_MAX        32
#define PIPE_INITIAL_CAPACITY 4096u
#define PIPE_CAPACITY_MAX     (1024u * 1024u)

struct Pipe {
    bool     used;
    uint32_t id;
    uint8_t *data;      // ring buffer
    uint32_t capacity;
    uint32_t head;      // next byte to read
    uint32_t length;    // bytes stored
    uint32_t readers;   // open read ends (descriptors)
    uint32_t writers;   // open write ends
    uint32_t total_in;
    uint32_t total_out;
};

// New pipe with no ends open yet (NULL when the pool / heap is exhausted)
struct Pipe *pipe_create(void);

void pipe_add_reader(struct Pipe *pipe);
void pipe_add_writer(struct Pipe *pipe);
// Close one end; the pipe is freed once both sides are closed
void pipe_close_reader(struct Pipe *pipe);
void pipe_close_writer(struct Pipe *pipe);

/**
 * Write up to `len` bytes.
 * @return bytes written (> 0), 0 when the pipe is full (caller blocks), -1 when no reader
 *         is left (broken pipe)
 */
int32_t pipe_write(struct Pipe *pipe, const void *src, uint32_t len);

/**
 * Read up to `len` bytes.
 * @return bytes read (> 0), 0 at end of file (empty and no writer left), -1 when empty but
 *         a writer may still produce data (caller blocks)
 */
int32_t pipe_read(struct Pipe *pipe, void *dst, uint32_t len);

bool pipe_readable(const struct Pipe *pipe); // data or EOF: a blocked reader can continue
bool pipe_writable(const struct Pipe *pipe); // room or no reader: a blocked writer can continue
uint32_t pipe_count_used(void);

#endif
