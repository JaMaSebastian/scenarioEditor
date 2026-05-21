"""Generates src/toolbar.bmp for ScenarioEditor.

A 240x16 24-bit BMP. 15 cells of 16x16, one per toolbar button.
The upper-left pixel is magenta (255,0,255), which is the standard
MFC toolbar transparent color: CMFCToolBar treats any pixel matching
the upper-left pixel as transparent.

Each cell has a simple recognizable monochrome glyph on the
magenta background:

  N(ew)  O(pen)  S(ave) | (V)alidate (P)review (G)enerate |
  (R)ecord (Replay) (Send Live) | Start Pause Resume Stop | Loop Speed

The glyphs are hand-laid 16x16 pixel-art so we don't need PIL.
"""

import os
import struct

# Cell size is fixed at 16x16 (the design size); CELL_OUT is the rendered
# cell size we want CMFCToolBar to load. The strip is nearest-neighbor
# up-sampled at write time so the rendered toolbar bitmap can match
# CMFCToolBar::SetSizes() exactly without MFC having to stretch.
CELL_OUT = 54
CELL     = 16
W, H     = 16 * CELL, CELL    # design-space strip dimensions

# Colors (BGR for BMP, but we'll use named tuples and pack at write time).
# Background is plain white so the toolbar buttons render with a clean
# white face. CMFCToolBar treats the upper-left pixel as the transparent
# color and substitutes the system button face for it — to *prevent* that
# substitution (which produces the dull "3D face" gray) we want the
# upper-left pixel to NOT match the visible background. The bitmap
# composition (below) writes a unique marker pixel at (0,0) for that.
WHITE   = (255, 255, 255) # icon background, rendered as-is
MAGENTA = WHITE           # legacy alias from earlier transparent design
BLACK   = (0, 0, 0)
DGRAY   = (96, 96, 96)
RED     = (220, 30, 30)
GREEN   = (30, 160, 30)
BLUE    = (30, 80, 200)
ORANGE  = (230, 130, 30)
PURPLE  = (160, 60, 180)
TRANSPARENT_MARKER = (1, 0, 1)   # near-black, won't appear elsewhere

def blank():
    """Return a fresh 16x16 grid filled with magenta (transparent)."""
    return [[MAGENTA for _ in range(16)] for _ in range(16)]

def setp(g, x, y, c):
    if 0 <= x < 16 and 0 <= y < 16:
        g[y][x] = c

def rect(g, x0, y0, x1, y1, c):
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            setp(g, x, y, c)

def hline(g, y, x0, x1, c):
    for x in range(x0, x1 + 1):
        setp(g, x, y, c)

def vline(g, x, y0, y1, c):
    for y in range(y0, y1 + 1):
        setp(g, x, y, c)

# ---------------------------------------------------------------------------
# Icon designs (16x16 each)
# ---------------------------------------------------------------------------

def icon_new():
    """Blank document with a '+' marker."""
    g = blank()
    # document outline
    rect(g, 4, 2, 11, 13, BLACK)
    rect(g, 5, 3, 10, 12, (240, 240, 240))   # paper fill (off-white BGR)
    # plus sign in lower-right
    hline(g, 10, 7, 11, GREEN); hline(g, 11, 7, 11, GREEN)
    vline(g, 8, 9, 12, GREEN); vline(g, 9, 9, 12, GREEN)
    return g

def icon_open():
    """Folder."""
    g = blank()
    # back tab
    rect(g, 2, 4, 7, 6, BLACK)
    rect(g, 3, 5, 6, 5, ORANGE)
    # body
    rect(g, 1, 6, 14, 13, BLACK)
    rect(g, 2, 7, 13, 12, ORANGE)
    return g

def icon_save():
    """Floppy disk."""
    g = blank()
    rect(g, 2, 2, 13, 13, BLACK)
    rect(g, 3, 3, 12, 12, BLUE)
    # label area
    rect(g, 4, 9, 11, 12, (240, 240, 240))
    # metal slider
    rect(g, 4, 3, 11, 6, (200, 200, 200))
    rect(g, 9, 3, 10, 5, BLACK)
    return g

def icon_validate():
    """Check mark."""
    g = blank()
    # checkmark stroke (two diagonal lines)
    pts = [(3,8),(4,9),(5,10),(6,11),(7,10),(8,9),(9,8),(10,7),(11,6),(12,5),(13,4)]
    for x, y in pts:
        setp(g, x, y, GREEN)
        setp(g, x, y+1, GREEN)
    return g

