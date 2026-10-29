#include "user/ulib.h"

/* ================================ SYSTEM CALL ====================================== */

void syscall(uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    // Same contract as the book's version (eax = service, ebx/ecx/edx = arguments), written
    // with register constraints so gcc cannot reuse eax while the others are loaded
    __asm__ volatile("int $0x30" : : "a"(eax), "b"(ebx), "c"(ecx), "d"(edx) : "memory");
}

void exit(int code) {
    syscall(SYS_EXIT, (uint32_t) code, 0, 0);
    for (;;) {
    }
}

/* ================================ CONSOLE ========================================== */

char getchar_wait(void) {
    char c = 0;
    syscall(SYS_GETCHAR_WAIT, (uint32_t) &c, 0, 0);
    return c;
}

char getchar_nowait(void) {
    char c = 0;
    syscall(SYS_GETCHAR, (uint32_t) &c, 0, 0);
    return c;
}

void console_raw_write(const char *s, uint32_t len, uint8_t color) {
    if (len > 0)
        syscall(SYS_PUTS, (uint32_t) s, len, color);
}

void console_clear_screen(void) {
    syscall(SYS_CLEAR_SCREEN, 0, 0, 0);
}

void console_set_cursor(uint8_t row, uint8_t col) {
    syscall(SYS_SET_CURSOR, row, col, 0);
}

void console_get_cursor(uint8_t *row, uint8_t *col) {
    syscall(SYS_GET_CURSOR, (uint32_t) row, (uint32_t) col, 0);
}

void console_screen_info(struct SyscallScreenInfo *info) {
    syscall(SYS_SCREEN_INFO, (uint32_t) info, 0, 0);
}

uint32_t console_scroll(int32_t lines) {
    uint32_t offset = 0;
    syscall(SYS_CONSOLE_SCROLL, (uint32_t) lines, (uint32_t) &offset, 0);
    return offset;
}

void set_status(const char *text) {
    syscall(SYS_SET_STATUS, (uint32_t) text, 0, 0);
}

/* ================================ MISC SERVICES ==================================== */

uint32_t uptime_ms(void) {
    uint32_t ms = 0;
    syscall(SYS_UPTIME, (uint32_t) &ms, 0, 0);
    return ms;
}

void sleep_ms(uint32_t ms) {
    syscall(SYS_SLEEP, ms, 0, 0);
}

void get_time(struct SyscallTime *t) {
    syscall(SYS_TIME, (uint32_t) t, 0, 0);
}

void beep(uint32_t frequency_hz, uint32_t ms) {
    syscall(SYS_BEEP, frequency_hz, ms, 0);
}

void power(uint32_t action) {
    syscall(SYS_POWER, action, 0, 0);
    for (;;) {
    }
}

void get_meminfo(struct SyscallMemInfo *info) {
    syscall(SYS_MEMINFO, (uint32_t) info, 0, 0);
}

void get_kheap_info(struct SyscallHeapInfo *info) {
    syscall(SYS_KHEAP_INFO, (uint32_t) info, 0, 0);
}

void get_sysinfo(struct SyscallSysInfo *info) {
    syscall(SYS_SYSINFO, (uint32_t) info, 0, 0);
}

void get_mouse(struct SyscallMouse *mouse) {
    syscall(SYS_MOUSE, (uint32_t) mouse, 0, 0);
}

/* ================================ HEAP ============================================= */

#define USER_HEAP_GROW (16 * 1024) // sbrk() step; pages are only backed when touched

static struct HeapArena user_heap;
static bool             user_heap_ready = false;

void *sbrk(int32_t increment) {
    uint32_t old_break = 0xFFFFFFFFu;
    syscall(SYS_SBRK, (uint32_t) increment, (uint32_t) &old_break, 0);
    return (void*) old_break;
}

// The arena only grows at its top, which is always the current program break
static bool user_heap_grow(struct HeapArena *arena, uint32_t bytes) {
    void *old = sbrk((int32_t) bytes);
    return old != (void*) -1 && (uint8_t*) old == arena->top;
}

// Trimming: the freed top of the arena goes back to the kernel (its pages are freed)
static bool user_heap_shrink(struct HeapArena *arena, uint32_t bytes) {
    void *old = sbrk(-(int32_t) bytes);
    return old != (void*) -1 && (uint8_t*) old == arena->top + bytes;
}

static void user_heap_init(void) {
    uint8_t *base = (uint8_t*) sbrk(0);
    // The arena may span the whole sbrk() range; the kernel enforces the real limit
    heap_init(&user_heap, base, (void*) 0x40000000u, USER_HEAP_GROW, user_heap_grow);
    heap_set_shrink(&user_heap, user_heap_shrink);
    user_heap_ready = true;
}

