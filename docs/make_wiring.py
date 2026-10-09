#!/usr/bin/env python3
"""Draws docs/wiring.png, the wiring picture in the README.

Nothing here is a photo: the board and every part are drawn in code. Everything
is drawn at twice the size and scaled down, which keeps lines and text smooth.

    python3 docs/make_wiring.py

Needs Pillow. The font path is the macOS Helvetica; change FONT elsewhere.
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "wiring.png")
FONT = "/System/Library/Fonts/Helvetica.ttc"

S = 2                                  # supersampling factor
WIRE, CASING, LANE = 6, 12, 22
RED, ORANGE, GRAY = (226, 75, 74), (240, 125, 20), (90, 90, 88)
BLUE, GREEN, PURPLE = (55, 138, 221), (99, 153, 34), (142, 68, 173)
TEXT, MUTED = (24, 28, 36), (110, 116, 128)
GOLD, GOLD_EDGE = (222, 186, 90), (150, 120, 50)
PANEL, PANEL_EDGE = (246, 247, 249), (226, 229, 234)
SILVER, SILVER_EDGE, STEEL = (196, 199, 205), (128, 131, 138), (104, 107, 114)
T_GREEN, T_PURPLE, T_TEAL, T_SLATE, T_SALMON, T_RED, T_BLACK = (
    (0, 168, 62), (128, 108, 228), (0, 178, 158), (104, 120, 142), (248, 160, 130), (226, 38, 38), (16, 16, 18))


def font(size, bold=False):
    return ImageFont.truetype(FONT, size * S, index=1 if bold else 0)


kit_font, title_font = font(44, True), font(40)
module_font, legend_font = font(30, True), font(28)
pin_font, note_font, chip_font = font(25), font(24), font(17, True)
tag_font, silk_font, tiny_font = font(21, True), font(17, True), font(12, True)


class Layer:
    """Takes ordinary coordinates and draws at S times the size."""

    def __init__(self, w, h):
        self.size = (w, h)
        self.im = Image.new("RGBA", (w * S, h * S), "white")
        self.d = ImageDraw.Draw(self.im, "RGBA")

    def text(self, xy, text, fnt, fill=TEXT, anchor="la"):
        self.d.text((xy[0] * S, xy[1] * S), text, font=fnt, fill=fill, anchor=anchor)

    def rect(self, box, radius, fill, outline=None, width=0):
        self.d.rounded_rectangle([v * S for v in box], radius=radius * S, fill=fill, outline=outline, width=width * S)

    def circle(self, p, r, fill, outline=None, width=0):
        self.d.ellipse(((p[0] - r) * S, (p[1] - r) * S, (p[0] + r) * S, (p[1] + r) * S),
                       fill=fill, outline=outline, width=width * S)

    def line(self, pts, colour, width=WIRE, radius=14):
        """A line through pts with rounded corners."""
        pts = [(x * S, y * S) for x, y in pts]
        w, d = width * S, self.d
        prev = pts[0]
        for i in range(1, len(pts) - 1):
            a, p, b = pts[i - 1], pts[i], pts[i + 1]
            la, lb = math.dist(a, p), math.dist(p, b)
            if la == 0 or lb == 0:
                continue
            u = ((p[0] - a[0]) / la, (p[1] - a[1]) / la)
            v = ((b[0] - p[0]) / lb, (b[1] - p[1]) / lb)
            r = min(radius * S, la / 2, lb / 2)
            if u == v or r < 2:
                d.line((prev, p), fill=colour, width=w)
                prev = p
                continue
            p_in = (p[0] - u[0] * r, p[1] - u[1] * r)
            p_out = (p[0] + v[0] * r, p[1] + v[1] * r)
            c = (p_in[0] + v[0] * r, p_in[1] + v[1] * r)
            d.line((prev, p_in), fill=colour, width=w)
            a1 = math.degrees(math.atan2(p_in[1] - c[1], p_in[0] - c[0])) % 360
            a2 = math.degrees(math.atan2(p_out[1] - c[1], p_out[0] - c[0])) % 360
            start, end = (a1, a2) if round((a2 - a1) % 360) == 90 else (a2, a1)
            R = r + w / 2
            d.arc((c[0] - R, c[1] - R, c[0] + R, c[1] + R), start, end, fill=colour, width=w)
            for q in (p_in, p_out):        # hide the seam between line and arc
                d.ellipse((q[0] - w / 2, q[1] - w / 2, q[0] + w / 2, q[1] + w / 2), fill=colour)
            prev = p_out
        d.line((prev, pts[-1]), fill=colour, width=w)

    def wire(self, pts, colour):
        """A wire with a white casing, so it reads clearly where it crosses another."""
        self.line(pts, "white", CASING)
        self.line(pts, colour)

    def end(self, p, colour):
        """Where a wire is soldered: a dot in the wire's colour."""
        self.circle(p, 9, colour, (255, 255, 255), 2)

    def save(self, path):
        self.im.resize(self.size, Image.LANCZOS).convert("RGB").save(path, optimize=True)


