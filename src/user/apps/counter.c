#include "user/ulib.h"

/**
 * counter - multitasking demo: prints a counter at a fixed interval.
 *   counter [label] [interval_ms] [count]
 * Run several in the background to watch the scheduler interleave them:
 *   counter A 300 10 & counter B 500 6 &
 * `counter spin` burns CPU instead of sleeping (preemption demo, see `ps -l` / `top`).
 */
int main(int argc, char *argv[]) {
    const char *label = argc > 1 ? argv[1] : "count";
    uint32_t interval = 500, count = 10;
    if (argc > 2)
        parse_uint(argv[2], &interval);
    if (argc > 3)
        parse_uint(argv[3], &count);
    static const uint8_t colors[] = {C_LIGHT_CYAN, C_LIGHT_GREEN, C_LIGHT_MAGENTA, C_YELLOW, C_LIGHT_RED, C_LIGHT_BLUE};
    uint8_t color = colors[getpid() % sizeof(colors)];
    if (strcmp(label, "spin") == 0) {
        // CPU-bound: never blocks, the timer interrupt preempts it every time slice
        uint32_t start = uptime_ms(), loops = 0;
        while (count == 0 || uptime_ms() - start < count * 1000) {
            for (volatile uint32_t i = 0; i < 100000; i++) {
            }
            loops++;
        }
        printf_color(color, "[spin pid %u] %u loops in %u s while others ran\n", getpid(), loops, count);
        return 0;
    }
    for (uint32_t i = 1; i <= count; i++) {
        printf_color(color, "[%s pid %u] %u/%u at %u ms\n", label, getpid(), i, count, uptime_ms());
        sleep_ms(interval);
    }
    return 0;
}