def icon_preview():
    """Eye shape."""
    g = blank()
    # outer almond
    pts_outer = [
        (3,8),(4,6),(5,5),(6,4),(7,4),(8,4),(9,4),(10,5),(11,6),(12,8),
        (12,8),(11,10),(10,11),(9,12),(8,12),(7,12),(6,12),(5,11),(4,10),(3,8),
    ]
    for x, y in pts_outer:
        setp(g, x, y, BLACK)
    # iris
    rect(g, 6, 6, 9, 10, BLUE)
    # pupil
    rect(g, 7, 7, 8, 9, BLACK)
    return g

def icon_generate():
    """Gear (simplified cross+ring)."""
    g = blank()
    # outer ring
    for x, y in [(7,2),(8,2),(7,13),(8,13),(2,7),(2,8),(13,7),(13,8),
                 (4,3),(11,3),(3,4),(12,4),(3,11),(12,11),(4,12),(11,12)]:
        setp(g, x, y, DGRAY)
    # ring body
    for y in range(4, 12):
        for x in range(4, 12):
            dx = x - 7.5
            dy = y - 7.5
            d2 = dx*dx + dy*dy
            if 5 <= d2 <= 16:
                setp(g, x, y, DGRAY)
    # inner hole
    rect(g, 6, 6, 9, 9, MAGENTA)
    return g

def icon_record():
    """Red filled circle."""
    g = blank()
    cx, cy, r = 7.5, 7.5, 5.5
    for y in range(16):
        for x in range(16):
            dx = x - cx
            dy = y - cy
            if dx*dx + dy*dy <= r*r:
                setp(g, x, y, RED)
    return g

def icon_replay():
    """Play triangle over a small film strip."""
    g = blank()
    # film strip rectangle
    rect(g, 2, 3, 13, 12, BLACK)
    rect(g, 3, 4, 12, 11, (60, 60, 60))
    # sprocket holes
    for x in (4, 7, 10):
        rect(g, x, 4, x+1, 5, (240, 240, 240))
        rect(g, x, 10, x+1, 11, (240, 240, 240))
    # play triangle overlay
    for i in range(5):
        for x in range(6, 6 + 5 - i):
            setp(g, x, 6 + i, GREEN if x <= 9 else GREEN)
    # better triangle: rebuild
    return g

def icon_send_live():
    """Antenna/broadcast waves."""
    g = blank()
    # antenna mast
    vline(g, 8, 6, 13, BLACK)
    # base
    hline(g, 13, 6, 10, BLACK)
    # wave arcs (left + right)
    for dx, c in [(-3, BLUE), (-5, BLUE), (3, BLUE), (5, BLUE)]:
        for dy in (-1, 0, 1):
            setp(g, 8 + dx, 5 + dy, c)
    # top dot
    rect(g, 7, 4, 9, 5, RED)
    return g

