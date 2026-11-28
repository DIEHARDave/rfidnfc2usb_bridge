"""Generate the LaneTag USB bridge logos from LaneTag's (the ESP32 Wi-Fi
reader).

Usage: python make_logo.py LANETAG_LOGO_DIR Poppins-Bold.ttf OUTPUT_DIR

LANETAG_LOGO_DIR is LaneTag's docs/logo (lanetag-logo.svg,
lanetag-mark.svg). The bridge keeps LaneTag's spool, reader card and ESP32
board, and a USB plug comes out of the board: the board puts the reader on
USB. The orange accent becomes purple, and a "BRIDGE" tag sits under the
wordmark (Poppins Bold, SIL OFL, converted to outlines like LaneTag's).
LaneTag's canvas and wordmark size are kept, so the family logos line up.
Needs fontTools. Sister of lanetag-nfc's docs/logo/make_logo.py (teal,
"USB").
"""
import math
import os
import re
import sys

from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

SRC, FONT, OUT = sys.argv[1], sys.argv[2], sys.argv[3]

INK = "#1b2632"
SLATE_D = "#243241"
GREY = "#c8d0d8"
WHITE = "#ffffff"
DARK_TEXT = "#eef2f6"
# LaneTag's orange -> purple for the bridge
ACCENT = {"#f28a1e": "#7e57c2", "#c9650f": "#5a3a9a"}
PURPLE = ACCENT["#f28a1e"]
# LaneTag's ESP32 board: centre, angle, size (LaneTag make_logo.py ESP_C...)
ESP_C, ESP_DEG, ESP_W, ESP_H = (108, 296), 5, 76, 104


def f(v):
    s = "%.2f" % v
    s = s.rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


def rot(px, py, cx, cy, deg):
    a = math.radians(deg)
    dx, dy = px - cx, py - cy
    return cx + dx * math.cos(a) - dy * math.sin(a), cy + dx * math.sin(a) + dy * math.cos(a)


def recolor(svg):
    for old, new in ACCENT.items():
        svg = svg.replace(old, new)
    return svg


def usb_plug(ax, ay, deg, scale):
    """USB-A plug whose cable end is (ax, ay), drawn hanging down and then
    rotated deg about that point."""
    bw, bh = 50, 62          # overmold
    sw, sh = 40, 46          # metal shell
    x, y = ax - bw / 2, ay - 4
    cx, ty = ax, y + 12
    o = ['<g id="usb-plug" transform="rotate(%s %s %s) translate(%s %s) scale(%s) translate(%s %s)">'
         % (deg, f(ax), f(ay), f(ax), f(ay), scale, f(-ax), f(-ay))]
    o.append('<rect x="%s" y="%s" width="18" height="16" rx="5" fill="%s" stroke="%s" stroke-width="3"/>'
             % (f(ax - 9), f(y - 10), SLATE_D, INK))
    o.append('<rect x="%s" y="%s" width="%d" height="%d" rx="10" fill="%s" stroke="%s" stroke-width="3"/>'
             % (f(x), f(y), bw, bh, SLATE_D, INK))
    o.append('<g id="usb-symbol" fill="none" stroke="%s" stroke-width="3.2" stroke-linecap="round" '
             'stroke-linejoin="round">' % GREY)
    o.append('<path d="M%s %sV%s"/>' % (f(cx), f(ty + 34), f(ty + 4)))
    o.append('<path d="M%s %sL%s %sV%s"/>' % (f(cx), f(ty + 27), f(cx - 11), f(ty + 18), f(ty + 12)))
    o.append('<path d="M%s %sL%s %sV%s"/>' % (f(cx), f(ty + 22), f(cx + 11), f(ty + 14), f(ty + 9)))
    o.append("</g>")
    o.append('<path d="M%s %sL%s %sL%s %sZ" fill="%s"/>'
             % (f(cx - 5), f(ty + 6), f(cx), f(ty - 2), f(cx + 5), f(ty + 6), GREY))
    o.append('<circle cx="%s" cy="%s" r="3.4" fill="%s"/>' % (f(cx - 11), f(ty + 11), GREY))
    o.append('<rect x="%s" y="%s" width="6.8" height="6.8" fill="%s"/>' % (f(cx + 7.6), f(ty + 5.6), GREY))
    o.append('<circle cx="%s" cy="%s" r="3.6" fill="%s"/>' % (f(cx), f(ty + 36), GREY))
    sx, sy = ax - sw / 2, y + bh - 3
    o.append('<rect x="%s" y="%s" width="%d" height="%d" rx="3" fill="%s" stroke="%s" stroke-width="3"/>'
             % (f(sx), f(sy), sw, sh, GREY, INK))
    o.append('<rect x="%s" y="%s" width="9" height="7" rx="1" fill="%s"/>' % (f(sx + 7), f(sy + 8), INK))
    o.append('<rect x="%s" y="%s" width="9" height="7" rx="1" fill="%s"/>' % (f(sx + sw - 16), f(sy + 8), INK))
    o.append('<rect x="%s" y="%s" width="%d" height="16" rx="2" fill="%s" stroke="%s" stroke-width="2"/>'
             % (f(sx + 5), f(sy + 22), sw - 10, WHITE, INK))
    o.append("</g>")
    return "\n".join(o)


