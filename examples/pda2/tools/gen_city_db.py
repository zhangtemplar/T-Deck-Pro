#!/usr/bin/env python3
"""
gen_city_db.py — build the shared city database (city_db.c) for T-Deck-Pro.

One table serves the world clock (name + timezone), the weather app (name +
coordinates) and GPS (nearest-city lookup), so a city is described in exactly
one place.

The C table is generated, never hand-edited: the source of truth is a readable
pipe-delimited file, and timezone strings are normalised into a separate zone
table so that hundreds of cities sharing "CET-1CEST,M3.5.0,M10.5.0/3" cost one
copy of it rather than one each.

Input
  tools/cities.txt          curated list:  city|country|lat|lon|posix_tz
  --geonames FILE           optional GeoNames extract (cities15000.txt et al),
                            tab-separated, for precise coordinates and scale
  --min-population N        with --geonames, keep cities at or above this

GeoNames columns used: 1 name, 4 latitude, 5 longitude, 8 country code,
14 population, 17 timezone (IANA).  IANA zones are mapped to POSIX strings via
the table below; a city whose zone is unknown is skipped and reported, so the
output never contains a city whose clock would be silently wrong.

Usage
  python3 tools/gen_city_db.py
  python3 tools/gen_city_db.py --geonames cities15000.txt --min-population 500000
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "cities.txt")
OUT_C = os.path.join(HERE, "..", "city_db.c")

# IANA -> POSIX TZ. Only needed for --geonames; the curated file carries POSIX
# strings directly. Rules current as of the 2024/2025 tzdata.
IANA_TO_POSIX = {
    "Pacific/Pago_Pago": "SST11",
    "Pacific/Honolulu": "HST10",
    "America/Anchorage": "AKST9AKDT,M3.2.0,M11.1.0",
    "America/Los_Angeles": "PST8PDT,M3.2.0,M11.1.0",
    "America/Vancouver": "PST8PDT,M3.2.0,M11.1.0",
    "America/Tijuana": "PST8PDT,M3.2.0,M11.1.0",
    "America/Denver": "MST7MDT,M3.2.0,M11.1.0",
    "America/Edmonton": "MST7MDT,M3.2.0,M11.1.0",
    "America/Phoenix": "MST7",
    "America/Chicago": "CST6CDT,M3.2.0,M11.1.0",
    "America/Winnipeg": "CST6CDT,M3.2.0,M11.1.0",
    "America/Mexico_City": "CST6",
    "America/Guatemala": "CST6",
    "America/Costa_Rica": "CST6",
    "America/New_York": "EST5EDT,M3.2.0,M11.1.0",
    "America/Toronto": "EST5EDT,M3.2.0,M11.1.0",
    "America/Panama": "EST5",
    "America/Bogota": "<-05>5",
    "America/Lima": "<-05>5",
    "America/Guayaquil": "<-05>5",
    "America/Havana": "CST5CDT,M3.2.0/0,M11.1.0/1",
    "America/Caracas": "<-04>4",
    "America/La_Paz": "<-04>4",
    "America/Santo_Domingo": "AST4",
    "America/Halifax": "AST4ADT,M3.2.0,M11.1.0",
    "America/Santiago": "<-04>4<-03>,M9.1.6/24,M4.1.6/24",
    "America/Sao_Paulo": "<-03>3",
    "America/Argentina/Buenos_Aires": "<-03>3",
    "America/Montevideo": "<-03>3",
    "Atlantic/Cape_Verde": "<-01>1",
    "Atlantic/Reykjavik": "GMT0",
    "Europe/London": "GMT0BST,M3.5.0/1,M10.5.0",
    "Europe/Dublin": "GMT0IST,M3.5.0/1,M10.5.0",   # conventional form; see cities.txt
    "Europe/Lisbon": "WET0WEST,M3.5.0/1,M10.5.0",
    "Africa/Casablanca": "<+01>-1",
    "Africa/Accra": "GMT0",
    "Africa/Abidjan": "GMT0",
    "Africa/Dakar": "GMT0",
    "Europe/Paris": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Berlin": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Madrid": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Rome": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Amsterdam": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Brussels": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Zurich": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Vienna": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Prague": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Warsaw": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Stockholm": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Oslo": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Copenhagen": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Budapest": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Europe/Belgrade": "CET-1CEST,M3.5.0,M10.5.0/3",
    "Africa/Lagos": "WAT-1",
    "Africa/Algiers": "CET-1",
    "Africa/Tunis": "CET-1",
    "Europe/Athens": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "Europe/Helsinki": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "Europe/Kiev": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "Europe/Kyiv": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "Europe/Bucharest": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "Europe/Sofia": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "Europe/Riga": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "Europe/Vilnius": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "Europe/Tallinn": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "Africa/Cairo": "EET-2EEST,M4.5.5/0,M10.5.4/24",
    "Africa/Johannesburg": "SAST-2",
    "Africa/Harare": "CAT-2",
    "Asia/Jerusalem": "IST-2IDT,M3.4.4/26,M10.5.0",
    "Europe/Moscow": "MSK-3",
    "Europe/Istanbul": "<+03>-3",
    "Asia/Riyadh": "<+03>-3",
    "Asia/Baghdad": "<+03>-3",
    "Asia/Qatar": "<+03>-3",
    "Asia/Kuwait": "<+03>-3",
    "Africa/Nairobi": "EAT-3",
    "Africa/Addis_Ababa": "EAT-3",
    "Africa/Dar_es_Salaam": "EAT-3",
    "Asia/Dubai": "<+04>-4",
    "Asia/Muscat": "<+04>-4",
    "Asia/Baku": "<+04>-4",
    "Asia/Tbilisi": "<+04>-4",
    "Asia/Yerevan": "<+04>-4",
    "Asia/Tehran": "<+0330>-3:30",
    "Asia/Kabul": "<+0430>-4:30",
    "Asia/Karachi": "PKT-5",
    "Asia/Tashkent": "<+05>-5",
    "Asia/Almaty": "<+05>-5",
    "Asia/Yekaterinburg": "<+05>-5",
    "Asia/Kolkata": "IST-5:30",
    "Asia/Calcutta": "IST-5:30",
    "Asia/Colombo": "<+0530>-5:30",
    "Asia/Kathmandu": "<+0545>-5:45",
    "Asia/Dhaka": "<+06>-6",
    "Asia/Yangon": "<+0630>-6:30",
    "Asia/Bangkok": "<+07>-7",
    "Asia/Jakarta": "WIB-7",
    "Asia/Ho_Chi_Minh": "<+07>-7",
    "Asia/Saigon": "<+07>-7",
    "Asia/Novosibirsk": "<+07>-7",
    "Asia/Shanghai": "CST-8",
    "Asia/Chongqing": "CST-8",
    "Asia/Taipei": "CST-8",
    "Asia/Hong_Kong": "HKT-8",
    "Asia/Macau": "CST-8",
    "Asia/Singapore": "<+08>-8",
    "Asia/Kuala_Lumpur": "<+08>-8",
    "Asia/Manila": "PST-8",
    "Australia/Perth": "AWST-8",
    "Asia/Makassar": "WITA-8",
    "Asia/Tokyo": "JST-9",
    "Asia/Seoul": "KST-9",
    "Asia/Pyongyang": "KST-9",
    "Asia/Jayapura": "WIT-9",
    "Australia/Adelaide": "ACST-9:30ACDT,M10.1.0,M4.1.0/3",
    "Australia/Darwin": "ACST-9:30",
    "Australia/Brisbane": "AEST-10",
    "Australia/Sydney": "AEST-10AEDT,M10.1.0,M4.1.0/3",
    "Australia/Melbourne": "AEST-10AEDT,M10.1.0,M4.1.0/3",
    "Australia/Hobart": "AEST-10AEDT,M10.1.0,M4.1.0/3",
    "Pacific/Port_Moresby": "<+10>-10",
    "Asia/Vladivostok": "<+10>-10",
    "Pacific/Noumea": "<+11>-11",
    "Pacific/Auckland": "NZST-12NZDT,M9.5.0,M4.1.0/3",
    "Pacific/Fiji": "<+12>-12",
    "Pacific/Apia": "<+13>-13",
    "Pacific/Kiritimati": "<+14>-14",
}


def read_curated(path):
    out = []
    with open(path, encoding="utf-8") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("|")
            if len(parts) != 5:
                sys.exit(f"{path}:{lineno}: expected 5 pipe-separated fields, got {len(parts)}")
            city, country, lat, lon, tz = (p.strip() for p in parts)
            out.append((city, country, float(lat), float(lon), tz))
    return out


def read_geonames(path, min_pop):
    out, skipped = [], {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            f = line.rstrip("\n").split("\t")
            if len(f) < 18:
                continue
            try:
                pop = int(f[14] or 0)
            except ValueError:
                continue
            if pop < min_pop:
                continue
            iana = f[17]
            posix = IANA_TO_POSIX.get(iana)
            if not posix:
                skipped[iana] = skipped.get(iana, 0) + 1
                continue
            out.append((f[1], f[8], float(f[4]), float(f[5]), posix))
    if skipped:
        print("  unmapped IANA zones (cities skipped):", file=sys.stderr)
        for z, n in sorted(skipped.items(), key=lambda kv: -kv[1])[:15]:
            print(f"    {z}: {n}", file=sys.stderr)
    return out


def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--geonames", help="GeoNames cities file (tab separated)")
    ap.add_argument("--min-population", type=int, default=500000)
    ap.add_argument("--out", default=OUT_C)
    args = ap.parse_args()

    cities = read_curated(SRC)
    print(f"curated: {len(cities)} cities")

    if args.geonames:
        extra = read_geonames(args.geonames, args.min_population)
        have = {c[0].lower() for c in cities}
        added = [e for e in extra if e[0].lower() not in have]
        cities += added
        print(f"geonames: +{len(added)} cities (pop >= {args.min_population})")

    # Normalise the timezone strings into their own table.
    zones = []
    zone_of = {}
    for _, _, _, _, tz in cities:
        if tz not in zone_of:
            zone_of[tz] = len(zones)
            zones.append(tz)
    if len(zones) > 255:
        sys.exit(f"{len(zones)} zones exceeds the uint8 index in city_db.h")

    cities.sort(key=lambda c: c[0].lower())          # binary-searchable by name

    for c in cities:
        if not (-90 <= c[2] <= 90) or not (-180 <= c[3] <= 180):
            sys.exit(f"{c[0]}: coordinates out of range ({c[2]}, {c[3]})")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("/**\n * @file      city_db.c\n"
                " * @brief     Shared city database. GENERATED - do not edit.\n *\n"
                " * Regenerate with tools/gen_city_db.py from tools/cities.txt.\n"
                " * Timezone strings are pooled: cities index into city_zones[].\n */\n")
        f.write('#include "city_db.h"\n\n')

        f.write("const city_zone_t city_zones[] = {\n")
        for tz in zones:
            f.write(f'    {{ "{c_escape(tz)}" }},\n')
        f.write("};\nconst int city_zone_count = (int)(sizeof(city_zones) / sizeof(city_zones[0]));\n\n")

        f.write("/* Sorted by name (case-insensitive) so lookups can binary search. */\n")
        f.write("const city_t city_table[] = {\n")
        for city, country, lat, lon, tz in cities:
            f.write(f'    {{ "{c_escape(city)}", "{c_escape(country)}", '
                    f"{round(lat * 100)}, {round(lon * 100)}, {zone_of[tz]} }},\n")
        f.write("};\nconst int city_count = (int)(sizeof(city_table) / sizeof(city_table[0]));\n")

    print(f"wrote {args.out}: {len(cities)} cities, {len(zones)} zones")


if __name__ == "__main__":
    main()
