#pragma once
#include <stdbool.h>
#include <time.h>

typedef enum {
    VG_THEME_DAY, VG_THEME_NIGHT, VG_THEME_AUTO
} VGThemeMode;

typedef struct {
    time_t sunrise;
    time_t sunset;
} VGSolarDay;

static inline bool vg_same_date(time_t first, time_t second) {
    const struct tm *local = localtime(&first);
    if (!local) return false;
    const int year = local->tm_year;
    const int day = local->tm_yday;
    local = localtime(&second);
    return local && local->tm_year == year && local->tm_yday == day;
}

static inline bool vg_solar_day_valid(VGSolarDay solar) {
    return solar.sunrise > 0 && solar.sunset > solar.sunrise &&
           solar.sunset - solar.sunrise < 24 * 60 * 60;
}

static inline bool vg_theme_is_dark(VGThemeMode mode, time_t now,
                                    const VGSolarDay *cached, unsigned count) {
    if (mode != VG_THEME_AUTO) return mode == VG_THEME_NIGHT;
    for (unsigned i = 0; i < count; ++i) {
        if (vg_solar_day_valid(cached[i]) && vg_same_date(now, cached[i].sunrise))
            return now < cached[i].sunrise || now >= cached[i].sunset;
    }
    const struct tm *local = localtime(&now);
    return local && (local->tm_hour < 7 || local->tm_hour >= 19);
}

#define SOLAR_THEME_STORAGE_KEY 436
static VGSolarDay s_solar_days[2];

static void solar_theme_load(void) {
  if (persist_read_data(SOLAR_THEME_STORAGE_KEY, s_solar_days, sizeof(s_solar_days)) != sizeof(s_solar_days))
    memset(s_solar_days, 0, sizeof(s_solar_days));
}

static bool solar_theme_dark(int mode) {
  return vg_theme_is_dark((VGThemeMode)mode, time(NULL), s_solar_days, ARRAY_LENGTH(s_solar_days));
}

static VGSolarDay solar_theme_read(DictionaryIterator *iter, uint32_t rise_key, uint32_t set_key) {
  const Tuple *rise = dict_find(iter, rise_key);
  const Tuple *set = dict_find(iter, set_key);
  if (!rise || !set || rise->length != 4 || set->length != 4 ||
      (rise->type != TUPLE_INT && rise->type != TUPLE_UINT) ||
      (set->type != TUPLE_INT && set->type != TUPLE_UINT))
    return (VGSolarDay){0};
  return (VGSolarDay){rise->value->int32, set->value->int32};
}
static void solar_theme_cache(VGSolarDay today, VGSolarDay tomorrow) {
    time_t now = time(NULL);
    time_t next_date = now + 24 * 60 * 60;
    // Use a noon anchor for the next date so DST transitions near midnight
    // don't choose a date two days out or repeat today's date.
    struct tm *local = localtime(&now);
    if (local) next_date = now + (36 - local->tm_hour) * 60 * 60;
    const VGSolarDay incoming[] = {today, tomorrow};
    const time_t dates[] = {now, next_date};
    VGSolarDay updated[2] = {{0}};
    for (unsigned day = 0; day < ARRAY_LENGTH(dates); ++day) {
        for (unsigned i = 0; i < ARRAY_LENGTH(s_solar_days); ++i) {
            if (vg_solar_day_valid(s_solar_days[i]) && vg_same_date(s_solar_days[i].sunrise, dates[day]))
                updated[day] = s_solar_days[i];
        }
        for (unsigned i = 0; i < ARRAY_LENGTH(incoming); ++i) {
            if (vg_solar_day_valid(incoming[i]) && vg_same_date(incoming[i].sunrise, dates[day]))
                updated[day] = incoming[i];
        }
    }
    if (memcmp(s_solar_days, updated, sizeof(s_solar_days))) {
        memcpy(s_solar_days, updated, sizeof(s_solar_days));
        persist_write_data(SOLAR_THEME_STORAGE_KEY, s_solar_days, sizeof(s_solar_days));
    }
}

static bool solar_theme_receive(DictionaryIterator *iter) {
  VGSolarDay today = solar_theme_read(iter, MESSAGE_KEY_VG_SUNRISE, MESSAGE_KEY_VG_SUNSET);
  VGSolarDay tomorrow = solar_theme_read(iter, MESSAGE_KEY_VG_NEXT_SUNRISE, MESSAGE_KEY_VG_NEXT_SUNSET);
  if (!vg_solar_day_valid(today) && !vg_solar_day_valid(tomorrow)) return false;
  solar_theme_cache(today, tomorrow);
  return true;
}
