#include "cosmos_libc.h"
#include "user/ulib.h"
#include "user/inflate.h"

/**
 * Hosted C library pieces for ported programs (DOOM). Files are kept in memory: fopen()
 * reads the whole file (inflating CZIP files transparently, so the 4 MiB root can hold
 * the compressed DOOM WAD), writes are stored back with the file system system calls
 * when the file is closed or flushed.
 */

int errno = 0;

/* ================================ FILES ============================================ */

enum FileKind { FILE_MEMORY, FILE_STDOUT, FILE_STDERR, FILE_STDIN };

struct CosmosFile {
    int      kind;
    uint8_t *data;
    uint32_t size;
    uint32_t cap;
    uint32_t pos;
    bool     writable;
    bool     dirty;
    bool     eof;
    uint32_t parent;
    char     name[256];
};

static struct CosmosFile std_files[3] = {
    {.kind = FILE_STDIN}, {.kind = FILE_STDOUT}, {.kind = FILE_STDERR},
};
FILE *stdin  = &std_files[0];
FILE *stdout = &std_files[1];
FILE *stderr = &std_files[2];

// Absolute path, or relative to the working directory -> parent directory inode + leaf
static bool locate(const char *path, uint32_t *parent, char *leaf) {
    char full[512];
    if (path[0] == '/') {
        snprintf(full, sizeof(full), "%s", path);
    } else {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd)))
            snprintf(cwd, sizeof(cwd), "/");
        snprintf(full, sizeof(full), "%s/%s", strcmp(cwd, "/") == 0 ? "" : cwd, path);
    }
    uint32_t dir = FS_ROOT;
    char *p = full;
    for (;;) {
        while (*p == '/')
            p++;
        char *slash = strchr(p, '/');
        if (slash == NULL)
            break;
        uint32_t len = (uint32_t) (slash - p);
        if (!(len == 1 && p[0] == '.')) {
            struct SyscallStat st;
            if (len > 255 || fs_stat(dir, p, (uint8_t) len, &st) != 0 || st.type != SYSCALL_TYPE_DIR) {
                errno = ENOENT;
                return false;
            }
            dir = st.inode;
        }
        p = slash + 1;
    }
    if (*p == '\0' || strlen(p) > 255) {
        errno = EINVAL;
        return false;
    }
    *parent = dir;
    strcpy(leaf, p);
    return true;
}

static bool store(FILE *f) {
    if (!f->dirty)
        return true;
    uint8_t len = (uint8_t) strlen(f->name);
    fs_delete(f->parent, f->name, len, false); // replace the old version
    if (fs_write(f->parent, f->name, len, f->data, f->size) != 0) {
        errno = EIO;
        return false;
    }
    f->dirty = false;
    return true;
}