def icon_start():
    """Play triangle (green)."""
    g = blank()
    for i in range(11):
        for x in range(5, 5 + (11 - i + 1) // 2 + 1):
            if (10 - i) >= (x - 5):
                setp(g, x, 3 + i, GREEN)
    # cleaner triangle:
    for y in range(3, 14):
        h = (y - 3) if y <= 8 else (13 - y)
        for x in range(5, 5 + h + 1):
            setp(g, x, y, GREEN)
    return g

def icon_pause():
    """Two vertical bars."""
    g = blank()
    rect(g, 4, 3, 6, 12, ORANGE)
    rect(g, 9, 3, 11, 12, ORANGE)
    return g

def icon_resume():
    """Play triangle (blue)."""
    g = blank()
    for y in range(3, 14):
        h = (y - 3) if y <= 8 else (13 - y)
        for x in range(5, 5 + h + 1):
            setp(g, x, y, BLUE)
    return g

def icon_stop():
    """Filled square."""
    g = blank()
    rect(g, 4, 4, 11, 11, RED)
    return g

def icon_loop():
    """Circular arrow."""
    g = blank()
    # ring approximation
    cx, cy = 7.5, 7.5
    for y in range(16):
        for x in range(16):
            dx = x - cx
            dy = y - cy
            d2 = dx*dx + dy*dy
            if 16 <= d2 <= 28:
                setp(g, x, y, PURPLE)
    # gap at top-right for arrow
    for x, y in [(10,4),(11,5),(11,4)]:
        setp(g, x, y, MAGENTA)
    # arrowhead
    for x, y in [(10,3),(11,3),(12,3),(12,4),(12,5),(13,5),(11,6),(12,6)]:
        setp(g, x, y, PURPLE)
    return g

def icon_speed():
    """Speedometer needle."""
    g = blank()
    # dial arc (lower half)
    cx, cy = 7.5, 10.0
    for y in range(16):
        for x in range(16):
            dx = x - cx
            dy = y - cy
            d2 = dx*dx + dy*dy
            if 18 <= d2 <= 28 and dy <= 0:
                setp(g, x, y, DGRAY)
    # needle pointing up-right
    pts = [(7,10),(8,9),(9,8),(10,7),(11,6),(12,5)]
    for x, y in pts:
        setp(g, x, y, RED)
        setp(g, x+1 if x < 15 else x, y, RED)
    # hub
    rect(g, 6, 9, 9, 11, BLACK)
    return g

def icon_help():
    """Question mark on magenta background."""
    g = blank()
    # upper curve of '?'
    for x, y in [(6,3),(7,3),(8,3),(9,3),
                 (5,4),(10,4),
                 (10,5),
                 (9,6),(10,6),
                 (8,7),(9,7),
                 (7,8),(8,8)]:
        setp(g, x, y, BLUE)
    # stem
    rect(g, 7, 9, 8, 10, BLUE)
    # dot
    rect(g, 7, 12, 8, 13, BLUE)
    return g

ICONS = [
    icon_new(),
    icon_open(),
    icon_save(),
    icon_validate(),
    icon_preview(),
    icon_generate(),
    icon_record(),
    icon_replay(),
    icon_send_live(),
    icon_start(),
    icon_pause(),
    icon_resume(),
    icon_stop(),
    icon_loop(),
    icon_speed(),
    icon_help(),
]

assert len(ICONS) == 16, len(ICONS)

# ---------------------------------------------------------------------------
# Compose the strip and emit a 24-bit BMP.
# ---------------------------------------------------------------------------

# 1) Compose the design-space (16-tall) strip from the 16x16 cells.
design = [[MAGENTA for _ in range(W)] for _ in range(H)]
for col, icon in enumerate(ICONS):
    x0 = col * CELL
    for y in range(CELL):
        for x in range(CELL):
            design[y][x0 + x] = icon[y][x]

# 2) Up-sample to CELL_OUT via nearest-neighbor. Each output pixel maps to
#    the nearest design pixel; CELL_OUT need not be an integer multiple of
#    CELL. Row width must stay a multiple of 4 for BMP — pad if needed.
out_w = 16 * CELL_OUT
out_h = CELL_OUT
assert (out_w * 3) % 4 == 0, "out_w * 3 must be a multiple of 4 (no BMP padding here)"

strip = [[WHITE for _ in range(out_w)] for _ in range(out_h)]
# Per-cell mapping keeps each glyph's design-space samples local to its cell.
for col in range(16):
    src_x0 = col * CELL
    dst_x0 = col * CELL_OUT
    for oy in range(CELL_OUT):
        src_y = (oy * CELL) // CELL_OUT
        for ox in range(CELL_OUT):
            src_x = (ox * CELL) // CELL_OUT
            strip[oy][dst_x0 + ox] = design[src_y][src_x0 + src_x]

# Mark the upper-left pixel as the "transparent" marker so CMFCToolBar
# substitutes only that one pixel with the button face — not the rest
# of the icon background, which stays white.
strip[0][0] = TRANSPARENT_MARKER

row_bytes = out_w * 3
assert row_bytes % 4 == 0, row_bytes
pixel_size = row_bytes * out_h

bmp_offset = 14 + 40  # file header + DIB header
file_size = bmp_offset + pixel_size

# BITMAPFILEHEADER
file_header = b"BM" + struct.pack("<I H H I", file_size, 0, 0, bmp_offset)
# BITMAPINFOHEADER (40 bytes)
dib_header = struct.pack(
    "<I i i H H I I i i I I",
    40,         # biSize
    out_w,      # biWidth
    out_h,      # biHeight (positive => bottom-up)
    1,          # biPlanes
    24,         # biBitCount
    0,          # biCompression = BI_RGB
    pixel_size, # biSizeImage
    2835,       # biXPelsPerMeter (~72 DPI)
    2835,       # biYPelsPerMeter
    0,          # biClrUsed
    0,          # biClrImportant
)

pixel_bytes = bytearray(pixel_size)
# BMP rows are bottom-to-top.
for src_y in range(out_h):
    dst_row = out_h - 1 - src_y
    base = dst_row * row_bytes
    for x in range(out_w):
        b, g, r = strip[src_y][x]
        idx = base + x * 3
        pixel_bytes[idx]     = b
        pixel_bytes[idx + 1] = g
        pixel_bytes[idx + 2] = r

out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "toolbar.bmp")
with open(out_path, "wb") as f:
    f.write(file_header)
    f.write(dib_header)
    f.write(pixel_bytes)

print(f"Wrote {out_path} ({file_size} bytes)")
