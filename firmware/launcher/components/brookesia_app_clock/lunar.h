/*
 * SPDX-FileCopyrightText: 2025
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int year;           // Lunar year (the one that starts at Spring Festival)
    int month;          // 1..12
    int day;            // 1..30
    bool is_leap;       // Leap month (闰月)
    int month_days;     // 29 or 30: length of this lunar month
} lunar_date_t;

/**
 * @brief Gregorian date -> Chinese lunar date. Valid from 2000-02-05 to 2099-12-31.
 * @return false outside that range
 */
bool lunar_from_solar(int year, int month, int day, lunar_date_t *out);

/** @brief "八月十六", "闰六月初一" (UTF-8) */
void lunar_format_date(const lunar_date_t *ld, char *buf, size_t len);

/** @brief Sexagenary year + zodiac: "丙午马年" (UTF-8) */
void lunar_format_year(const lunar_date_t *ld, char *buf, size_t len);

/**
 * @brief Festival name for the day (lunar festivals first, then a few Gregorian ones),
 *        e.g. "中秋节", "除夕", "国庆节". NULL if none.
 */
const char *lunar_festival(int year, int month, int day, const lunar_date_t *ld);

#ifdef __cplusplus
}
#endif
