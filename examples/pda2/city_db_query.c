/**
 * @file      city_db_query.c
 * @brief     Lookups over the generated city table. See city_db.h.
 *
 * Kept separate from city_db.c because that file is generated and gets
 * overwritten by tools/gen_city_db.py.
 */
#include "city_db.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

/* Parse the standard offset out of a POSIX TZ string.
 *
 * The form is  std offset [dst [offset] [,rule]]  where the offset is the time
 * to ADD to local to get UTC — i.e. the sign is inverted relative to how the
 * zone is normally written. "CET-1CEST,..." is UTC+1, "EST5EDT,..." is UTC-5.
 * The name may be an abbreviation or a bracketed form like "<+0530>".
 */
int city_std_offset_min(const city_t *c)
{
    const char *p = city_tz(c);
    if (!p) return 0;

    if (*p == '<') {                      /* skip a bracketed zone name */
        while (*p && *p != '>') p++;
        if (*p == '>') p++;
    } else {
        while (*p && (isalpha((unsigned char)*p))) p++;
    }

    int sign = 1;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }

    if (!isdigit((unsigned char)*p)) return 0;

    int hours = 0, mins = 0;
    while (isdigit((unsigned char)*p)) hours = hours * 10 + (*p++ - '0');
    if (*p == ':') {
        p++;
        while (isdigit((unsigned char)*p)) mins = mins * 10 + (*p++ - '0');
    }

    /* POSIX sign is inverted: a positive number means west of Greenwich. */
    return -sign * (hours * 60 + mins);
}

static int ci_starts_with(const char *hay, const char *needle)
{
    while (*needle) {
        if (!*hay) return 0;
        if (tolower((unsigned char)*hay) != tolower((unsigned char)*needle)) return 0;
        hay++;
        needle++;
    }
    return 1;
}

int city_search_prefix(const char *prefix, const city_t **out, int max)
{
    if (!prefix || !*prefix || max <= 0) return 0;
    int n = 0;

    /* City names first — that is what people type — then country, skipping
     * anything already listed. */
    for (int i = 0; i < city_count && n < max; i++) {
        if (ci_starts_with(city_table[i].city, prefix)) out[n++] = &city_table[i];
    }
    for (int i = 0; i < city_count && n < max; i++) {
        if (ci_starts_with(city_table[i].city, prefix)) continue;
        if (ci_starts_with(city_table[i].country, prefix)) out[n++] = &city_table[i];
    }
    return n;
}

int city_in_zone(int offset_min, const city_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < city_count && n < max; i++) {
        if (city_std_offset_min(&city_table[i]) == offset_min) out[n++] = &city_table[i];
    }
    return n;
}

int city_index_of(const char *name)
{
    if (!name) return -1;
    for (int i = 0; i < city_count; i++) {
        if (strcmp(city_table[i].city, name) == 0) return i;
    }
    return -1;
}

const city_t *city_nearest(float lat, float lon)
{
    const city_t *best = NULL;
    float best_d = 0;

    for (int i = 0; i < city_count; i++) {
        float dlat = city_lat(&city_table[i]) - lat;
        float dlon = city_lon(&city_table[i]) - lon;
        /* Wrap the longitude difference so a point near the date line is not
         * considered half a world away from a city just across it. */
        if (dlon > 180.0f)  dlon -= 360.0f;
        if (dlon < -180.0f) dlon += 360.0f;
        /* Scale longitude by cos(lat) so degrees are comparable distances.
         * A coarse cosine is fine — we only need the ordering. */
        float latr = lat * 0.01745329f;
        float cosl = 1.0f - (latr * latr) / 2.0f;      /* small-angle cosine */
        if (cosl < 0.1f) cosl = 0.1f;
        dlon *= cosl;

        float d = dlat * dlat + dlon * dlon;
        if (!best || d < best_d) { best = &city_table[i]; best_d = d; }
    }
    return best;
}

int city_zone_offsets(int16_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < city_count; i++) {
        int16_t o = (int16_t)city_std_offset_min(&city_table[i]);
        int seen = 0;
        for (int j = 0; j < n; j++) if (out[j] == o) { seen = 1; break; }
        if (!seen && n < max) out[n++] = o;
    }
    for (int i = 1; i < n; i++) {              /* short list: insertion sort */
        int16_t v = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}

void city_format_offset(int offset_min, char *out, int cap)
{
    char sign = offset_min < 0 ? '-' : '+';
    int a = offset_min < 0 ? -offset_min : offset_min;
    snprintf(out, cap, "UTC%c%02d:%02d", sign, a / 60, a % 60);
}
