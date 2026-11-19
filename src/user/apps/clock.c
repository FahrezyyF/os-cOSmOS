#include "user/ulib.h"

/**
 * clock - background clock (book 4.3.3). Reads the CMOS real-time clock through
 * SYS_TIME every 250 ms and shows HH:MM:SS in the bottom-right corner of the screen
 * (a taskbar widget, SYS_WIDGET_SET). It detaches from the shell, so the prompt returns at
 * once while the clock keeps running as its own process; `kill <pid>` stops it and the
 * kernel removes the widget, `clock` starts it again.
 *
 *   clock          start (UTC + the offset from CLOCK_TZ, e.g. export CLOCK_TZ=7 for WIB)
 *   clock -d       with the date
 */

static bool already_running(void) {
    struct SyscallProcessInfo list[16];
    uint32_t n = process_list(list, 16), me = getpid();
    for (uint32_t i = 0; i < n && i < 16; i++) {
        if (list[i].pid != me && strcmp(list[i].name, "clock") == 0) {
            printf("clock: already running as pid %u (`kill %u` stops it)\n", list[i].pid, list[i].pid);
            return true;
        }
    }
    return false;
}

static const uint8_t days_in_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

// Apply a timezone offset in hours to an RTC time (UTC)
static void shift_hours(struct SyscallTime *t, int offset) {
    int hour = (int) t->hour + offset;
    int day = t->day, month = t->month, year = t->year;
    while (hour < 0) {
        hour += 24;
        if (--day < 1) {
            if (--month < 1) {
                month = 12;
                year--;
            }
            day = days_in_month[month - 1] + (month == 2 && year % 4 == 0);
        }
    }
    while (hour >= 24) {
        hour -= 24;
        int dim = days_in_month[month - 1] + (month == 2 && year % 4 == 0);
        if (++day > dim) {
            day = 1;
            if (++month > 12) {
                month = 1;
                year++;
            }
        }
    }
    t->hour = (uint8_t) hour;
    t->day = (uint8_t) day;
    t->month = (uint8_t) month;
    t->year = (uint16_t) year;
}

int main(int argc, char *argv[]) {
    bool with_date = argc > 1 && strcmp(argv[1], "-d") == 0;
    if (already_running())
        return 1;
    const char *tz = getenv("CLOCK_TZ");
    int offset = tz != NULL ? atoi(tz) : 0;
    printf("clock: running as pid %u in the background (bottom-right corner); `kill %u` stops it\n",
           getpid(), getpid());
    detach(); // the shell stops waiting for us: we are a background program now

    char text[40], last[40] = "";
    for (;;) {
        struct SyscallTime t;
        get_time(&t);
        shift_hours(&t, offset);
        if (with_date)
            snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day, t.hour,
                     t.minute, t.second);
        else
            snprintf(text, sizeof(text), "%02u:%02u:%02u", t.hour, t.minute, t.second);
        if (strcmp(text, last) != 0) {
            widget_set(text, C_BLACK | C_YELLOW << 4);
            strncpy(last, text, sizeof(last) - 1);
        }
        sleep_ms(250); // blocks: other processes run meanwhile
    }
}
