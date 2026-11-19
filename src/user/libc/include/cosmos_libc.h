#ifndef _COSMOS_LIBC_H
#define _COSMOS_LIBC_H

/**
 * Minimal hosted C library for ports of ordinary C programs (DOOM) to cOSmOS. Every
 * standard header in this folder includes this one. string / snprintf come from the
 * kernel's own src/stdlib, the rest (stdio over the file system, ctype, stdlib) from
 * src/user/libc/libc.c on top of the cOSmOS system calls.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "header/stdlib/string.h"
#include "header/stdlib/format.h"

#define EOF       (-1)
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2
#define BUFSIZ    512
#define FILENAME_MAX 256
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX  0x7FFF

typedef struct CosmosFile FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* stdio */
FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
size_t fread(void *buf, size_t size, size_t count, FILE *f);
size_t fwrite(const void *buf, size_t size, size_t count, FILE *f);
int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
int    feof(FILE *f);
int    fflush(FILE *f);
int    fputs(const char *s, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
char  *fgets(char *s, int n, FILE *f);
int    fgetc(FILE *f);
int    printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    fprintf(FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vprintf(const char *fmt, va_list ap);
int    sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    vsprintf(char *buf, const char *fmt, va_list ap);
int    sscanf(const char *s, const char *fmt, ...);
int    puts(const char *s);
int    putchar(int c);
int    remove(const char *path);
int    rename(const char *old_path, const char *new_path);
int    fileno(FILE *f);
void   perror(const char *s);

/* stdlib */
void  *malloc(size_t size);
void  *calloc(size_t count, size_t size);
void  *realloc(void *ptr, size_t size);
void   free(void *ptr);
void   exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
int    atexit(void (*fn)(void));
const char *getenv(const char *name);
int    atoi(const char *s);
long   atol(const char *s);
double atof(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
int    abs(int x);
long   labs(long x);
int    system(const char *command);
int    rand(void);
void   srand(unsigned int seed);
void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

/* string extras */
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
char  *strstr(const char *haystack, const char *needle);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strerror(int err);

/* ctype */
int toupper(int c);
int tolower(int c);
int isspace(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isprint(int c);
int isupper(int c);
int islower(int c);
int isxdigit(int c);
int ispunct(int c);

/* math (only what the port needs; no FPU state is assumed between calls) */
double fabs(double x);

/* unistd / sys */
typedef int32_t ssize_t;
typedef int32_t off_t;
typedef uint32_t mode_t;
typedef int32_t pid_t;
int    usleep(uint32_t usec);
unsigned int sleep(unsigned int seconds);
int    isatty(int fd);
int    mkdir(const char *path, mode_t mode);
int    access(const char *path, int mode);
#define F_OK 0
#define R_OK 4
#define W_OK 2

struct timeval {
    long tv_sec;
    long tv_usec;
};
int gettimeofday(struct timeval *tv, void *tz);

/* errno */
extern int errno;
#define ENOENT 2
#define EIO    5
#define ENOMEM 12
#define EEXIST 17
#define EISDIR 21
#define EINVAL 22
#define ERANGE 34

#endif
