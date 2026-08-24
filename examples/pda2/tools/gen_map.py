#!/usr/bin/env python3
"""
Build a .tdmap vector map for the T-Deck-Pro from OSM data.

Why a custom format rather than shipping Garmin IMG or Mapsforge to the
device: the device needs to answer exactly one question quickly — "what lines
cross this rectangle?" — from a file it cannot hold in RAM, and draw the
answer on a 240x320 one-bit panel. Everything else those formats carry
(routing graphs, address search, label placement, styling, multiple
character encodings) is weight the firmware would have to skip past on every
frame. This format is a spatial index and a pile of polylines, and the reader
for it is a few hundred lines instead of a few thousand.

Input, in order of preference:
  *.geojson   LineString / MultiLineString / Polygon / MultiPolygon / Point
  *.osm       OpenStreetMap XML (Overpass exports, small extracts)
  *.osm.pbf   only if pyosmium happens to be installed

Getting data with no toolchain at all: draw a box on overpass-turbo.eu, export
as GeoJSON. For contours, gdal_contour writes GeoJSON, or pass SRTM .hgt tiles
directly with --hgt and they are contoured here (needs numpy).

Usage:
  gen_map.py -o fiji.tdmap fiji.osm
  gen_map.py -o alps.tdmap roads.geojson --hgt N46E007.hgt --contour-step 100

FILE FORMAT (all little-endian; coordinates are degrees x 1e7 as int32,
which is the same unit u-blox uses and good to about a centimetre)

  Header, 32 bytes
    0   char[6] "TDMAP\\0"
    6   u8      version = 1
    7   u8      n_levels
    8   i32     min_lat        bounding box of the whole map
    12  i32     min_lon
    16  i32     max_lat
    20  i32     max_lon
    24  u32     level_table_off
    28  u32     0

  Level table, n_levels x 16 bytes, coarsest first
    0   i32     tile_span      degrees x 1e7, square tiles
    4   u16     n_cols
    6   u16     n_rows
    8   u32     index_off      tile index for this level
    12  u32     0

  Tile index, n_cols*n_rows x 8 bytes, row-major from the SW corner
    0   u32     payload offset (0 if the tile is empty)
    4   u32     payload length

  Tile payload: features back to back until the length is used up
    u8      type      (see TYPE_* below)
    u8      flags     bit0 = closed ring
    u16     n_points
    then n_points pairs of zigzag varints (dlat, dlon) in TILE-LOCAL units,
    where one unit is tile_span/65536. The first pair is measured from the
    tile's SW corner, the rest from the previous point.

  Local units rather than absolute 1e-7 degrees because it turns almost every
  delta into a one- or two-byte varint: at the 0.125 degree level a unit is
  about 0.2 m, which is far finer than a 240 px screen can show.
"""

import argparse
import json
import math
import os
import struct
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict

# ---------------------------------------------------------------- feature types
# Kept deliberately short. Every extra type is another branch in the renderer
# and another visually distinct treatment to find on a screen with two colours.
TYPE_COASTLINE     = 0
TYPE_WATER         = 1    # lake/reservoir outline (closed)
TYPE_WATERWAY      = 2    # river, stream
TYPE_ROAD_MAJOR    = 3
TYPE_ROAD_MINOR    = 4
TYPE_PATH          = 5    # footway, track, cycleway
TYPE_RAIL          = 6
TYPE_BUILDING      = 7
TYPE_LANDUSE       = 8
TYPE_CONTOUR       = 9
TYPE_CONTOUR_INDEX = 10   # every fifth contour, drawn heavier
TYPE_PLACE         = 11
TYPE_BOUNDARY      = 12

TYPE_NAMES = {
    v: k[5:].lower() for k, v in list(globals().items()) if k.startswith("TYPE_")
}

