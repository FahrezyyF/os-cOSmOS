#include "header/driver/rtc.h"
#include "header/cpu/portio.h"

static uint8_t cmos_read(uint8_t reg) {
    out(CMOS_ADDRESS_PORT, reg | 0x80); // bit 7 keeps NMI disabled while selecting
    return in(CMOS_DATA_PORT);
}

static bool update_in_progress(void) {
    return cmos_read(0x0A) & 0x80;
}

static void read_raw(struct RTCTime *t, uint8_t *century) {
    for (uint32_t spin = 0; spin < 1000000 && update_in_progress(); spin++)
        ;
    t->second = cmos_read(0x00);
    t->minute = cmos_read(0x02);
    t->hour   = cmos_read(0x04);
    t->day    = cmos_read(0x07);
    t->month  = cmos_read(0x08);
    t->year   = cmos_read(0x09);
    *century  = cmos_read(0x32);
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t) ((v & 0x0F) + (v >> 4) * 10);
}

void rtc_read(struct RTCTime *out) {
    struct RTCTime a, b;
    uint8_t ca, cb;
    read_raw(&a, &ca);
    for (int attempt = 0; attempt < 8; attempt++) {
        read_raw(&b, &cb);
        if (a.second == b.second && a.minute == b.minute && a.hour == b.hour &&
            a.day == b.day && a.month == b.month && a.year == b.year && ca == cb)
            break;
        a  = b;
        ca = cb;
    }

    uint8_t status_b = cmos_read(0x0B);
    bool    pm       = (a.hour & 0x80) != 0;
    a.hour &= 0x7F;
    if (!(status_b & 0x04)) { // BCD mode
        a.second = bcd_to_bin(a.second);
        a.minute = bcd_to_bin(a.minute);
        a.hour   = bcd_to_bin(a.hour);
        a.day    = bcd_to_bin(a.day);
        a.month  = bcd_to_bin(a.month);
        a.year   = bcd_to_bin((uint8_t) a.year);
        ca       = bcd_to_bin(ca);
    }
    if (!(status_b & 0x02)) { // 12-hour mode
        if (a.hour == 12)
            a.hour = 0;
        if (pm)
            a.hour += 12;
    }
    a.year += (ca >= 19 && ca <= 30) ? ca * 100 : 2000;
    *out = a;
}

static bool is_leap(uint32_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static const uint8_t days_in_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

uint32_t rtc_unix_time(void) {
    struct RTCTime t;
    rtc_read(&t);
    uint32_t days = 0;
    for (uint32_t y = 1970; y < t.year; y++)
        days += is_leap(y) ? 366 : 365;
    for (uint32_t m = 1; m < t.month && m <= 12; m++)
        days += days_in_month[m - 1] + ((m == 2 && is_leap(t.year)) ? 1 : 0);
    days += (uint32_t) (t.day ? t.day - 1 : 0);
    return days * 86400u + t.hour * 3600u + t.minute * 60u + t.second;
}

void rtc_from_unix(uint32_t timestamp, struct RTCTime *out) {
    uint32_t days = timestamp / 86400u;
    uint32_t rem  = timestamp % 86400u;
    out->hour   = (uint8_t) (rem / 3600u);
    out->minute = (uint8_t) ((rem % 3600u) / 60u);
    out->second = (uint8_t) (rem % 60u);
    uint32_t year = 1970;
    for (;;) {
        uint32_t year_days = is_leap(year) ? 366 : 365;
        if (days < year_days)
            break;
        days -= year_days;
        year++;
    }
    uint8_t month = 0;
    for (; month < 12; month++) {
        uint32_t mdays = days_in_month[month] + ((month == 1 && is_leap(year)) ? 1 : 0);
        if (days < mdays)
            break;
        days -= mdays;
    }
    out->year  = (uint16_t) year;
    out->month = (uint8_t) (month + 1);
    out->day   = (uint8_t) (days + 1);
}
