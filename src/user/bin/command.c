#include "user/shell.h"

/**
 * One separate shell command (book 4.3.6). The Makefile compiles this file once per
 * command with -DCOMMAND_FUNCTION=cmd_<name>; the linker (--gc-sections) keeps only the
 * code that command needs. The program:
 *   - gets argc / argv from crt0.s (bonus: C runtime),
 *   - takes its working directory from the kernel (the shell's `cd` sets it),
 *   - reads the previous pipeline stage from stdin when stdin is a pipe,
 *   - writes to stdout (the console, or a pipe the shell reads),
 *   - returns its exit status from main(); crt0.s ends the process (no zombie: the kernel
 *     frees everything at once and hands the status to the waiting shell).
 */
#ifndef COMMAND_FUNCTION
#error "compile with -DCOMMAND_FUNCTION=cmd_<name>"
#endif

int main(int argc, char **argv) {
    shlib_init();
    struct Input in;
    char *owned;
    if (!shlib_read_stdin(&in, &owned)) {
        err_printf("%s: out of memory reading stdin\n", argv[0]);
        return 1;
    }
    int status = COMMAND_FUNCTION(argc, argv, &in);
    out_flush();
    out_color(C_LIGHT_GRAY);
    free(owned);
    return status;
}