void *malloc(size_t size) {
    if (!user_heap_ready)
        user_heap_init();
    return heap_alloc(&user_heap, size);
}

void *calloc(size_t count, size_t size) {
    if (!user_heap_ready)
        user_heap_init();
    return heap_calloc(&user_heap, count, size);
}

void *realloc(void *ptr, size_t size) {
    if (!user_heap_ready)
        user_heap_init();
    return heap_realloc(&user_heap, ptr, size);
}

void free(void *ptr) {
    if (user_heap_ready)
        heap_free(&user_heap, ptr);
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *copy = malloc(len + 1);
    if (copy != NULL) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

char *strdup(const char *s) {
    return strndup(s, strlen(s));
}

void user_heap_stats(struct HeapStats *stats) {
    if (!user_heap_ready)
        user_heap_init();
    heap_get_stats(&user_heap, stats);
}

bool user_heap_check(void) {
    if (!user_heap_ready)
        user_heap_init();
    return heap_check(&user_heap);
}

/* ================================ STRINGS ========================================== */

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
}

int atoi(const char *s) {
    int sign = 1, value = 0;
    while (is_space(*s))
        s++;
    if (*s == '-' || *s == '+')
        sign = (*s++ == '-') ? -1 : 1;
    while (is_digit(*s))
        value = value * 10 + (*s++ - '0');
    return sign * value;
}

