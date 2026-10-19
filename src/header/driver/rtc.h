#ifndef _RTC_H
#define _RTC_H

#include <stdint.h>
#include <stdbool.h>

#define CMOS_ADDRESS_PORT 0x70
#define CMOS_DATA_PORT    0x71

/**
 * RTCTime, wall clock read from the CMOS real-time clock (UTC in QEMU by default)
 */
struct RTCTime {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
};

/**
 * Read the CMOS RTC. Waits for "update in progress" to clear and reads twice
 * until two consecutive reads agree; handles BCD and 12-hour formats.
 */
void rtc_read(struct RTCTime *out);

// Seconds since 1970-01-01 00:00:00 (used for file timestamps)
uint32_t rtc_unix_time(void);

// Convert a Unix timestamp back into calendar fields
void rtc_from_unix(uint32_t timestamp, struct RTCTime *out);

#endif
