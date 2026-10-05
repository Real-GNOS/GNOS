#ifndef DRIVERS_RTC_H
#define DRIVERS_RTC_H
#include <stdint.h>

int rtc_read_time(uint8_t *hour, uint8_t *min, uint8_t *sec);
int rtc_read_date(uint8_t *year, uint8_t *month, uint8_t *day);
uint64_t rtc_unix_time(void);

#endif