# ------------------------------------------------------ the XIAO ESP32-C6 --
LEFT_PADS = ["D0", "D1", "D2", "D3", "D4", "D5", "D6"]
RIGHT_PADS = ["5V", "GND", "3V3", "D10", "D9", "D8", "D7"]
# Each pad's other names, shown as extra tags beside it.
PAD_EXTRAS = {"D0": [("GPIO0", T_SLATE), ("A0", T_SALMON)], "D1": [("GPIO1", T_SLATE), ("A1", T_SALMON)],
              "D2": [("GPIO2", T_SLATE), ("A2", T_SALMON)], "D3": [("GPIO21", T_SLATE)],
              "D4": [("GPIO22", T_SLATE), ("SDA", T_TEAL)], "D5": [("GPIO23", T_SLATE), ("SCL", T_TEAL)],
              "D6": [("GPIO16", T_SLATE), ("TX", T_PURPLE)], "D10": [("GPIO18", T_SLATE), ("MOSI", T_PURPLE)],
              "D9": [("GPIO20", T_SLATE), ("MISO", T_PURPLE)], "D8": [("GPIO19", T_SLATE), ("SCK", T_PURPLE)],
              "D7": [("GPIO17", T_SLATE), ("RX", T_PURPLE)]}
BOARD_W, BOARD_H, PAD_PITCH, PAD_TOP, BOARD_Y = 330, 420, 49, 48, 204


def tag_width(text):
    return tag_font.getlength(text) / S + 22


def tags_extent(names):
    return max(sum(tag_width(t) + 12 for t in [n] + [e[0] for e in PAD_EXTRAS.get(n, [])]) for n in names)


def board_pads(x):
    pads = {n: (x + 17, BOARD_Y + PAD_TOP + i * PAD_PITCH) for i, n in enumerate(LEFT_PADS)}
    pads.update({n: (x + BOARD_W - 17, BOARD_Y + PAD_TOP + i * PAD_PITCH) for i, n in enumerate(RIGHT_PADS)})
    return pads


def draw_board(layer, x, gap_l, gap_r):
    """The XIAO ESP32-C6 from above, left edge at x. gap_l / gap_r is the clear
    space between the board and its pin tags, where the wires run."""
    y, cx, box = BOARD_Y, x + BOARD_W / 2, layer.rect

    def tag(text, colour, tx, ty, side):
        w = tag_width(text)
        x0 = tx if side > 0 else tx - w
        box((x0, ty - 17, x0 + w, ty + 17), 6, colour)
        layer.text((x0 + w / 2, ty + 1), text, tag_font, "white", "mm")
        return w

    box((x + 6, y + 8, x + BOARD_W + 6, y + BOARD_H + 8), 22, (0, 0, 0, 40))
    box((x, y, x + BOARD_W, y + BOARD_H), 22, (26, 28, 34), (6, 6, 8), 2)

    for name, (px, py) in board_pads(x).items():
        side = -1 if name in LEFT_PADS else 1
        box((px - 17, py - 15, px + 17, py + 15), 9, GOLD, GOLD_EDGE, 1)
        layer.circle((px, py), 8, (245, 245, 240), GOLD_EDGE, 1)
        colour = T_RED if name in ("5V", "3V3") else T_BLACK if name == "GND" else T_GREEN
        tx = (x - gap_l) if side < 0 else (x + BOARD_W + gap_r)
        for text, colour in [(name, colour)] + PAD_EXTRAS.get(name, []):
            tx += side * (tag(text, colour, tx, py, side) + 12)
        layer.text((px + side * -24, py), "G" if name == "GND" else name, silk_font, (232, 232, 236),
                   "lm" if side < 0 else "rm")

    # USB-C socket, overhanging the top edge.
    box((cx - 70, y - 30, cx + 70, y + 96), 16, SILVER, SILVER_EDGE, 2)
    box((cx - 54, y - 20, cx + 54, y + 4), 10, (150, 153, 160), STEEL, 2)
    box((cx - 42, y - 14, cx + 42, y - 4), 5, (34, 36, 42))

    # BOOT and RESET buttons, either side of the socket's back end.
    for bx, label in ((cx - 48, "BOOT"), (cx + 48, "RESET")):
        box((bx - 20, y + 112, bx + 20, y + 140), 4, SILVER, SILVER_EDGE, 2)
        layer.circle((bx, y + 126), 8, (70, 73, 82), (40, 42, 48), 2)
        layer.text((bx, y + 152), label, tiny_font, (225, 225, 230), "mm")

    # The radio module under its metal shield.
    box((cx - 78, y + 172, cx + 78, y + 318), 8, (206, 208, 214), SILVER_EDGE, 2)
    box((cx - 70, y + 180, cx + 70, y + 310), 6, (188, 191, 198))
    layer.text((cx, y + 236), "ESP32-C6", chip_font, (60, 62, 70), "mm")
    layer.text((cx, y + 258), "Zigbee", chip_font, (60, 62, 70), "mm")

    # Ceramic antenna and the U.FL socket for the external one, at the far end.
    box((cx - 6, y + BOARD_H - 62, cx + 96, y + BOARD_H - 28), 4, (236, 236, 232), (150, 150, 146), 2)
    box((cx + 14, y + BOARD_H - 58, cx + 76, y + BOARD_H - 32), 2, (204, 54, 54))
    box((x + 64, y + BOARD_H - 66, x + 104, y + BOARD_H - 26), 5, GOLD, GOLD_EDGE, 2)
    layer.circle((x + 84, y + BOARD_H - 46), 11, (240, 240, 236), GOLD_EDGE, 2)
    layer.circle((x + 84, y + BOARD_H - 46), 4, GOLD_EDGE)
    layer.text((x + 112, y + BOARD_H - 46), "U.FL", tiny_font, (225, 225, 230), "lm")