bool parse_uint(const char *s, uint32_t *out) {
    if (s == NULL || !is_digit(*s))
        return false;
    uint32_t value = 0;
    while (is_digit(*s)) {
        uint32_t digit = (uint32_t) (*s++ - '0');
        if (value > (0xFFFFFFFFu - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    if (*s != '\0')
        return false;
    *out = value;
    return true;
}

int strcasecmp_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char ca = to_lower(a[i]), cb = to_lower(b[i]);
        if (ca != cb)
            return (unsigned char) ca - (unsigned char) cb;
        if (ca == '\0')
            return 0;
    }
    return 0;
}

char *strcat_s(char *dst, size_t cap, const char *src) {
    size_t len = strnlen(dst, cap);
    while (*src != '\0' && len + 1 < cap)
        dst[len++] = *src++;
    if (len < cap)
        dst[len] = '\0';
    return dst;
}

/* ================================ FILE SYSTEM ====================================== */

static int8_t fs_call(uint32_t service, struct EXT2DriverRequest *request) {
    int8_t retcode = -1;
    syscall(service, (uint32_t) request, (uint32_t) &retcode, 0);
    return retcode;
}

int8_t fs_read(uint32_t parent, const char *name, uint8_t name_len, void *buf, uint32_t buf_size) {
    struct EXT2DriverRequest request = {
        .buf = buf, .name = (char*) name, .name_len = name_len, .parent_inode = parent, .buffer_size = buf_size,
    };
    return fs_call(SYS_FS_READ, &request);
}

int8_t fs_write(uint32_t parent, const char *name, uint8_t name_len, const void *buf, uint32_t size) {
    struct EXT2DriverRequest request = {
        .buf = (void*) buf, .name = (char*) name, .name_len = name_len, .parent_inode = parent,
        .buffer_size = size, .is_directory = false,
    };
    return fs_call(SYS_FS_WRITE, &request);
}

int8_t fs_mkdir(uint32_t parent, const char *name, uint8_t name_len) {
    struct EXT2DriverRequest request = {
        .buf = NULL, .name = (char*) name, .name_len = name_len, .parent_inode = parent, .is_directory = true,
    };
    return fs_call(SYS_FS_WRITE, &request);
}

int8_t fs_delete(uint32_t parent, const char *name, uint8_t name_len, bool is_directory) {
    struct EXT2DriverRequest request = {
        .name = (char*) name, .name_len = name_len, .parent_inode = parent, .is_directory = is_directory,
    };
    return fs_call(SYS_FS_DELETE, &request);
}

int8_t fs_rename(uint32_t parent, const char *name, uint8_t name_len,
                 uint32_t new_parent, const char *new_name, uint8_t new_name_len) {
    struct SyscallRename r = {
        .parent = parent, .name = name, .name_len = name_len,
        .new_parent = new_parent, .new_name = new_name, .new_name_len = new_name_len,
    };
    int8_t retcode = -1;
    syscall(SYS_FS_RENAME, (uint32_t) &r, (uint32_t) &retcode, 0);
    return retcode;
}

int8_t fs_stat(uint32_t parent, const char *name, uint8_t name_len, struct SyscallStat *st) {
    memset(st, 0, sizeof(*st));
    st->parent   = parent;
    st->name     = name;
    st->name_len = name_len;
    int8_t retcode = -1;
    syscall(SYS_FS_STAT, (uint32_t) st, (uint32_t) &retcode, 0);
    return retcode;
}

int8_t fs_read_file(uint32_t parent, const char *name, uint8_t name_len, char **data, uint32_t *size) {
    struct SyscallStat st;
    int8_t ret = fs_stat(parent, name, name_len, &st);
    if (ret == 1)
        return 3; // read(): not found
    if (ret != 0)
        return 4; // read(): invalid parent
    if (st.type != SYSCALL_TYPE_FILE)
        return 1; // read(): not a file
    char *buf = malloc(st.size + 1);
    if (buf == NULL)
        return -1;
    ret = fs_read(parent, name, name_len, buf, st.size);
    if (ret != 0) {
        free(buf);
        return ret;
    }
    buf[st.size] = '\0';
    *data = buf;
    *size = st.size;
    return 0;
}

int8_t fs_list(uint32_t dir, struct DirEntry **entries, uint32_t *count) {
    // Probe: an empty buffer makes the driver report the size of the directory table
    struct EXT2DriverRequest request = {
        .buf = NULL, .name = ".", .name_len = 1, .parent_inode = dir, .buffer_size = 0,
    };
    int8_t ret = fs_call(SYS_FS_READ_DIRECTORY, &request);
    if (ret != -1 && ret != 0)
        return ret;
    uint32_t size = request.buffer_size;
    if (size == 0)
        return -1;
    uint8_t *table = malloc(size);
    if (table == NULL)
        return -1;
    request.buf = table;
    request.buffer_size = size;
    ret = fs_call(SYS_FS_READ_DIRECTORY, &request);
    if (ret != 0) {
        free(table);
        return ret;
    }

    // Each 512-byte block is a chain of EXT2 directory records
    uint32_t capacity = 16, n = 0;
    struct DirEntry *list = malloc(capacity * sizeof(*list));
    if (list == NULL) {
        free(table);
        return -1;
    }
    for (uint32_t block = 0; block < size / BLOCK_SIZE; block++) {
        uint8_t *b = table + block * BLOCK_SIZE;
        for (uint32_t off = 0; off + EXT2_DIR_ENTRY_HEADER <= BLOCK_SIZE;) {
            struct EXT2DirectoryEntry *e = (struct EXT2DirectoryEntry*) (b + off);
            if (e->rec_len < EXT2_DIR_ENTRY_HEADER || off + e->rec_len > BLOCK_SIZE)
                break;
            const char *name = (const char*) (b + off + EXT2_DIR_ENTRY_HEADER);
            bool dot = (e->name_len == 1 && name[0] == '.') || (e->name_len == 2 && name[0] == '.' && name[1] == '.');
            if (e->inode != 0 && e->name_len > 0 && !dot) {
                if (n == capacity) {
                    capacity *= 2;
                    struct DirEntry *bigger = realloc(list, capacity * sizeof(*list));
                    if (bigger == NULL) {
                        free(list);
                        free(table);
                        return -1;
                    }
                    list = bigger;
                }
                list[n].inode    = e->inode;
                list[n].type     = e->file_type == EXT2_FT_DIR ? SYSCALL_TYPE_DIR : SYSCALL_TYPE_FILE;
                list[n].name_len = e->name_len;
                memcpy(list[n].name, name, e->name_len);
                list[n].name[e->name_len] = '\0';
                n++;
            }
            off += e->rec_len;
        }
    }
    free(table);
    *entries = list;
    *count   = n;
    return 0;
}

bool fs_name_of(uint32_t inode, uint32_t *parent, char *name) {
    struct SyscallNameOf *q = malloc(sizeof(*q));
    if (q == NULL)
        return false;
    q->inode = inode;
    int8_t retcode = -1;
    syscall(SYS_FS_NAME_OF, (uint32_t) q, (uint32_t) &retcode, 0);
    if (retcode == 0) {
        *parent = q->parent;
        memcpy(name, q->name, sizeof(q->name));
    }
    free(q);
    return retcode == 0;
}

uint32_t fs_find(const char *name, uint8_t name_len, struct SyscallFindHit *hits, uint32_t max_hits) {
    struct SyscallFind f = {.name = name, .name_len = name_len, .hits = hits, .max_hits = max_hits, .total = 0};
    syscall(SYS_FS_FIND, (uint32_t) &f, 0, 0);
    return f.total;
}

void fs_info(struct SyscallFsInfo *info) {
    memset(info, 0, sizeof(*info));
    syscall(SYS_FS_INFO, (uint32_t) info, 0, 0);
}

void fs_check(struct SyscallFsCheck *result) {
    memset(result, 0, sizeof(*result));
    syscall(SYS_FS_CHECK, (uint32_t) result, 0, 0);
}