# ---------------------------------------------------------------- zoom levels
# tile_span in degrees, and the simplification tolerance to use at that level,
# also in degrees. Tolerance is roughly one screen pixel: a 240 px viewport
# showing `span` degrees resolves span/240, and simplifying below that spends
# bytes on detail the panel cannot draw.
LEVELS = [
    # tile_span, simplify_tol, types to keep
    (8.0,     8.0 / 240,  {TYPE_COASTLINE, TYPE_BOUNDARY, TYPE_WATER, TYPE_PLACE}),
    (1.0,     1.0 / 240,  {TYPE_COASTLINE, TYPE_BOUNDARY, TYPE_WATER, TYPE_WATERWAY,
                           TYPE_ROAD_MAJOR, TYPE_PLACE, TYPE_CONTOUR_INDEX}),
    (0.125,   0.125 / 240, {TYPE_COASTLINE, TYPE_WATER, TYPE_WATERWAY, TYPE_ROAD_MAJOR,
                            TYPE_ROAD_MINOR, TYPE_RAIL, TYPE_PLACE, TYPE_LANDUSE,
                            TYPE_CONTOUR, TYPE_CONTOUR_INDEX}),
    (0.03125, 0.03125 / 240, None),   # None = everything
]

E7 = 10_000_000
MAX_PTS_PER_FEATURE = 250    # longer runs are split; keeps the device buffer small


# ---------------------------------------------------------------- tag mapping
def classify(tags):
    """OSM tags -> feature type, or None to drop. Order matters: first match wins."""
    if tags.get("natural") == "coastline":
        return TYPE_COASTLINE
    if tags.get("natural") in ("water", "bay") or tags.get("landuse") == "reservoir":
        return TYPE_WATER
    if "waterway" in tags:
        return TYPE_WATERWAY if tags["waterway"] in (
            "river", "stream", "canal", "drain", "ditch") else None

    hw = tags.get("highway")
    if hw:
        if hw in ("motorway", "trunk", "primary", "secondary", "tertiary",
                  "motorway_link", "trunk_link", "primary_link", "secondary_link"):
            return TYPE_ROAD_MAJOR
        if hw in ("residential", "unclassified", "service", "living_street", "road"):
            return TYPE_ROAD_MINOR
        if hw in ("path", "footway", "track", "cycleway", "bridleway", "steps"):
            return TYPE_PATH
        return TYPE_ROAD_MINOR

    if tags.get("railway") in ("rail", "light_rail", "subway", "tram", "narrow_gauge"):
        return TYPE_RAIL
    if "building" in tags:
        return TYPE_BUILDING
    if tags.get("landuse") in ("residential", "industrial", "commercial", "retail"):
        return TYPE_LANDUSE
    if tags.get("boundary") == "administrative":
        lvl = tags.get("admin_level", "99")
        return TYPE_BOUNDARY if lvl in ("2", "3", "4") else None
    if tags.get("place") in ("city", "town", "village", "hamlet", "suburb"):
        return TYPE_PLACE

    # Contours as produced by gdal_contour, or by --hgt below.
    if "elevation" in tags or "ele" in tags or "contour" in tags:
        try:
            ele = float(tags.get("elevation") or tags.get("ele"))
        except (TypeError, ValueError):
            return TYPE_CONTOUR
        return TYPE_CONTOUR_INDEX if abs(ele) % 500 < 1e-6 else TYPE_CONTOUR
    return None


# ---------------------------------------------------------------- geometry
def simplify(pts, tol):
    """Douglas-Peucker. Iterative so a long coastline cannot blow the stack."""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    tol2 = tol * tol

    while stack:
        lo, hi = stack.pop()
        if hi <= lo + 1:
            continue
        ax, ay = pts[lo]
        bx, by = pts[hi]
        dx, dy = bx - ax, by - ay
        seg2 = dx * dx + dy * dy

        worst, worst_i = -1.0, -1
        for i in range(lo + 1, hi):
            px, py = pts[i]
            if seg2 == 0.0:
                d2 = (px - ax) ** 2 + (py - ay) ** 2
            else:
                t = ((px - ax) * dx + (py - ay) * dy) / seg2
                t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
                d2 = (px - ax - t * dx) ** 2 + (py - ay - t * dy) ** 2
            if d2 > worst:
                worst, worst_i = d2, i

        if worst > tol2:
            keep[worst_i] = True
            stack.append((lo, worst_i))
            stack.append((worst_i, hi))

    return [p for p, k in zip(pts, keep) if k]


