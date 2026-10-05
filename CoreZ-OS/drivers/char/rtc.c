#include "drivers/char/rtc.h"

#include "arch/interrupt/interrupt.h"
#include "kernel/asm_func.h"
#include "kernel/init/pit/pit.h"

#define RTC_REG_INDEX 0x70
#define RTC_REG_DATA 0x71
#define RTC_REG_SEC 0x00
#define RTC_REG_MIN 0x02
#define RTC_REG_HOUR 0x04
#define RTC_REG_DAY 0x07
#define RTC_REG_MONTH 0x08
#define RTC_REG_YEAR 0x09
#define RTC_REG_STATUS_A 0x0A
#define RTC_REG_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
    outb(RTC_REG_INDEX, reg);
    return inb(RTC_REG_DATA);
}

static uint8_t from_bcd(uint8_t v) {
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

int rtc_read_time(uint8_t *hour, uint8_t *min, uint8_t *sec) {
    uint8_t status_b = cmos_read(RTC_REG_STATUS_B);
    uint8_t h;
    uint8_t m;
    uint8_t s;
    for (int i = 0; i < 64; i++) {
        if (!(cmos_read(RTC_REG_STATUS_A) & 0x80))
            break;
    }
    s = cmos_read(RTC_REG_SEC);
    m = cmos_read(RTC_REG_MIN);
    h = cmos_read(RTC_REG_HOUR);
    if (!(status_b & 0x04)) {
        s = from_bcd(s);
        m = from_bcd(m);
        h = from_bcd(h);
    }
    if (!(status_b & 0x02)) {
        int pm = (h & 0x80) != 0;
        h = (uint8_t)(h & 0x7F);
        if (pm && h != 12)
            h = (uint8_t)(h + 12);
        if (!pm && h == 12)
            h = 0;
    }
    if (h >= 24)
        h = 0;
    if (m >= 60)
        m = 0;
    if (s >= 60)
        s = 0;
    *hour = h;
    *min = m;
    *sec = s;
    return 0;
}

int rtc_read_date(uint8_t *year, uint8_t *month, uint8_t *day) {
    uint8_t status_b = cmos_read(RTC_REG_STATUS_B);
    uint8_t y;
    uint8_t mo;
    uint8_t d;
    for (int i = 0; i < 64; i++) {
        if (!(cmos_read(RTC_REG_STATUS_A) & 0x80))
            break;
    }
    d = cmos_read(RTC_REG_DAY);
    mo = cmos_read(RTC_REG_MONTH);
    y = cmos_read(RTC_REG_YEAR);
    if (!(status_b & 0x04)) {
        d = from_bcd(d);
        mo = from_bcd(mo);
        y = from_bcd(y);
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31) {
        return -1;
    }
    *year = y;
    *month = mo;
    *day = d;
    return 0;
}

static uint64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (uint64_t)(era * 146097 + (int)doe - 719468);
}

uint64_t rtc_unix_time(void) {
    static uint64_t base;
    static uint32_t base_tick;
    static int have;
    if (!have) {
        uint8_t y;
        uint8_t mo;
        uint8_t d;
        uint8_t h;
        uint8_t mi;
        uint8_t s;
        if (rtc_read_date(&y, &mo, &d) != 0 ||
            rtc_read_time(&h, &mi, &s) != 0) {
            return 0;
        }
        base = days_from_civil(2000 + (int)y, mo, d) * 86400ull +
               (uint64_t)h * 3600ull + (uint64_t)mi * 60ull + (uint64_t)s;
        base_tick = tick;
        have = 1;
    }
    return base + (uint64_t)(tick - base_tick) / (uint64_t)PIT_HZ;
}