def board_usb():
    """A cable from the ESP32 board's USB port (bottom edge) to a plug
    lying along the bottom, pointing right."""
    cx, cy = ESP_C
    port = rot(cx, cy + ESP_H / 2, cx, cy, ESP_DEG)
    end = (port[0] + 34, 371)
    d = "M%s %sC%s %s %s %s %s %s" % (f(port[0]), f(port[1]), f(port[0]), f(port[1] + 22),
                                      f(end[0] - 22), f(end[1]), f(end[0]), f(end[1]))
    cable = ('<g id="usb-cable" fill="none" stroke-linecap="round">'
             '<path d="%s" stroke="%s" stroke-width="10"/><path d="%s" stroke="%s" stroke-width="5"/></g>'
             % (d, INK, d, GREY))
    return cable + "\n" + usb_plug(end[0], end[1], -90, 0.76)


def text_path(text, x, baseline, size, color, path_id, tracking=0.02):
    font = TTFont(FONT)
    gs, cmap = font.getGlyphSet(), font.getBestCmap()
    s = size / font["head"].unitsPerEm
    px, ds = x, []
    for ch in text:
        pen = SVGPathPen(gs, ntos=f)
        gs[cmap[ord(ch)]].draw(TransformPen(pen, (s, 0, 0, -s, px, baseline)))
        ds.append(pen.getCommands())
        px += gs[cmap[ord(ch)]].width * s + tracking * size
    return '<path id="%s" fill="%s" d="%s"/>' % (path_id, color, "".join(ds)), px - x - tracking * size



def wordmark_ink():
    """Ink box of LaneTag's wordmark (LaneTag make_logo.py: x 444,
    baseline 244, size 128, tracking -0.01)."""
    font = TTFont(FONT)
    gs, cmap = font.getGlyphSet(), font.getBestCmap()
    s = 128 / font["head"].unitsPerEm
    pen_x, box = 444, None
    for ch in "LaneTag":
        g = gs[cmap[ord(ch)]]
        bp = BoundsPen(gs)
        g.draw(TransformPen(bp, (s, 0, 0, -s, pen_x, 244)))
        if bp.bounds:
            b = bp.bounds
            box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
        pen_x += g.width * s - 0.01 * 128
    return box


def tag(word):
    """Purple tag under the wordmark, right-aligned with its ink; the
    canvas and the wordmark stay exactly LaneTag's."""
    size, pad_x, h, gap = 40, 16, 56, 18
    x0, _, x1, y1 = wordmark_ink()      # y1: bottom of the "g"
    _, w = text_path(word, 0, 0, size, WHITE, "word-tag")
    tw = w + 2 * pad_x
    x, top = x1 - tw, y1 + gap
    # cap height of Poppins Bold ~0.7 em: centre the letters in the tag
    glyphs, _ = text_path(word, x + pad_x, top + h / 2 + 0.35 * size, size, WHITE,
                          "word-" + word.lower())
    rect = '<rect x="%s" y="%s" width="%s" height="%d" rx="12" fill="%s"/>' % (f(x), f(top), f(tw), h, PURPLE)
    return '<g id="%s-tag">\n%s\n%s\n</g>' % (word.lower(), rect, glyphs)


def lockup(src_svg, lane_color):
    svg = recolor(src_svg)
    # the plug and its cable go under the board, over the spool
    svg = re.sub(r'(<g id="esp32")', board_usb().replace("\\", "\\\\") + r"\n\1", svg, count=1)
    if 'id="usb-plug"' not in svg:
        raise SystemExit("no esp32 group in " + SRC)
    svg = re.sub(r'(<path id="word-lane" fill=")[^"]+', r"\g<1>" + lane_color, svg)
    t = tag("BRIDGE")
    svg = svg.replace("<title>LaneTag</title>", "<title>LaneTag USB bridge (lanetag-usb-bridge)</title>")
    return svg.replace("</svg>", t + "\n</svg>")


src = open(os.path.join(SRC, "lanetag-logo.svg"), encoding="utf-8").read()
for name, lane in (("lanetag-usb-bridge-logo.svg", SLATE_D),
                   ("lanetag-usb-bridge-logo-dark.svg", DARK_TEXT)):
    open(os.path.join(OUT, name), "w", encoding="utf-8").write(lockup(src, lane))
mark = recolor(open(os.path.join(SRC, "lanetag-mark.svg"), encoding="utf-8").read())
mark = mark.replace("<title>LaneTag</title>", "<title>LaneTag USB bridge (lanetag-usb-bridge)</title>")
open(os.path.join(OUT, "lanetag-usb-bridge-mark.svg"), "w", encoding="utf-8").write(mark)
