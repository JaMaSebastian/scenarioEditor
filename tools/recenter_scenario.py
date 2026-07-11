#!/usr/bin/env python3
# One-off: re-center harburtField.ini's ENU world onto the UE beach terrain.
#
# The UE landscape (set_landscape_transform.py) is anchored at the origin: shoreline at
# North=0, land North[0,10km], East[+/-10km]. But the scenario placed its Level rects and
# all entity positions ~40 km north / ~15 km west of the origin, so DIS entities arrived
# 40 km from the terrain. Fix = pure translation of every ENU coordinate:
#     dE (East)  = +14768.0   (Land East center -14768 -> 0)
#     dN (North) = -34907.9   (Land south edge 34907.9 -> shoreline 0)
# Z (Up) is never shifted. Caches (ECEF + geodetic) are recomputed from the shifted Local
# so the file stays self-consistent. Run from the ScenarioEditor dir; edits in place.

import math, sys, re

dE = 14768.0
dN = -34907.9

# Scenario origin (== DISBrowser EarthOrigin); see [Origin] in harburtField.ini.
OLAT = math.radians(30.395911)
OLON = math.radians(-86.686803)
OALT = 0.0

A = 6378137.0
F = 1.0 / 298.257223563
E2 = 2.0 * F - F * F

def geodetic_to_ecef(latr, lonr, alt):
    sl, cl = math.sin(latr), math.cos(latr)
    so, co = math.sin(lonr), math.cos(lonr)
    n = A / math.sqrt(1.0 - E2 * sl * sl)
    return ((n + alt) * cl * co, (n + alt) * cl * so, (n * (1.0 - E2) + alt) * sl)

OX, OY, OZ = geodetic_to_ecef(OLAT, OLON, OALT)
SLAT, CLAT = math.sin(OLAT), math.cos(OLAT)
SLON, CLON = math.sin(OLON), math.cos(OLON)

def enu_to_ecef(e, n, u):
    x = OX + (-SLON) * e + (-SLAT * CLON) * n + (CLAT * CLON) * u
    y = OY + ( CLON) * e + (-SLAT * SLON) * n + (CLAT * SLON) * u
    z = OZ +                ( CLAT)        * n + ( SLAT)       * u
    return x, y, z

def ecef_to_geodetic(x, y, z):
    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    lat = math.atan2(z, p * (1.0 - E2))
    for _ in range(8):
        sl = math.sin(lat)
        n = A / math.sqrt(1.0 - E2 * sl * sl)
        h = p / math.cos(lat) - n
        newlat = math.atan2(z, p * (1.0 - E2 * n / (n + h)))
        if abs(newlat - lat) < 1e-13:
            lat = newlat
            break
        lat = newlat
    sl = math.sin(lat)
    n = A / math.sqrt(1.0 - E2 * sl * sl)
    alt = p / math.cos(lat) - n
    return math.degrees(lat), math.degrees(lon), alt

def fmt(v):
    return repr(float(v))

path = "harburtField.ini"
with open(path, "r", encoding="utf-8") as fh:
    lines = fh.readlines()

# Split into sections: (header_or_None, [raw_line_indices]).
sections = []
cur = (None, [])
for i, ln in enumerate(lines):
    if ln.lstrip().startswith("["):
        sections.append(cur)
        cur = (ln.strip(), [])
    cur[1].append(i)
sections.append(cur)

kv = re.compile(r"^(\w+)=(.*?)\s*$")
shifted_groups = 0
shifted_levels = 0
garc_dbg = None

def set_val(idxmap, key, value):
    if key in idxmap:
        lines[idxmap[key]] = f"{key}={value}\n"

for header, idxs in sections:
    if header is None:
        continue
    # key -> line index within this section
    idxmap = {}
    for i in idxs:
        m = kv.match(lines[i])
        if m:
            idxmap[m.group(1)] = i

    def getf(key):
        m = kv.match(lines[idxmap[key]])
        return float(m.group(2))

    # Level rects: shift East/North bounds (heights untouched).
    if header.startswith("[Level"):
        for k in ("EastMinMeters", "EastMaxMeters"):
            if k in idxmap:
                set_val(idxmap, k, fmt(getf(k) + dE));
        for k in ("NorthMinMeters", "NorthMaxMeters"):
            if k in idxmap:
                set_val(idxmap, k, fmt(getf(k) + dN))
        if "EastMinMeters" in idxmap:
            shifted_levels += 1
        continue

    # Position groups: Initial / Start / End / Focus1 / Focus2.
    for pre in ("Initial", "Start", "End", "Focus1", "Focus2"):
        lx, ly, lz = pre + "LocalX", pre + "LocalY", pre + "LocalZ"
        if lx not in idxmap or ly not in idxmap:
            continue
        x = getf(lx); y = getf(ly)
        if x == 0.0 and y == 0.0:
            continue  # placeholder / unused field
        z = getf(lz) if lz in idxmap else 0.0
        ne, nn = x + dE, y + dN
        ex, ey, ez = enu_to_ecef(ne, nn, z)
        latd, lond, altm = ecef_to_geodetic(ex, ey, ez)
        set_val(idxmap, lx, fmt(ne)); set_val(idxmap, ly, fmt(nn))
        set_val(idxmap, pre + "EcefX", fmt(ex)); set_val(idxmap, pre + "EcefY", fmt(ey)); set_val(idxmap, pre + "EcefZ", fmt(ez))
        set_val(idxmap, pre + "LatitudeDeg", fmt(latd)); set_val(idxmap, pre + "LongitudeDeg", fmt(lond)); set_val(idxmap, pre + "AltitudeMeters", fmt(altm))
        shifted_groups += 1
        if header.startswith("[Entity.5.Motion") and pre == "Start":
            garc_dbg = (ne, nn, z, latd, lond, altm)

with open(path, "w", encoding="utf-8") as fh:
    fh.writelines(lines)

print(f"shifted position groups: {shifted_groups}")
print(f"shifted level rects:     {shifted_levels}")
if garc_dbg:
    print("GARC motion start now: ENU(E=%.2f N=%.2f U=%.2f)  geo(lat=%.6f lon=%.6f alt=%.2f m)" % garc_dbg)