def clip_polyline(pts, x0, y0, x1, y1):
    """Split a polyline into the runs that lie inside the box.

    Each run keeps the crossing point on the boundary so lines meet the tile
    edge cleanly instead of stopping short of it.
    """
    def inside(p):
        return x0 <= p[0] <= x1 and y0 <= p[1] <= y1

    def clip_seg(a, b):
        """Liang-Barsky; returns the visible part of a-b or None."""
        t0, t1 = 0.0, 1.0
        dx, dy = b[0] - a[0], b[1] - a[1]
        for p, q in ((-dx, a[0] - x0), (dx, x1 - a[0]),
                     (-dy, a[1] - y0), (dy, y1 - a[1])):
            if p == 0:
                if q < 0:
                    return None
                continue
            r = q / p
            if p < 0:
                if r > t1:
                    return None
                t0 = max(t0, r)
            else:
                if r < t0:
                    return None
                t1 = min(t1, r)
        return ((a[0] + t0 * dx, a[1] + t0 * dy),
                (a[0] + t1 * dx, a[1] + t1 * dy))

    runs, cur = [], []
    for i in range(len(pts) - 1):
        a, b = pts[i], pts[i + 1]
        seg = clip_seg(a, b)
        if seg is None:
            if len(cur) > 1:
                runs.append(cur)
            cur = []
            continue
        sa, sb = seg
        if not cur:
            cur = [sa]
        elif abs(cur[-1][0] - sa[0]) > 1e-12 or abs(cur[-1][1] - sa[1]) > 1e-12:
            if len(cur) > 1:
                runs.append(cur)
            cur = [sa]
        cur.append(sb)
    if len(cur) > 1:
        runs.append(cur)
    return runs


# ---------------------------------------------------------------- input readers
def read_geojson(path):
    """Yield (type, [(lon, lat), ...], closed)."""
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)

    feats = doc.get("features", [doc]) if doc.get("type") != "FeatureCollection" \
        else doc["features"]

    for feat in feats:
        geom = feat.get("geometry") or {}
        tags = feat.get("properties") or {}
        ftype = classify({str(k): str(v) for k, v in tags.items()})
        if ftype is None:
            continue
        gt, coords = geom.get("type"), geom.get("coordinates")
        if not coords:
            continue

        if gt == "LineString":
            yield ftype, [(c[0], c[1]) for c in coords], False
        elif gt == "MultiLineString":
            for line in coords:
                yield ftype, [(c[0], c[1]) for c in line], False
        elif gt == "Polygon":
            for ring in coords:
                yield ftype, [(c[0], c[1]) for c in ring], True
        elif gt == "MultiPolygon":
            for poly in coords:
                for ring in poly:
                    yield ftype, [(c[0], c[1]) for c in ring], True
        elif gt == "Point":
            yield ftype, [(coords[0], coords[1])], False


