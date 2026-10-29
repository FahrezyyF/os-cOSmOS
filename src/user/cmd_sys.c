#include "user/shell.h"

/* ================================ shell built-ins ================================== */

int cmd_help(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc > 1) {
        for (uint32_t i = 0; i < shell_command_count; i++) {
            if (strcmp(shell_commands[i].name, argv[1]) == 0) {
                out_color(C_YELLOW);
                out_printf("%s %s\n", shell_commands[i].name, shell_commands[i].usage);
                out_color(C_LIGHT_GRAY);
                out_printf("  %s\n", shell_commands[i].help);
                return 0;
            }
        }
        err_printf("help: no command named %s\n", argv[1]);
        return 1;
    }
    out_color(C_WHITE);
    out_printf("cOSmOS shell commands (all run in user mode, through int 0x30 system calls):\n");
    bool wide = screen_cols >= 100;
    for (uint32_t i = 0; i < shell_command_count; i++) {
        const struct Command *c = &shell_commands[i];
        out_color(C_YELLOW);
        out_printf("  %-9s", c->name);
        out_color(C_DARK_GRAY);
        if (wide)
            out_printf("%-30s", c->usage);
        out_color(C_LIGHT_GRAY);
        out_printf("%s\n", c->help);
    }
    out_color(C_WHITE);
    out_printf("Syntax: a | b (pipe), > / >> / < (redirect), ; && || (sequence), 'quotes', $VAR, $?, *.txt\n");
    out_printf("Keys  : Tab complete, Up/Down history, Left/Right/Home/End edit, PgUp/PgDn or wheel scroll,\n");
    out_printf("        Ctrl+C cancel, Ctrl+L clear, Ctrl+U / Ctrl+K / Ctrl+W delete\n");
    out_color(C_LIGHT_GRAY);
    return 0;
}

int cmd_clear(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    out_flush();
    console_clear_screen();
    return 0;
}

int cmd_history(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    shell_print_history();
    return 0;
}

int cmd_sh(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc < 2) {
        err_printf("usage: %s <script.sh> [args]...\n", argv[0]);
        return 2;
    }
    return shell_run_script(argv[1], argc - 1, argv + 1);
}

int cmd_set(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc == 1) {
        shell_print_vars();
        return 0;
    }
    // set NAME value...  (the words are joined with spaces)
    uint32_t len = 1;
    for (int i = 2; i < argc; i++)
        len += (uint32_t) strlen(argv[i]) + 1;
    char *value = malloc(len);
    if (value == NULL)
        return 1;
    value[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2)
            strcat_s(value, len, " ");
        strcat_s(value, len, argv[i]);
    }
    shell_set_var(argv[1], value);
    free(value);
    return 0;
}

int cmd_unset(int argc, char **argv, struct Input *in) {
    (void) in;
    for (int i = 1; i < argc; i++)
        shell_unset_var(argv[i]);
    return 0;
}

int cmd_exit(int argc, char **argv, struct Input *in) {
    (void) in;
    shell_request_exit(argc > 1 ? atoi(argv[1]) : 0);
    return argc > 1 ? atoi(argv[1]) : 0;
}

int cmd_true(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    return 0;
}

int cmd_false(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    return 1;
}

/* ================================ time & sound ===================================== */

int cmd_date(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    struct SyscallTime t;
    get_time(&t);
    out_printf("%04u-%02u-%02u %02u:%02u:%02u UTC (unix time %u)\n", t.year, t.month, t.day, t.hour, t.minute,
               t.second, t.unix_time);
    return 0;
}

int cmd_uptime(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    uint32_t ms = uptime_ms(), s = ms / 1000;
    out_printf("up %u:%02u:%02u (%u ms)\n", s / 3600, (s / 60) % 60, s % 60, ms);
    return 0;
}