# ------------------------------------------------------------- the parts --
def draw_sensor(layer, x, y, pins):
    """AHT20 + BMP280 module, pins down its left edge. Returns {pin: (x, y)}."""
    w, h = 280, 190
    layer.rect((x + 5, y + 7, x + w + 5, y + h + 7), 10, (0, 0, 0, 40))
    layer.rect((x, y, x + w, y + h), 10, (104, 52, 150), (66, 30, 100), 2)
    at = {}
    for i, name in enumerate(pins):
        p = (x + 28, y + 32 + i * 42)
        layer.circle(p, 13, GOLD, GOLD_EDGE, 1)
        layer.text((p[0] + 28, p[1] + 1), name, pin_font, "white", "lm")
        at[name] = p
    layer.rect((x + 180, y + 26, x + 228, y + 76), 3, (26, 26, 30), (90, 90, 96), 2)
    layer.circle((x + 204, y + 51), 6, (240, 240, 240))
    layer.text((x + 204, y + 94), "AHT20", chip_font, "white", "mm")
    layer.rect((x + 188, y + 120, x + 220, y + 152), 3, SILVER, SILVER_EDGE, 2)
    layer.circle((x + 195, y + 127), 3, (90, 90, 96))
    layer.text((x + 204, y + 170), "BMP280", chip_font, "white", "mm")
    return at


def draw_resistor(layer, cx, y0, y1, label):
    """An upright resistor between y0 and y1, with the bands of 1 M ohm."""
    layer.line([(cx, y0), (cx, y1)], (150, 152, 158), 4)
    top, bottom = (y0 + y1) / 2 - 36, (y0 + y1) / 2 + 36
    layer.rect((cx - 15, top, cx + 15, bottom), 10, (222, 200, 160), (150, 128, 90), 2)
    for k, colour in enumerate(((120, 70, 30), (20, 20, 20), (40, 150, 60), (200, 160, 40))):
        yy = top + 12 + k * 13 + (6 if k == 3 else 0)
        layer.rect((cx - 15, yy, cx + 15, yy + 7), 0, colour)
    layer.text((cx + 28, (y0 + y1) / 2 - 14), label, pin_font, TEXT, "lm")
    layer.text((cx + 28, (y0 + y1) / 2 + 14), "1 MΩ", note_font, MUTED, "lm")


def draw_capacitor(layer, cx, y0, y1):
    """A ceramic disc capacitor between y0 and y1."""
    layer.line([(cx, y0), (cx, y1)], (150, 152, 158), 4)
    cy = (y0 + y1) / 2
    layer.circle((cx, cy), 24, (226, 150, 60), (160, 96, 30), 2)
    layer.text((cx, cy + 1), "104", tiny_font, (90, 50, 10), "mm")
    layer.text((cx + 36, cy - 14), "C1", pin_font, TEXT, "lm")
    layer.text((cx + 36, cy + 14), "100 nF", note_font, MUTED, "lm")