FILE *fopen(const char *path, const char *mode) {
    FILE *f = calloc(1, sizeof(*f));
    if (f == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    f->kind = FILE_MEMORY;
    if (!locate(path, &f->parent, f->name)) {
        free(f);
        return NULL;
    }
    bool read_existing = mode[0] == 'r' || mode[0] == 'a';
    f->writable = mode[0] != 'r' || strchr(mode, '+') != NULL;
    if (read_existing) {
        char *data;
        uint32_t size;
        if (fs_read_file(f->parent, f->name, (uint8_t) strlen(f->name), &data, &size) != 0) {
            if (mode[0] == 'r') {
                errno = ENOENT;
                free(f);
                return NULL;
            }
        } else if (czip_is((uint8_t*) data, size)) {
            uint8_t *plain;
            uint32_t plain_size;
            bool ok = czip_unpack((uint8_t*) data, size, &plain, &plain_size);
            free(data);
            if (!ok) {
                errno = EIO;
                free(f);
                return NULL;
            }
            f->data = plain;
            f->size = f->cap = plain_size;
        } else {
            f->data = (uint8_t*) data;
            f->size = f->cap = size;
        }
    }
    if (mode[0] == 'w')
        f->dirty = true; // creates / truncates the file even when nothing is written
    if (mode[0] == 'a')
        f->pos = f->size;
    return f;
}

int fflush(FILE *f) {
    if (f == NULL || f->kind != FILE_MEMORY)
        return 0;
    return store(f) ? 0 : EOF;
}

int fclose(FILE *f) {
    if (f == NULL || f->kind != FILE_MEMORY)
        return 0;
    int r = store(f) ? 0 : EOF;
    free(f->data);
    free(f);
    return r;
}

size_t fread(void *buf, size_t size, size_t count, FILE *f) {
    if (f->kind != FILE_MEMORY || size == 0)
        return 0;
    uint32_t want = (uint32_t) (size * count), left = f->size - f->pos;
    if (want > left) {
        want = left - left % (uint32_t) size;
        f->eof = true;
    }
    memcpy(buf, f->data + f->pos, want);
    f->pos += want;
    return want / size;
}

static bool reserve(FILE *f, uint32_t need) {
    if (need <= f->cap)
        return true;
    uint32_t cap = f->cap < 256 ? 256 : f->cap;
    while (cap < need)
        cap *= 2;
    uint8_t *bigger = realloc(f->data, cap);
    if (bigger == NULL)
        return false;
    f->data = bigger;
    f->cap  = cap;
    return true;
}

size_t fwrite(const void *buf, size_t size, size_t count, FILE *f) {
    uint32_t n = (uint32_t) (size * count);
    if (f->kind == FILE_STDOUT || f->kind == FILE_STDERR) {
        fd_write_all(f->kind == FILE_STDOUT ? 1 : 2, buf, n, 0);
        return count;
    }
    if (f->kind != FILE_MEMORY || !f->writable || !reserve(f, f->pos + n))
        return 0;
    memcpy(f->data + f->pos, buf, n);
    f->pos += n;
    if (f->pos > f->size)
        f->size = f->pos;
    f->dirty = true;
    return count;
}

int fseek(FILE *f, long offset, int whence) {
    if (f->kind != FILE_MEMORY)
        return -1;
    long base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (long) f->pos : (long) f->size;
    long target = base + offset;
    if (target < 0) {
        errno = EINVAL;
        return -1;
    }
    if ((uint32_t) target > f->size && f->writable) {
        if (!reserve(f, (uint32_t) target))
            return -1;
        memset(f->data + f->size, 0, (uint32_t) target - f->size);
        f->size = (uint32_t) target;
    }
    f->pos = (uint32_t) target > f->size ? f->size : (uint32_t) target;
    f->eof = false;
    return 0;
}

long ftell(FILE *f) {
    return f->kind == FILE_MEMORY ? (long) f->pos : 0;
}

int feof(FILE *f) {
    return f->kind == FILE_MEMORY ? (f->eof || f->pos >= f->size) : 0;
}

int fgetc(FILE *f) {
    if (f->kind != FILE_MEMORY || f->pos >= f->size) {
        f->eof = true;
        return EOF;
    }
    return f->data[f->pos++];
}

char *fgets(char *s, int n, FILE *f) {
    int i = 0;
    while (i + 1 < n) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        s[i++] = (char) c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    s[i] = '\0';
    return s;
}

int fputc(int c, FILE *f) {
    char ch = (char) c;
    return fwrite(&ch, 1, 1, f) == 1 ? (unsigned char) c : EOF;
}

int putc(int c, FILE *f) {
    return fputc(c, f);
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    return fwrite(s, 1, n, f) == n ? 0 : EOF;
}

int puts(const char *s) {
    fputs(s, stdout);
    return fputc('\n', stdout);
}

int putchar(int c) {
    return fputc(c, stdout);
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char small[512];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    if (n < 0) {
        va_end(copy);
        return n;
    }
    if ((size_t) n < sizeof(small)) {
        fwrite(small, 1, (size_t) n, f);
    } else {
        char *big = malloc((size_t) n + 1);
        if (big != NULL) {
            vsnprintf(big, (size_t) n + 1, fmt, copy);
            fwrite(big, 1, (size_t) n, f);
            free(big);
        }
    }
    va_end(copy);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return n;
}

int fileno(FILE *f) {
    return f->kind == FILE_STDIN ? 0 : f->kind == FILE_STDOUT ? 1 : f->kind == FILE_STDERR ? 2 : 3;
}

void perror(const char *s) {
    fprintf(stderr, "%s: error %d\n", s, errno);
}

int remove(const char *path) {
    uint32_t parent;
    char leaf[256];
    if (!locate(path, &parent, leaf))
        return -1;
    uint8_t len = (uint8_t) strlen(leaf);
    if (fs_delete(parent, leaf, len, false) == 0 || fs_delete(parent, leaf, len, true) == 0)
        return 0;
    errno = ENOENT;
    return -1;
}

int rename(const char *old_path, const char *new_path) {
    uint32_t p1, p2;
    char l1[256], l2[256];
    if (!locate(old_path, &p1, l1) || !locate(new_path, &p2, l2))
        return -1;
    fs_delete(p2, l2, (uint8_t) strlen(l2), false); // rename() replaces the target
    return fs_rename(p1, l1, (uint8_t) strlen(l1), p2, l2, (uint8_t) strlen(l2)) == 0 ? 0 : -1;
}

int mkdir(const char *path, mode_t mode) {
    (void) mode;
    uint32_t parent;
    char leaf[256];
    if (!locate(path, &parent, leaf))
        return -1;
    int8_t r = fs_mkdir(parent, leaf, (uint8_t) strlen(leaf));
    if (r == 1)
        errno = EEXIST;
    return r == 0 ? 0 : -1;
}

int access(const char *path, int mode) {
    (void) mode;
    uint32_t parent;
    char leaf[256];
    struct SyscallStat st;
    return locate(path, &parent, leaf) && fs_stat(parent, leaf, (uint8_t) strlen(leaf), &st) == 0 ? 0 : -1;
}

/* ================================ SCANF (subset) =================================== */

static int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 99;
}