int cmd_sleep(int argc, char **argv, struct Input *in) {
    (void) in;
    uint32_t ms;
    if (argc != 2 || !parse_uint(argv[1], &ms)) {
        err_printf("usage: sleep <milliseconds>\n");
        return 2;
    }
    out_flush();
    sleep_ms(ms);
    return 0;
}

int cmd_beep(int argc, char **argv, struct Input *in) {
    (void) in;
    uint32_t hz = 880, ms = 150;
    if ((argc > 1 && !parse_uint(argv[1], &hz)) || (argc > 2 && !parse_uint(argv[2], &ms))) {
        err_printf("usage: beep [Hz] [ms]\n");
        return 2;
    }
    beep(hz, ms);
    return 0;
}

/* ================================ memory =========================================== */

static uint32_t percent(uint32_t part, uint32_t whole) {
    return whole == 0 ? 0 : part * 100 / whole;
}

int cmd_free(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    struct SyscallMemInfo m;
    get_meminfo(&m);
    uint32_t usable = m.total_frames - m.kernel_frames;
    uint32_t used   = usable - m.free_frames;
    out_color(C_WHITE);
    out_printf("            %10s %10s %10s\n", "total", "used", "free");
    out_color(C_LIGHT_GRAY);
    out_printf("RAM pool  : %7u KiB %7u KiB %7u KiB   (%u%% used, user pages from 0x400000)\n", usable * 4,
               used * 4, m.free_frames * 4, percent(used, usable));
    if (m.swap_total > 0)
        out_printf("Swap      : %7u KiB %7u KiB %7u KiB   (%u%% used, on the root disk after 4 MiB)\n",
                   m.swap_total * 4, m.swap_used * 4, (m.swap_total - m.swap_used) * 4, percent(m.swap_used, m.swap_total));
    else
        out_printf("Swap      : none (the disk image has no swap partition: `make disk`)\n");
    out_printf("Kernel    : first %u KiB reserved = image + stack %u KiB + pool %u KiB (%u KiB free);\n",
               m.kernel_frames * 4, m.kernel_image_frames * 4, (m.kernel_frames - m.kernel_image_frames) * 4,
               m.kernel_pool_free * 4);
    out_printf("            heap %u KiB, page tables %u (pool first, then the RAM pool)\n", m.kernel_heap_pages * 4,
               m.page_tables);
    out_printf("This shell: %u resident pages (%u KiB), program break 0x%08x\n", m.user_pages, m.user_pages * 4,
               m.user_break);
    out_printf("Paging    : %u page faults = %u demand-zero + %u swap-in, %u swap-out, %u refused\n", m.page_faults,
               m.demand_zero, m.swap_ins, m.swap_outs, m.failed_faults);
    out_color(C_DARK_GRAY);
    out_printf("RAM managed: %u KiB (SYSTEM_MEMORY_MB = %u)\n", m.total_frames * 4, m.system_memory_mb);
    out_color(C_LIGHT_GRAY);
    return 0;
}

static void print_heap(const char *title, uint32_t committed, uint32_t used, uint32_t used_chunks, uint32_t free_bytes,
                       uint32_t free_chunks, uint32_t largest, uint32_t allocs, uint32_t frees) {
    out_color(C_YELLOW);
    out_printf("%s\n", title);
    out_color(C_LIGHT_GRAY);
    out_printf("  committed %u KiB, in use %u B in %u chunks, free %u B in %u chunks (largest %u B)\n",
               committed / 1024, used, used_chunks, free_bytes, free_chunks, largest);
    out_printf("  %u allocations, %u frees\n", allocs, frees);
}