def read_osm_xml(path):
    """Yield (type, [(lon, lat), ...], closed) from OSM XML."""
    nodes = {}
    # Two passes so node order in the file does not matter; iterparse and clear
    # as we go, because an .osm can be far larger than memory otherwise.
    for _, el in ET.iterparse(path, events=("end",)):
        if el.tag == "node":
            nodes[el.get("id")] = (float(el.get("lon")), float(el.get("lat")))
        if el.tag in ("node", "way", "relation"):
            el.clear()

    for _, el in ET.iterparse(path, events=("end",)):
        if el.tag == "node":
            tags = {t.get("k"): t.get("v") for t in el.findall("tag")}
            ftype = classify(tags)
            if ftype is not None and el.get("id") in nodes:
                yield ftype, [nodes[el.get("id")]], False
        elif el.tag == "way":
            tags = {t.get("k"): t.get("v") for t in el.findall("tag")}
            ftype = classify(tags)
            if ftype is not None:
                refs = [nd.get("ref") for nd in el.findall("nd")]
                pts = [nodes[r] for r in refs if r in nodes]
                if len(pts) >= 2:
                    closed = refs[0] == refs[-1]
                    yield ftype, pts, closed
        if el.tag in ("node", "way", "relation"):
            el.clear()


def read_pbf(path):
    try:
        import osmium
    except ImportError:
        sys.exit("reading .osm.pbf needs pyosmium (pip install osmium); "
                 "or convert to .osm/.geojson first")

    out = []

    class H(osmium.SimpleHandler):
        def node(self, n):
            ftype = classify(dict(n.tags))
            if ftype is not None:
                out.append((ftype, [(n.location.lon, n.location.lat)], False))

        def way(self, w):
            ftype = classify(dict(w.tags))
            if ftype is None:
                return
            try:
                pts = [(n.lon, n.lat) for n in w.nodes if n.location.valid()]
            except osmium.InvalidLocationError:
                return
            if len(pts) >= 2:
                out.append((ftype, pts, w.is_closed()))

    H().apply_file(path, locations=True)
    return out


def read_hgt(path, step):
    """Contour an SRTM .hgt tile with marching squares. Needs numpy."""
    try:
        import numpy as np
    except ImportError:
        sys.exit("--hgt needs numpy")

    name = os.path.basename(path).upper()
    # Filenames look like N46E007.hgt; the coordinate is the SW corner.
    try:
        lat0 = int(name[1:3]) * (1 if name[0] == "N" else -1)
        lon0 = int(name[4:7]) * (1 if name[3] == "E" else -1)
    except ValueError:
        sys.exit(f"cannot read a lat/lon out of {name}; expected e.g. N46E007.hgt")

    raw = np.fromfile(path, dtype=">i2")
    side = int(round(math.sqrt(raw.size)))
    if side * side != raw.size:
        sys.exit(f"{name}: {raw.size} samples is not a square tile")
    grid = raw.reshape(side, side).astype(np.float32)
    grid[grid < -1000] = np.nan          # SRTM voids

    cell = 1.0 / (side - 1)
    lo = np.nanmin(grid)
    hi = np.nanmax(grid)
    if not math.isfinite(lo) or not math.isfinite(hi):
        return

    first = int(math.ceil(lo / step) * step)
    for level in range(first, int(hi) + 1, step):
        for seg in _march(grid, level, side):
            (r0, c0), (r1, c1) = seg
            # Row 0 of an .hgt is the NORTH edge.
            yield (TYPE_CONTOUR_INDEX if level % (step * 5) == 0 else TYPE_CONTOUR,
                   [(lon0 + c0 * cell, lat0 + 1 - r0 * cell),
                    (lon0 + c1 * cell, lat0 + 1 - r1 * cell)],
                   False)


