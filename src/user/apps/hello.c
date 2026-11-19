#include "user/ulib.h"

/**
 * hello - the book's "Hello World!" external application (book 4.3.4).
 * A separate process started with `exec hello` (or just `hello`, found through PATH);
 * it uses the linker script + crt0.s entry point and only talks to the kernel through
 * system calls. Arguments show the C runtime at work: `hello a b c`.
 */
int main(int argc, char *argv[]) {
    printf_color(C_LIGHT_GREEN, "Hello World!\n");
    printf("I am process %u (parent %u), started as \"%s\" with %d argument%s:\n", getpid(), getppid(),
           argv[0], argc - 1, argc - 1 == 1 ? "" : "s");
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    const char *path = getenv("PATH");
    printf("  PATH    = %s\n", path ? path : "(not set)");
    return 0;
}