int cmd_heap(int argc, char **argv, struct Input *in) {
    (void) in;
    struct HeapStats u;
    user_heap_stats(&u);
    print_heap("User heap (malloc / free over the sbrk system call, from 0x00400000):", u.committed_bytes,
               u.used_bytes, u.used_chunks, u.free_bytes, u.free_chunks, u.largest_free, u.alloc_count, u.free_count);
    out_printf("  %u sbrk grows, %u trims back to the kernel, %u bad / double frees caught\n", u.grow_count,
               u.trim_count, u.errors);
    struct SyscallHeapInfo k;
    get_kheap_info(&k);
    print_heap("Kernel heap (kmalloc / kfree, mapped on demand at 0xE0000000):", k.committed_bytes, k.used_bytes,
               k.used_chunks, k.free_bytes, k.free_chunks, k.largest_free, k.alloc_count, k.free_count);
    out_color(C_DARK_GRAY);
    out_printf("Allocator: boundary tags, 28 segregated free lists, split + coalesce, in-place realloc\n");
    out_color(C_LIGHT_GRAY);
    if (argc > 1 && strcmp(argv[1], "check") == 0) {
        bool ok = user_heap_check();
        out_color(ok ? C_LIGHT_GREEN : C_LIGHT_RED);
        out_printf("heap_check(user heap): %s\n", ok ? "consistent" : "CORRUPTED");
        out_color(C_LIGHT_GRAY);
        return ok ? 0 : 1;
    }
    return 0;
}

#define MEMTEST_BLOCK (256u * 1024u)

static bool cancel_requested(void) {
    return getchar_nowait() == KEY_CTRL('c');
}

static void progress(const char *what, uint32_t done, uint32_t total) {
    if (!out_is_console())
        return;
    uint32_t width = 30, filled = total ? done * width / total : width;
    char bar[40];
    for (uint32_t i = 0; i < width; i++)
        bar[i] = i < filled ? '#' : '.';
    bar[width] = '\0';
    out_printf("\r  %-8s [%s] %3u%%", what, bar, percent(done, total));
    out_flush();
}

int cmd_memtest(int argc, char **argv, struct Input *in) {
    (void) in;
    uint32_t mib;
    if (argc != 2 || !parse_uint(argv[1], &mib) || mib == 0 || mib > 1024) {
        err_printf("usage: memtest <MiB>   (try more than the free RAM in `free` to watch swapping)\n");
        return 2;
    }
    struct SyscallMemInfo before, after;
    get_meminfo(&before);
    uint32_t start_ms = uptime_ms();
    uint32_t wanted = mib * (1024 * 1024 / MEMTEST_BLOCK);
    uint32_t **blocks = calloc(wanted, sizeof(uint32_t*));
    if (blocks == NULL) {
        err_printf("memtest: out of memory\n");
        return 1;
    }
    out_printf("memtest: %u MiB in %u KiB blocks (RAM pool has %u KiB free, swap %u KiB free) - Ctrl+C stops\n",
               mib, MEMTEST_BLOCK / 1024, before.free_frames * 4, (before.swap_total - before.swap_used) * 4);

    // 1. allocate + fill every word with a pattern derived from its address
    uint32_t got = 0;
    bool cancelled = false;
    const uint32_t words = MEMTEST_BLOCK / 4;
    for (; got < wanted; got++) {
        uint32_t *b = malloc(MEMTEST_BLOCK);
        if (b == NULL)
            break;
        for (uint32_t w = 0; w < words; w++)
            b[w] = ((uint32_t) &b[w] * 2654435761u) ^ 0xC05305u;
        blocks[got] = b;
        progress("write", got + 1, wanted);
        if (cancel_requested()) {
            got++;
            cancelled = true;
            break;
        }
    }
    if (out_is_console())
        out_putc('\n');
    if (got < wanted && !cancelled)
        note_printf(C_YELLOW, "memtest: allocation stopped at %u KiB (RAM + swap are full)\n", got * (MEMTEST_BLOCK / 1024));

    // 2. read everything back: pages that were swapped out come back in
    uint32_t errors = 0;
    for (uint32_t i = 0; i < got && !cancelled; i++) {
        uint32_t *b = blocks[i];
        for (uint32_t w = 0; w < words; w++)
            if (b[w] != (((uint32_t) &b[w] * 2654435761u) ^ 0xC05305u))
                errors++;
        progress("verify", i + 1, got);
        if (cancel_requested())
            cancelled = true;
    }
    if (out_is_console())
        out_putc('\n');
    get_meminfo(&after);
    for (uint32_t i = 0; i < got; i++)
        free(blocks[i]);
    free(blocks);

    uint32_t elapsed = uptime_ms() - start_ms;
    out_color(errors == 0 ? C_LIGHT_GREEN : C_LIGHT_RED);
    out_printf("memtest: %u KiB %s, %u bad words%s, %u ms\n", got * (MEMTEST_BLOCK / 1024),
               cancelled ? "tested before Ctrl+C" : "written and verified", errors,
               errors == 0 ? " (OK)" : " (MEMORY CORRUPTION)", elapsed);
    out_color(C_LIGHT_GRAY);
    out_printf("paging : +%u page faults (+%u demand-zero, +%u swap-in), +%u swap-out, peak swap use %u KiB\n",
               after.page_faults - before.page_faults, after.demand_zero - before.demand_zero,
               after.swap_ins - before.swap_ins, after.swap_outs - before.swap_outs, after.swap_used * 4);
    return errors == 0 ? 0 : 1;
}

