#include "user/shell.h"
#include "header/process/cex.h"
#include "header/process/elf.h"

/**
 * Process management commands (book 4.3.2): ps, kill, nice, pmap, sched, execinfo.
 * Each is also a separate program in /bin (book 4.3.6).
 */

static const char *priority_name(uint8_t p) {
    return p == 0 ? "low" : (p == 2 ? "high" : "normal");
}

int cmd_ps(int argc, char **argv, struct Input *in) {
    (void) in;
    bool flags[128];
    int first = parse_flags(argc, argv, "l", flags);
    if (first < 0)
        return 2;
    struct SyscallProcessInfo *list = calloc(32, sizeof(*list));
    if (list == NULL)
        return 1;
    uint32_t n = process_list(list, 32);
    if (n > 32)
        n = 32;
    for (uint32_t i = 1; i < n; i++) { // by PID (the kernel lists them in PCB slot order)
        struct SyscallProcessInfo key = list[i];
        uint32_t j = i;
        for (; j > 0 && list[j - 1].pid > key.pid; j--)
            list[j] = list[j - 1];
        list[j] = key;
    }
    uint32_t me = getpid();
    if (flags['l'])
        out_printf("  PID  PPID STATE    PRIO   FMT   RES  SWAP  PEAK   HEAP      CPU  SWITCH  COMMAND\n");
    else
        out_printf("  PID  PPID STATE    MEM(KiB)  CPU(ms)  COMMAND\n");
    for (uint32_t i = 0; i < n && i < 32; i++) {
        struct SyscallProcessInfo *p = &list[i];
        char mark = p->pid == me ? '*' : (p->foreground ? '+' : (p->detached ? '&' : ' '));
        if (out_is_console())
            out_color(p->init ? C_LIGHT_CYAN : (p->foreground ? C_LIGHT_GREEN : C_LIGHT_GRAY));
        if (flags['l'])
            out_printf("%5u %5u %-8s %-6s %-4s %5u %5u %5u %6uK %8u %7u %c%s\n", p->pid, p->ppid, p->state,
                       priority_name(p->priority), p->format, p->pages_resident, p->pages_swapped, p->peak_pages,
                       p->heap_bytes / 1024, p->cpu_ms, p->switches, mark, p->cmdline);
        else
            out_printf("%5u %5u %-8s %8u %8u  %c%s\n", p->pid, p->ppid, p->state, p->pages * 4, p->cpu_ms, mark,
                       p->cmdline);
    }
    out_color(C_LIGHT_GRAY);
    if (out_is_console())
        note_printf(C_DARK_GRAY, "%u processes. * = this ps, + = owns the keyboard, & = background; `ps -l` for details\n", n);
    free(list);
    return 0;
}

int cmd_kill(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc < 2) {
        err_printf("usage: kill <pid>...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        uint32_t pid;
        if (!parse_uint(argv[i], &pid) || pid == 0) {
            err_printf("kill: %s: not a process id\n", argv[i]);
            status = 1;
            continue;
        }
        int8_t r = process_kill(pid);
        if (r == 1) {
            err_printf("kill: (%u): no such process\n", pid);
            status = 1;
        } else if (r == 2) {
            err_printf("kill: (%u): init (the shell) cannot be killed; use `exit`\n", pid);
            status = 1;
        } else if (out_is_console()) {
            note_printf(C_DARK_GRAY, "killed %u\n", pid);
        }
    }
    return status;
}

int cmd_nice(int argc, char **argv, struct Input *in) {
    (void) in;
    uint32_t pid, prio = 1;
    if (argc != 3 || !parse_uint(argv[1], &pid) ||
        !(strcmp(argv[2], "low") == 0 || strcmp(argv[2], "normal") == 0 || strcmp(argv[2], "high") == 0 ||
          (parse_uint(argv[2], &prio) && prio <= 2))) {
        err_printf("usage: nice <pid> <low|normal|high|0-2>\n");
        return 2;
    }
    if (strcmp(argv[2], "low") == 0) prio = 0;
    if (strcmp(argv[2], "normal") == 0) prio = 1;
    if (strcmp(argv[2], "high") == 0) prio = 2;
    if (process_set_priority(pid, (uint8_t) prio) != 0) {
        err_printf("nice: (%u): no such process\n", pid);
        return 1;
    }
    struct SyscallSchedInfo s;
    sched_info(&s);
    out_printf("pid %u: priority %s (time slice %u ms)\n", pid, priority_name((uint8_t) prio), s.slice_ms[prio]);
    return 0;
}