def _march(grid, level, side):
    """Marching squares, one cell at a time. Yields ((row,col),(row,col)) pairs
    in fractional grid coordinates."""
    import numpy as np

    g = grid
    a = g[:-1, :-1]
    b = g[:-1, 1:]
    c = g[1:, 1:]
    d = g[1:, :-1]

    above = lambda x: x >= level                                   # noqa: E731
    idx = (above(a).astype(np.uint8) |
           (above(b).astype(np.uint8) << 1) |
           (above(c).astype(np.uint8) << 2) |
           (above(d).astype(np.uint8) << 3))

    rows, cols = np.nonzero((idx != 0) & (idx != 15))
    for r, cc in zip(rows.tolist(), cols.tolist()):
        va, vb, vc, vd = g[r, cc], g[r, cc + 1], g[r + 1, cc + 1], g[r + 1, cc]
        if not all(map(math.isfinite, (va, vb, vc, vd))):
            continue

        def ip(v0, v1):
            return 0.5 if v1 == v0 else (level - v0) / (v1 - v0)

        top    = (r,             cc + ip(va, vb))
        right  = (r + ip(vb, vc), cc + 1)
        bottom = (r + 1,         cc + ip(vd, vc))
        left   = (r + ip(va, vd), cc)

        case = int(idx[r, cc])
        table = {
            1:  [(left, top)],      2:  [(top, right)],    3:  [(left, right)],
            4:  [(right, bottom)],  6:  [(top, bottom)],   7:  [(left, bottom)],
            8:  [(bottom, left)],   9:  [(bottom, top)],   11: [(bottom, right)],
            12: [(right, left)],    13: [(right, top)],    14: [(top, left)],
            5:  [(left, top), (right, bottom)],
            10: [(top, right), (bottom, left)],
        }
        for seg in table.get(case, []):
            yield seg


# ---------------------------------------------------------------- writer
def zigzag(v):
    return (v << 1) ^ (v >> 63) if v < 0 else (v << 1)


def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def build(features, out_path, verbose=False):
    if not features:
        sys.exit("no features survived classification; nothing to write")

    min_lon = min(p[0] for _, pts, _ in features for p in pts)
    max_lon = max(p[0] for _, pts, _ in features for p in pts)
    min_lat = min(p[1] for _, pts, _ in features for p in pts)
    max_lat = max(p[1] for _, pts, _ in features for p in pts)

    print(f"bounds  lat {min_lat:.4f}..{max_lat:.4f}  lon {min_lon:.4f}..{max_lon:.4f}")

    level_blobs = []     # (tile_span, n_cols, n_rows, {(col,row): bytes})

    for span, tol, keep in LEVELS:
        n_cols = max(1, int(math.ceil((max_lon - min_lon) / span)))
        n_rows = max(1, int(math.ceil((max_lat - min_lat) / span)))
        if n_cols * n_rows > 65535:
            print(f"  level {span}deg: {n_cols}x{n_rows} tiles is too many, skipped")
            continue

        unit = span / 65536.0
        tiles = defaultdict(bytearray)
        n_feat = 0

        for ftype, pts, closed in features:
            if keep is not None and ftype not in keep:
                continue

            spts = pts if len(pts) < 3 else simplify(pts, tol)
            if len(spts) < (1 if len(pts) == 1 else 2):
                continue

            # Which tiles can this touch?
            f_min_c = int((min(p[0] for p in spts) - min_lon) / span)
            f_max_c = int((max(p[0] for p in spts) - min_lon) / span)
            f_min_r = int((min(p[1] for p in spts) - min_lat) / span)
            f_max_r = int((max(p[1] for p in spts) - min_lat) / span)

            for row in range(max(0, f_min_r), min(n_rows - 1, f_max_r) + 1):
                for col in range(max(0, f_min_c), min(n_cols - 1, f_max_c) + 1):
                    tx0 = min_lon + col * span
                    ty0 = min_lat + row * span
                    tx1, ty1 = tx0 + span, ty0 + span

                    if len(spts) == 1:
                        if not (tx0 <= spts[0][0] <= tx1 and ty0 <= spts[0][1] <= ty1):
                            continue
                        runs = [spts]
                    else:
                        runs = clip_polyline(spts, tx0, ty0, tx1, ty1)

                    for run in runs:
                        for i in range(0, len(run), MAX_PTS_PER_FEATURE - 1):
                            chunk = run[i:i + MAX_PTS_PER_FEATURE]
                            if len(chunk) < len(run) and len(chunk) < 2:
                                continue
                            blob = _encode(ftype, closed and len(runs) == 1,
                                           chunk, tx0, ty0, unit)
                            tiles[(col, row)] += blob
                            n_feat += 1

        used = sum(1 for v in tiles.values() if v)
        total = sum(len(v) for v in tiles.values())
        print(f"  level {span:>8}deg  {n_cols:>4}x{n_rows:<4} tiles, "
              f"{used:>5} non-empty, {n_feat:>6} features, {total/1024:.1f} KiB")
        level_blobs.append((span, n_cols, n_rows, dict(tiles)))

    # ---- lay the file out ----
    n_levels = len(level_blobs)
    header = 32
    level_table = header
    body = level_table + n_levels * 16

    index_offs = []
    for _, n_cols, n_rows, _ in level_blobs:
        index_offs.append(body)
        body += n_cols * n_rows * 8

    payload = bytearray()
    payload_base = body
    entries = []
    for _, n_cols, n_rows, tiles in level_blobs:
        table = []
        for row in range(n_rows):
            for col in range(n_cols):
                blob = tiles.get((col, row))
                if not blob:
                    table.append((0, 0))
                else:
                    table.append((payload_base + len(payload), len(blob)))
                    payload += blob
        entries.append(table)

    with open(out_path, "wb") as f:
        f.write(struct.pack("<6sBB4iII", b"TDMAP\0", 1, n_levels,
                            int(round(min_lat * E7)), int(round(min_lon * E7)),
                            int(round(max_lat * E7)), int(round(max_lon * E7)),
                            level_table, 0))
        for (span, n_cols, n_rows, _), ioff in zip(level_blobs, index_offs):
            f.write(struct.pack("<iHHII", int(round(span * E7)),
                                n_cols, n_rows, ioff, 0))
        for table in entries:
            for off, ln in table:
                f.write(struct.pack("<II", off, ln))
        f.write(payload)

    print(f"wrote {out_path}  {os.path.getsize(out_path)/1024:.1f} KiB")