unsigned long strtoul(const char *s, char **end, int base) {
    while (isspace((unsigned char) *s))
        s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }
    unsigned long v = 0;
    while (digit_value(*s) < base)
        v = v * (unsigned long) base + (unsigned long) digit_value(*s++);
    if (end != NULL)
        *end = (char*) s;
    return v;
}

long strtol(const char *s, char **end, int base) {
    while (isspace((unsigned char) *s))
        s++;
    bool neg = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    long v = (long) strtoul(s, end, base);
    return neg ? -v : v;
}

long atol(const char *s) {
    return strtol(s, NULL, 10);
}

double atof(const char *s) {
    while (isspace((unsigned char) *s))
        s++;
    bool neg = *s == '-';
    if (*s == '-' || *s == '+')
        s++;
    double v = 0, scale = 1;
    while (isdigit((unsigned char) *s))
        v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char) *s)) {
            scale /= 10;
            v += (*s++ - '0') * scale;
        }
    }
    return neg ? -v : v;
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int assigned = 0;
    while (*fmt) {
        if (isspace((unsigned char) *fmt)) {
            while (isspace((unsigned char) *s))
                s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt)
                break;
            s++;
            fmt++;
            continue;
        }
        fmt++;
        char conv = *fmt++;
        if (conv != 'c')
            while (isspace((unsigned char) *s))
                s++;
        if (*s == '\0')
            break;
        char *end;
        if (conv == 'd' || conv == 'i' || conv == 'x' || conv == 'o' || conv == 'u') {
            int base = conv == 'd' || conv == 'u' ? 10 : conv == 'x' ? 16 : conv == 'o' ? 8 : 0;
            long v = conv == 'd' || conv == 'i' ? strtol(s, &end, base) : (long) strtoul(s, &end, base);
            if (end == s)
                break;
            *va_arg(ap, int*) = (int) v;
            s = end;
        } else if (conv == 's') {
            char *out = va_arg(ap, char*);
            while (*s && !isspace((unsigned char) *s))
                *out++ = *s++;
            *out = '\0';
        } else if (conv == 'c') {
            *va_arg(ap, char*) = *s++;
        } else {
            break;
        }
        assigned++;
    }
    va_end(ap);
    return assigned;
}

/* ================================ STDLIB / STRING / CTYPE ========================== */

static void (*exit_handlers[8])(void);
static int exit_handler_count = 0;

