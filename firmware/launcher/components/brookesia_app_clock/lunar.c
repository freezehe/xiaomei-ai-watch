/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lunar.h"

#include <stdint.h>
#include <stdio.h>

/*
 * One entry per lunar year 2000..2099 (same encoding as the widely used 1900-2100 table,
 * taken from the Python `lunardate` package and checked day by day against both `lunardate`
 * and `zhdate` for 2000-02-05..2099-12-31, 36490 days, no mismatch):
 *   bits 15..4 : month 1..12 is long (30 days) when set, short (29 days) otherwise
 *   bits  3..0 : leap month number, 0 = no leap month
 *   bit     16 : the leap month is long (30 days)
 */
static const uint32_t LUNAR_INFO[] = {
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552,   // 2000-2004
    0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,   // 2005-2009
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9,   // 2010-2014
    0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,   // 2015-2019
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60,   // 2020-2024
    0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,   // 2025-2029
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0,   // 2030-2034
    0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,   // 2035-2039
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577,   // 2040-2044
    0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,   // 2045-2049
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0,   // 2050-2054
    0x168a6, 0x0ea50, 0x06aa0, 0x1a6c4, 0x0aae0,   // 2055-2059
    0x092e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0,   // 2060-2064
    0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4,   // 2065-2069
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6,   // 2070-2074
    0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0,   // 2075-2079
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50,   // 2080-2084
    0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160,   // 2085-2089
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0,   // 2090-2094
    0x04ae0, 0x0a9d4, 0x0a2d0, 0x0d150, 0x0f252,   // 2095-2099
};
#define LUNAR_FIRST_YEAR 2000
#define LUNAR_YEARS      ((int)(sizeof(LUNAR_INFO) / sizeof(LUNAR_INFO[0])))

// Days since 1970-01-01 (proleptic Gregorian), H. Hinnant's days_from_civil
static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int leap_month_days(uint32_t info)
{
    if ((info & 0xf) == 0) {
        return 0;
    }
    return (info & 0x10000) ? 30 : 29;
}

static int month_days(uint32_t info, int month)
{
    return ((info >> (16 - month)) & 1) ? 30 : 29;
}

static int year_days(uint32_t info)
{
    int n = 0;
    for (int m = 1; m <= 12; m++) {
        n += month_days(info, m);
    }
    return n + leap_month_days(info);
}

bool lunar_from_solar(int year, int month, int day, lunar_date_t *out)
{
    // Lunar 2000-01-01 (Spring Festival) = 2000-02-05
    long offset = days_from_civil(year, month, day) - days_from_civil(2000, 2, 5);
    if (offset < 0) {
        return false;
    }

    int ly = LUNAR_FIRST_YEAR;
    uint32_t info;
    while (true) {
        if (ly - LUNAR_FIRST_YEAR >= LUNAR_YEARS) {
            return false;
        }
        info = LUNAR_INFO[ly - LUNAR_FIRST_YEAR];
        int yd = year_days(info);
        if (offset < yd) {
            break;
        }
        offset -= yd;
        ly++;
    }

    const int leap = info & 0xf;
    for (int m = 1; m <= 12; m++) {
        int md = month_days(info, m);
        if (offset < md) {
            *out = (lunar_date_t) {
                ly, m, (int)offset + 1, false, md
            };
            return true;
        }
        offset -= md;
        if (m == leap) {
            int lmd = leap_month_days(info);
            if (offset < lmd) {
                *out = (lunar_date_t) {
                    ly, m, (int)offset + 1, true, lmd
                };
                return true;
            }
            offset -= lmd;
        }
    }
    return false;
}

static const char *const MONTH_NAMES[] = {
    "正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"
};
static const char *const DIGITS[] = {
    "", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"
};

void lunar_format_date(const lunar_date_t *ld, char *buf, size_t len)
{
    char day[16];
    const int d = ld->day;
    if (d <= 10) {
        snprintf(day, sizeof(day), "初%s", DIGITS[d]);
    } else if (d < 20) {
        snprintf(day, sizeof(day), "十%s", DIGITS[d - 10]);
    } else if (d == 20) {
        snprintf(day, sizeof(day), "二十");
    } else if (d < 30) {
        snprintf(day, sizeof(day), "廿%s", DIGITS[d - 20]);
    } else {
        snprintf(day, sizeof(day), "三十");
    }
    snprintf(buf, len, "%s%s月%s", ld->is_leap ? "闰" : "", MONTH_NAMES[ld->month - 1], day);
}

void lunar_format_year(const lunar_date_t *ld, char *buf, size_t len)
{
    static const char *const GAN[] = { "甲", "乙", "丙", "丁", "戊", "己", "庚", "辛", "壬", "癸" };
    static const char *const ZHI[] = { "子", "丑", "寅", "卯", "辰", "巳", "午", "未", "申", "酉", "戌", "亥" };
    static const char *const ANIMAL[] = { "鼠", "牛", "虎", "兔", "龙", "蛇", "马", "羊", "猴", "鸡", "狗", "猪" };
    const int n = ld->year - 4;   // 4 AD was a 甲子 year
    snprintf(buf, len, "%s%s%s年", GAN[n % 10], ZHI[n % 12], ANIMAL[n % 12]);
}

const char *lunar_festival(int year, int month, int day, const lunar_date_t *ld)
{
    (void)year;
    if (ld != NULL && !ld->is_leap) {
        const int m = ld->month, d = ld->day;
        if (m == 12 && d == ld->month_days) {
            return "除夕";
        }
        if (m == 1 && d == 1) {
            return "春节";
        }
        if (m == 1 && d == 15) {
            return "元宵节";
        }
        if (m == 5 && d == 5) {
            return "端午节";
        }
        if (m == 7 && d == 7) {
            return "七夕";
        }
        if (m == 7 && d == 15) {
            return "中元节";
        }
        if (m == 8 && d == 15) {
            return "中秋节";
        }
        if (m == 9 && d == 9) {
            return "重阳节";
        }
        if (m == 12 && d == 8) {
            return "腊八节";
        }
    }
    if (month == 1 && day == 1) {
        return "元旦";
    }
    if (month == 5 && day == 1) {
        return "劳动节";
    }
    if (month == 6 && day == 1) {
        return "儿童节";
    }
    if (month == 10 && day == 1) {
        return "国庆节";
    }
    if (month == 12 && day == 25) {
        return "圣诞节";
    }
    return NULL;
}