/* ================================ system =========================================== */

int cmd_sysinfo(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    struct SyscallSysInfo s;
    get_sysinfo(&s);
    out_color(C_YELLOW);
    out_printf("%s %s (Milestone 3: paging, swap, user mode, shell)\n", s.os_name, s.version);
    out_color(C_LIGHT_GRAY);
    if (s.graphics)
        out_printf("  display : BGA %ux%ux32 linear framebuffer (phys 0x%08x -> virt 0xF0000000), id 0x%04x, %ux%u cells\n",
                   s.width, s.height, s.lfb_address, s.bga_version, s.cols, s.rows);
    else
        out_printf("  display : VGA text mode %ux%u (0xB8000 -> virt 0xC00B8000)\n", s.cols, s.rows);
    out_printf("  memory  : %u KiB managed by paging (GRUB reported %u KiB)\n", s.ram_kb, s.detected_ram_kb);
    out_printf("  kernel  : higher half at 0xC0100000, image %u KiB incl. %u KiB stack\n", s.kernel_image_kb,
               s.kernel_stack_kb);
    out_printf("  root fs : %s on %s (%u sectors), swap %s\n", s.fs_name, s.disk_model[0] ? s.disk_model : "?",
               s.disk_sectors, s.swap ? "on" : "off");
    out_printf("  mouse   : %s\n", s.mouse ? (s.mouse_wheel ? "PS/2 with wheel" : "PS/2") : "none");
    uint32_t sec = s.uptime_ms / 1000;
    out_printf("  uptime  : %u:%02u:%02u\n", sec / 3600, (sec / 60) % 60, sec % 60);
    out_printf("  boot    : \"%s\"\n", s.cmdline);
    return 0;
}

int cmd_crash(int argc, char **argv, struct Input *in) {
    (void) in;
    const char *kind = argc > 1 ? argv[1] : "kernel";
    out_flush();
    note_printf(C_YELLOW, "crash: %s - the kernel catches the fault and starts a new shell\n", kind);
    if (strcmp(kind, "gpf") == 0) {
        __asm__ volatile("cli"); // privileged instruction in ring 3: #GP
    } else if (strcmp(kind, "div") == 0) {
        volatile int zero = 0;
        volatile int x = 1 / zero; // #DE
        (void) x;
    } else {
        *(volatile uint32_t*) 0xC0100000u = 0xDEAD; // kernel memory is supervisor-only: #PF
    }
    return 1;
}

int cmd_reboot(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    out_flush();
    power(SYS_POWER_REBOOT);
}

int cmd_shutdown(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    out_flush();
    note_printf(C_YELLOW, "Powering off...\n");
    power(SYS_POWER_SHUTDOWN);
}