int atexit(void (*fn)(void)) {
    if (exit_handler_count == 8)
        return -1;
    exit_handlers[exit_handler_count++] = fn;
    return 0;
}

void abort(void) {
    fprintf(stderr, "abort()\n");
    exit(134);
}

void cosmos_assert_fail(const char *expr, const char *file, int line) {
    fprintf(stderr, "assertion failed: %s (%s:%d)\n", expr, file, line);
    abort();
}

int abs(int x) {
    return x < 0 ? -x : x;
}

long labs(long x) {
    return x < 0 ? -x : x;
}

int system(const char *command) {
    (void) command;
    return -1; // no shell from inside a program
}

static uint32_t rand_state = 1;
int rand(void) {
    rand_state = rand_state * 1103515245u + 12345u;
    return (int) ((rand_state >> 16) & RAND_MAX);
}

void srand(unsigned int seed) {
    rand_state = seed;
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    // Shell sort with byte swaps: no allocation, fine for the small tables programs sort
    uint8_t *a = base;
    for (size_t gap = n / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < n; i++) {
            for (size_t j = i; j >= gap && cmp(a + (j - gap) * size, a + j * size) > 0; j -= gap) {
                for (size_t k = 0; k < size; k++) {
                    uint8_t t = a[(j - gap) * size + k];
                    a[(j - gap) * size + k] = a[j * size + k];
                    a[j * size + k] = t;
                }
            }
        }
    }
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++) != '\0') {
    }
    return dst;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    while (n-- && *src)
        *d++ = *src++;
    *d = '\0';
    return dst;
}

char *strstr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (; *haystack; haystack++)
        if (strncmp(haystack, needle, n) == 0)
            return (char*) haystack;
    return n == 0 ? (char*) haystack : NULL;
}

int toupper(int c) {
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

int tolower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char) a[i]), y = tolower((unsigned char) b[i]);
        if (x != y || x == 0)
            return x - y;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    return strncasecmp(a, b, (size_t) -1);
}

char *strerror(int err) {
    static char text[32];
    snprintf(text, sizeof(text), "error %d", err);
    return text;
}

int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isalpha(int c)  { return isupper(c) || islower(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isprint(int c)  { return c >= 32 && c < 127; }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int ispunct(int c)  { return isprint(c) && !isalnum(c) && c != ' '; }

double fabs(double x) {
    return x < 0 ? -x : x;
}

/* ================================ TIME ============================================= */

int usleep(uint32_t usec) {
    sleep_ms(usec / 1000);
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    sleep_ms(seconds * 1000);
    return 0;
}

int isatty(int fd) {
    return fd_type(fd) == SYSCALL_FD_CONSOLE;
}

int gettimeofday(struct timeval *tv, void *tz) {
    (void) tz;
    uint32_t ms = uptime_ms();
    tv->tv_sec  = (long) (ms / 1000);
    tv->tv_usec = (long) (ms % 1000) * 1000;
    return 0;
}

/* ================================ 64-BIT DIVISION (libgcc) ========================= */
// gcc calls these for 64-bit / and % on i386 (DOOM's FixedDiv); shift-subtract division.

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem) {
    if (d == 0) {
        if (rem) *rem = 0;
        return 0;
    }
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t) 1 << i;
        }
    }
    if (rem) *rem = r;
    return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d) {
    return __udivmoddi4(n, d, NULL);
}

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    uint64_t r;
    __udivmoddi4(n, d, &r);
    return r;
}

int64_t __divdi3(int64_t n, int64_t d) {
    bool neg = (n < 0) != (d < 0);
    uint64_t q = __udivmoddi4(n < 0 ? -(uint64_t) n : (uint64_t) n, d < 0 ? -(uint64_t) d : (uint64_t) d, NULL);
    return neg ? -(int64_t) q : (int64_t) q;
}

int64_t __moddi3(int64_t n, int64_t d) {
    uint64_t r;
    __udivmoddi4(n < 0 ? -(uint64_t) n : (uint64_t) n, d < 0 ? -(uint64_t) d : (uint64_t) d, &r);
    return n < 0 ? -(int64_t) r : (int64_t) r;
}