int cmd_pmap(int argc, char **argv, struct Input *in) {
    (void) in;
    bool flags[128];
    int first = parse_flags(argc, argv, "a", flags);
    uint32_t pid = 0;
    if (first < 0 || (first < argc && !parse_uint(argv[first], &pid))) {
        err_printf("usage: pmap [-a] [pid]   (-a: dump the whole page list)\n");
        return 2;
    }
    struct SyscallProcessMemory *m = calloc(1, sizeof(*m));
    uint32_t list_max = 4096;
    uint32_t *list = malloc(list_max * sizeof(uint32_t));
    if (m == NULL || list == NULL) {
        free(m);
        free(list);
        return 1;
    }
    m->pid = pid;
    m->page_list = list;
    m->page_list_max = list_max;
    if (process_memory(m) != 0) {
        err_printf("pmap: (%u): no such process\n", pid);
        free(m);
        free(list);
        return 1;
    }
    out_printf("pid %u  %s  (%s executable, entry 0x%08x)\n", m->pid, m->name, m->format, m->entry);
    out_printf("  REGION   START      END        SIZE     PERM\n");
    for (uint32_t i = 0; i < m->region_count && i < SYSCALL_REGION_MAX; i++)
        out_printf("  %-7s  0x%08x 0x%08x %6uK   %c%c%c\n", m->regions[i].kind, m->regions[i].start, m->regions[i].end,
                   (m->regions[i].end - m->regions[i].start) / 1024, (m->regions[i].flags & 1) ? 'r' : '-',
                   (m->regions[i].flags & 2) ? 'w' : '-', (m->regions[i].flags & 4) ? 'x' : '-');
    out_printf("  heap     0x%08x 0x%08x %6uK   rw-   (grows with sbrk/malloc)\n", m->heap_base, m->heap_break,
               (m->heap_break - m->heap_base) / 1024);
    out_printf("Dynamic page list (PCB memory.virtual_addr_used, kernel heap @0x%08x):\n", m->page_list_address);
    out_printf("  %u pages mapped (%u resident, %u in swap), capacity %u, peak %u\n", m->pages, m->pages_resident,
               m->pages_swapped, m->page_list_capacity, m->peak_pages);
    uint32_t show = flags['a'] ? m->page_list_copied : (m->page_list_copied < 16 ? m->page_list_copied : 16);
    for (uint32_t i = 0; i < show; i++)
        out_printf("%s0x%08x%s", i % 6 == 0 ? "  " : " ", list[i], (i % 6 == 5 || i + 1 == show) ? "\n" : "");
    if (show < m->page_list_copied)
        out_printf("  ... (%u more, `pmap -a` lists all)\n", m->page_list_copied - show);
    free(m);
    free(list);
    return 0;
}

int cmd_sched(int argc, char **argv, struct Input *in) {
    (void) argc;
    (void) argv;
    (void) in;
    struct SyscallSchedInfo s;
    sched_info(&s);
    uint32_t total = s.idle_ms + s.busy_ms;
    out_printf("scheduler      : %s, preemptive (IRQ0 at %u Hz)\n", s.algorithm, s.timer_hz);
    out_printf("time slices    : low %u ms, normal %u ms, high %u ms\n", s.slice_ms[0], s.slice_ms[1], s.slice_ms[2]);
    out_printf("processes      : %u of %u running, %u created since boot, init restarted %u times\n",
               s.process_count, s.process_max, s.created_total, s.init_respawns);
    out_printf("context switch : %u (%u preemptions, %u blocking system calls)\n", s.context_switches,
               s.preemptions, s.blocks);
    out_printf("cpu            : busy %u ms, idle %u ms (%u%% idle)\n", s.busy_ms, s.idle_ms,
               total ? s.idle_ms * 100 / total : 0);
    out_printf("resources      : %u pipes, %u of %u page directories\n", s.pipes_used, s.page_directories_used,
               s.page_directories_max);
    return 0;
}