def _encode(ftype, closed, pts, tx0, ty0, unit):
    body = bytearray()
    prev_x = prev_y = 0
    for i, (lon, lat) in enumerate(pts):
        lx = int(round((lon - tx0) / unit))
        ly = int(round((lat - ty0) / unit))
        lx = 0 if lx < 0 else (65535 if lx > 65535 else lx)
        ly = 0 if ly < 0 else (65535 if ly > 65535 else ly)
        body += varint(zigzag(ly - prev_y))
        body += varint(zigzag(lx - prev_x))
        prev_x, prev_y = lx, ly
    return struct.pack("<BBH", ftype, 1 if closed else 0, len(pts)) + bytes(body)


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help=".geojson / .osm / .osm.pbf")
    ap.add_argument("-o", "--out", required=True, help="output .tdmap")
    ap.add_argument("--hgt", action="append", default=[],
                    help="SRTM .hgt tile to contour (repeatable)")
    ap.add_argument("--contour-step", type=int, default=100,
                    help="contour interval in metres (default 100)")
    args = ap.parse_args()

    features = []
    for path in args.inputs:
        low = path.lower()
        print(f"reading {path}")
        if low.endswith(".pbf"):
            features += read_pbf(path)
        elif low.endswith((".geojson", ".json")):
            features += list(read_geojson(path))
        elif low.endswith(".osm") or low.endswith(".xml"):
            features += list(read_osm_xml(path))
        else:
            sys.exit(f"don't know how to read {path}")

    for path in args.hgt:
        print(f"contouring {path} every {args.contour_step} m")
        features += list(read_hgt(path, args.contour_step))

    counts = defaultdict(int)
    for ftype, _, _ in features:
        counts[ftype] += 1
    print("features: " + ", ".join(
        f"{TYPE_NAMES.get(k, k)}={v}" for k, v in sorted(counts.items())))

    build(features, args.out)


if __name__ == "__main__":
    main()