def draw_cell(layer, cx, y0, y1):
    """An upright Li-ion cell, positive end up, between y0 and y1."""
    layer.rect((cx - 16, y0, cx + 16, y0 + 16), 4, SILVER, SILVER_EDGE, 2)
    layer.rect((cx - 46, y0 + 10, cx + 46, y1), 12, (38, 120, 168), (20, 78, 116), 2)
    layer.rect((cx - 46, y0 + 10, cx + 46, y0 + 36), 12, (232, 236, 240), (20, 78, 116), 2)
    layer.text((cx, y0 + 62), "+", module_font, "white", "mm")
    layer.text((cx, y1 - 26), "−", module_font, "white", "mm")
    layer.text((cx, (y0 + y1) / 2 + 6), "Li-ion", chip_font, "white", "mm")


def draw_underside(layer, x, y, y_plus, y_minus):
    """The back of the board, where the two battery pads are. Returns their centres."""
    w = 200
    top, bottom = y_plus - 50, y_minus + 50
    layer.rect((x + 5, top + 7, x + w + 5, bottom + 7), 18, (0, 0, 0, 40))
    layer.rect((x, top, x + w, bottom), 18, (26, 28, 34), (6, 6, 8), 2)
    at = {}
    for name, py in (("BAT+", y_plus), ("BAT−", y_minus)):
        p = (x + 34, py)
        layer.rect((p[0] - 20, py - 17, p[0] + 20, py + 17), 6, GOLD, GOLD_EDGE, 1)
        layer.text((p[0] + 34, py + 1), name, pin_font, "white", "lm")
        at[name] = p
    layer.text((x + w / 2, (y_plus + y_minus) / 2 - 12), "underside", silk_font, (150, 152, 160), "mm")
    layer.text((x + w / 2, (y_plus + y_minus) / 2 + 12), "of the board", silk_font, (150, 152, 160), "mm")
    return at


def draw_antenna(layer, x, y, socket):
    """A flexible 2.4 GHz antenna lying under the board, on a short lead from the U.FL socket."""
    layer.line([socket, (socket[0], y + 18), (x + 6, y + 18)], (30, 30, 34), 4, 8)
    layer.rect((x, y, x + 230, y + 36), 6, (40, 42, 48), (10, 10, 12), 2)
    layer.rect((x + 12, y + 8, x + 218, y + 28), 3, (206, 160, 60))
    layer.text((x + 115, y + 19), "2.4 GHz antenna", tiny_font, (40, 30, 10), "mm")


