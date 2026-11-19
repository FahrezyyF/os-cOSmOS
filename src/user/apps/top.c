#include "user/ulib.h"

/**
 * top - live process monitor: CPU usage per process over the last second (from the
 * scheduler's per-process CPU time), states, memory. Q or Ctrl+C quits.
 */

#define MAX 32

struct Sample {
    uint32_t pid;
    uint32_t cpu_ms;
};

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    struct SyscallProcessInfo *list = calloc(MAX, sizeof(*list));
    struct Sample prev[MAX];
    uint32_t prev_n = 0, prev_idle = 0, prev_busy = 0, prev_switch = 0;
    if (list == NULL)
        return 1;
    struct SyscallScreenInfo screen;
    console_screen_info(&screen);
    for (;;) {
        struct SyscallSchedInfo s;
        sched_info(&s);
        uint32_t n = process_list(list, MAX);
        if (n > MAX)
            n = MAX;
        uint32_t d_idle = s.idle_ms - prev_idle, d_busy = s.busy_ms - prev_busy, d_total = d_idle + d_busy;
        console_clear_screen();
        printf_color(C_BLACK | C_LIGHT_CYAN << 4, " top - up %u s, %u processes, %u context switches/s, CPU %u%% busy  (q quits) ",
                     s.uptime_ms / 1000, s.process_count, s.context_switches - prev_switch,
                     d_total ? d_busy * 100 / d_total : 0);
        printf("\n%s, slices %u/%u/%u ms\n\n", s.algorithm, s.slice_ms[0], s.slice_ms[1], s.slice_ms[2]);
        printf_color(C_WHITE, "  PID  PPID STATE    PRIO    CPU%%   CPU(ms)  RES  SWAP  HEAP(K) FMT  COMMAND\n");
        for (uint32_t i = 0; i < n; i++) {
            struct SyscallProcessInfo *p = &list[i];
            uint32_t before = p->cpu_ms;
            for (uint32_t k = 0; k < prev_n; k++)
                if (prev[k].pid == p->pid)
                    before = prev[k].cpu_ms;
            uint32_t pct = d_total ? (p->cpu_ms - before) * 100 / d_total : 0;
            uint8_t color = pct >= 50 ? C_LIGHT_RED : (pct > 0 ? C_YELLOW : (p->foreground ? C_LIGHT_GREEN : C_LIGHT_GRAY));
            printf_color(color, "%5u %5u %-8s %-6s %4u%% %9u %4u %5u %8u %-4s %s\n", p->pid, p->ppid, p->state,
                         p->priority == 0 ? "low" : (p->priority == 2 ? "high" : "normal"), pct, p->cpu_ms,
                         p->pages_resident, p->pages_swapped, p->heap_bytes / 1024, p->format, p->cmdline);
        }
        printf_color(C_DARK_GRAY, "\n  idle %u%%   (`nice <pid> high` gives a process longer time slices)\n",
                     d_total ? d_idle * 100 / d_total : 0);
        prev_n = n;
        for (uint32_t i = 0; i < n; i++) {
            prev[i].pid    = list[i].pid;
            prev[i].cpu_ms = list[i].cpu_ms;
        }
        prev_idle   = s.idle_ms;
        prev_busy   = s.busy_ms;
        prev_switch = s.context_switches;
        // ~1 s refresh, but react to q quickly
        for (int t = 0; t < 10; t++) {
            char c = getchar_nowait();
            if (c == 'q' || c == 'Q') {
                free(list);
                return 0;
            }
            sleep_ms(100);
        }
    }
}