/* -- execinfo: dump the header of an executable (CEX, ELF or flat) -- */

int cmd_execinfo(int argc, char **argv, struct Input *in) {
    (void) in;
    if (argc != 2) {
        err_printf("usage: execinfo <program>\n");
        return 2;
    }
    struct Resolved r;
    const char *path = argv[1];
    char *data;
    uint32_t len;
    if (strchr(path, '/') == NULL && !(resolve_path(path, &r, NULL, true) && r.exists) && shell_find_program(path, &r)) {
        if (fs_read_file(r.parent, r.name, (uint8_t) strlen(r.name), &data, &len) != 0) {
            err_printf("execinfo: cannot read %s\n", path);
            return 1;
        }
    } else if (!read_whole_file(path, &data, &len, "execinfo")) {
        return 1;
    }
    const uint8_t *f = (const uint8_t*) data;
    if (cex_has_magic(f, len) && len >= sizeof(struct CEXHeader)) {
        struct CEXHeader h;
        memcpy(&h, f, sizeof(h));
        uint32_t crc = cex_checksum(f, len);
        out_printf("%s: CEX (cOSmOS Executable) version %u, %u bytes\n", path, h.version, len);
        out_printf("  name %.16s, entry 0x%08x, image 0x%08x-0x%08x, %u sections\n", h.name, h.entry, h.image_base,
                   h.image_end, h.section_count);
        out_printf("  checksum 0x%08x (%s)\n", h.checksum, crc == h.checksum ? "ok" : "MISMATCH");
        out_printf("  SECTION  TYPE     VADDR      MEMSZ    FILEOFF  FILESZ   ALIGN  PERM\n");
        for (uint32_t i = 0; i < h.section_count && i < CEX_MAX_SECTIONS; i++) {
            struct CEXSection s;
            if (h.section_table_offset + (i + 1) * sizeof(s) > len)
                break;
            memcpy(&s, f + h.section_table_offset + i * sizeof(s), sizeof(s));
            out_printf("  %-8.8s %-8s 0x%08x %8u %8u %8u %6u  %c%c%c\n", s.name, cex_section_type_name(s.type),
                       s.vaddr, s.mem_size, s.file_offset, s.file_size, s.alignment,
                       (s.flags & CEX_FLAG_READ) ? 'r' : '-', (s.flags & CEX_FLAG_WRITE) ? 'w' : '-',
                       (s.flags & CEX_FLAG_EXEC) ? 'x' : '-');
        }
    } else if (elf_has_magic(f, len) && len >= sizeof(struct ELF32Header)) {
        struct ELF32Header h;
        memcpy(&h, f, sizeof(h));
        out_printf("%s: ELF32 %s executable, i386, %u bytes, entry 0x%08x\n", path, h.type == ELF_TYPE_EXEC ? "static" : "non-exec",
                   len, h.entry);
        out_printf("  SEGMENT  OFFSET   VADDR      FILESZ   MEMSZ    ALIGN  PERM\n");
        for (uint32_t i = 0; i < h.phnum && h.phoff + (i + 1) * sizeof(struct ELF32ProgramHeader) <= len; i++) {
            struct ELF32ProgramHeader ph;
            memcpy(&ph, f + h.phoff + i * sizeof(ph), sizeof(ph));
            if (ph.type != ELF_PT_LOAD)
                continue;
            out_printf("  LOAD     %8u 0x%08x %8u %8u %6u  %c%c%c\n", ph.offset, ph.vaddr, ph.filesz, ph.memsz,
                       ph.align, (ph.flags & ELF_PF_R) ? 'r' : '-', (ph.flags & ELF_PF_W) ? 'w' : '-',
                       (ph.flags & ELF_PF_X) ? 'x' : '-');
        }
    } else {
        out_printf("%s: no CEX / ELF header: loaded as a flat binary at 0x0 (%u bytes)\n", path, len);
    }
    free(data);
    return 0;
}