# ----------------------------------------------------------- the picture --
def main():
    # Three wires leave each side of the board, so that is the room left for them.
    gap = 3 * LANE + 30
    bx = int(40 + tags_extent(LEFT_PADS) + gap)
    br = bx + BOARD_W
    pads = board_pads(bx)
    tags_r = br + gap + tags_extent(RIGHT_PADS)
    ch = int(tags_r + 40)                  # first of the four lanes up to the sensor
    sx = ch + 3 * LANE + 80                # sensor
    W = int(sx + 380)

    bottom = BOARD_Y + BOARD_H
    ant_y = bottom + 30
    trunk = ant_y + 36 + 44                # first of the four trunks under the board

    # Battery side: two rails, with the divider, capacitor and cell between them.
    rail_p, rail_m = trunk + 3 * LANE + 130, trunk + 3 * LANE + 370
    mid = (rail_p + rail_m) / 2
    r_x, c_x, cell_x = bx + 60, bx + 250, bx + 480
    back_x = cell_x + 230
    H = int(rail_m + 50 + 140)
    layer = Layer(W, H)

    layer.text((40, 26), "Outdoor sensor: wiring", kit_font)
    layer.text((40, 84), "Zigbee outdoor weather sensor", note_font, MUTED)
    layer.text((bx + BOARD_W / 2, 116), "XIAO ESP32-C6", title_font, TEXT, "ma")
    layer.text((sx, 150), "AHT20+BMP280 sensor", module_font)
    sensor = draw_sensor(layer, sx, 198, ["VDD", "SDA", "GND", "SCL"])
    layer.text((back_x, rail_p - 108), "Battery pads", module_font)
    back = draw_underside(layer, back_x, 0, rail_p, rail_m)

    # Wires, drawn before the parts they end on. Each list of points is one run.
    lane_l = [bx - 20 - k * LANE for k in range(3)]          # beside the board, nearest first
    lane_r = [br + 20 + k * LANE for k in range(2)]
    t = [trunk + k * LANE for k in range(4)]
    runs = [
        # 3.3 V and GND leave on the right, SCL and SDA on the left; all four go under the board.
        (GRAY, [pads["GND"], (lane_r[1], pads["GND"][1]), (lane_r[1], t[0]), (ch, t[0]), (ch, sensor["GND"][1]), sensor["GND"]]),
        (RED, [pads["3V3"], (lane_r[0], pads["3V3"][1]), (lane_r[0], t[1]), (ch + LANE, t[1]), (ch + LANE, sensor["VDD"][1]), sensor["VDD"]]),
        (GREEN, [pads["D5"], (lane_l[0], pads["D5"][1]), (lane_l[0], t[2]), (ch + 2 * LANE, t[2]), (ch + 2 * LANE, sensor["SCL"][1]), sensor["SCL"]]),
        (BLUE, [pads["D4"], (lane_l[1], pads["D4"][1]), (lane_l[1], t[3]), (ch + 3 * LANE, t[3]), (ch + 3 * LANE, sensor["SDA"][1]), sensor["SDA"]]),
        # Battery rails and the divider's middle point up to A0.
        (ORANGE, [(r_x, rail_p + 30), (r_x, rail_p), back["BAT+"]]),
        (ORANGE, [(cell_x, rail_p), (cell_x, rail_p + 40)]),
        (GRAY, [(r_x, rail_m - 30), (r_x, rail_m), back["BAT−"]]),
        (GRAY, [(c_x, rail_m), (c_x, rail_m - 30)]),
        (GRAY, [(cell_x, rail_m), (cell_x, rail_m - 40)]),
        (PURPLE, [pads["D0"], (lane_l[2], pads["D0"][1]), (lane_l[2], mid), (c_x, mid), (c_x, mid + 30)]),
    ]
    for colour, pts in runs:
        layer.wire(pts, colour)

    draw_antenna(layer, bx + 50, ant_y, (bx + 84, bottom - 46))
    draw_board(layer, bx, gap, gap)
    draw_resistor(layer, r_x, rail_p + 20, mid, "R1")
    draw_resistor(layer, r_x, mid, rail_m - 20, "R2")
    draw_capacitor(layer, c_x, mid + 20, rail_m - 20)
    draw_cell(layer, cell_x, rail_p + 30, rail_m - 30)
    layer.text((cell_x + 62, mid - 30), "Li-ion cell", pin_font, TEXT, "lm")
    layer.text((cell_x + 62, mid), "1S, 3.7 V", note_font, MUTED, "lm")
    layer.text((cell_x + 62, mid + 28), "protected", note_font, MUTED, "lm")

    # Solder points and junctions.
    for colour, pts in runs[:4]:
        layer.end(pts[0], colour)
        layer.end(pts[-1], colour)
    layer.end(pads["D0"], PURPLE)
    layer.end(back["BAT+"], ORANGE)
    layer.end(back["BAT−"], GRAY)
    for p, colour in (((r_x, mid), PURPLE), ((c_x, mid), PURPLE), ((cell_x, rail_p), ORANGE),
                      ((c_x, rail_m), GRAY), ((cell_x, rail_m), GRAY)):
        layer.circle(p, 8, colour)

    # Legend, under the sensor.
    legend = [(RED, "3.3V"), (GRAY, "GND / battery −"), (BLUE, "I2C data (SDA)"), (GREEN, "I2C clock (SCL)"),
              (ORANGE, "Battery +"), (PURPLE, "Battery sense (A0)")]
    lx, ly = sx - 30, 450
    layer.rect((lx, ly, W - 40, ly + 78 + len(legend) * 42), 14, PANEL, PANEL_EDGE, 2)
    layer.text((lx + 24, ly + 16), "Wires", module_font)
    for i, (colour, label) in enumerate(legend):
        yy = ly + 78 + i * 42
        layer.line([(lx + 24, yy), (lx + 60, yy)], colour, 9)
        layer.text((lx + 74, yy + 1), label, legend_font, TEXT, "lm")

    layer.text((40, H - 104), "The drawings are simplified and show only the pins you connect.", note_font, MUTED)
    layer.text((40, H - 72), "BAT+ and BAT− are solder pads on the underside of the board. BAT− and GND are the same net.",
               note_font, MUTED)
    layer.save(OUT)
    print(OUT, W, H)


if __name__ == "__main__":
    main()
