#!/usr/bin/env python3
"""Generate the Xbox-style button prompt textures for Black Kat (original art, drawn procedurally).

The game draws button prompts from small PSMCT16 (RGBA5551) textures:
  * four 32x32 face-button icons (menus; uploaded to GS at boot),
  * a 64x64 face-button atlas (circle, square / triangle, cross; in-game HUD),
  * a 64x64 shoulder-button atlas (L1, L2 / R1, R2 pills; in-game HUD).
The runtime swaps them at GS upload, matched by a hash of the original upload bytes
(ps2xRuntime/src/lib/gs/gs_texrep.inl, ps2x_texrep::hash64; see PS2X_TEXREP_LOG=1).
No fonts or third-party glyph assets are used: balls, pills and letters are drawn here.

Mapping (by position on the pad): Cross->A, Circle->B, Square->X, Triangle->Y, L1->LB, R1->RB, L2->LT, R2->RT.

Writes:
  assets/button_prompts/xbox/<name>.png      previews (RGBA)
  src/overrides/blackkat_button_prompts.inc  C++ arrays (RGBA5551, little endian) + original hashes
Usage: python3 tools/gen_button_prompts.py [repo_root]
"""
import os, sys
from PIL import Image, ImageDraw, ImageFilter

K = 8  # supersampling factor

GREEN, RED, BLUE, YELLOW = (64, 200, 64), (230, 60, 56), (60, 130, 235), (245, 200, 40)
CYAN = (40, 225, 200)

# --- tiny stroke font (unit box: x 0..1, y 0..1 top->bottom) -------------------------------------
FONT = {
    "A": [("l", (0.0, 1.0), (0.5, 0.0)), ("l", (0.5, 0.0), (1.0, 1.0)), ("l", (0.22, 0.68), (0.78, 0.68))],
    "X": [("l", (0.05, 0.0), (0.95, 1.0)), ("l", (0.95, 0.0), (0.05, 1.0))],
    "Y": [("l", (0.0, 0.0), (0.5, 0.52)), ("l", (1.0, 0.0), (0.5, 0.52)), ("l", (0.5, 0.52), (0.5, 1.0))],
    "L": [("l", (0.1, 0.0), (0.1, 1.0)), ("l", (0.1, 1.0), (0.95, 1.0))],
    "T": [("l", (0.0, 0.0), (1.0, 0.0)), ("l", (0.5, 0.0), (0.5, 1.0))],
    "B": [("l", (0.05, 0.0), (0.05, 1.0)), ("bowl", 0.0, 0.48, 0.80), ("bowl", 0.48, 1.0, 0.95)],
    "R": [("l", (0.05, 0.0), (0.05, 1.0)), ("bowl", 0.0, 0.52, 0.90), ("l", (0.45, 0.52), (0.95, 1.0))],
}


def draw_text(d, text, box, colour, sw):
    """Draw text with the stroke font into box=(x0,y0,x1,y1) (supersampled coords)."""
    x0, y0, x1, y1 = box
    n = len(text)
    gap = (x1 - x0) * 0.16 if n > 1 else 0
    cw = ((x1 - x0) - gap * (n - 1)) / n
    c = tuple(colour) + (255,)

    def line(p, q):
        d.line([p, q], fill=c, width=sw)
        for (x, y) in (p, q):
            d.ellipse([x - sw / 2, y - sw / 2, x + sw / 2, y + sw / 2], fill=c)

    for i, ch in enumerate(text):
        gx0 = x0 + i * (cw + gap)
        P = lambda u, v: (gx0 + u * cw, y0 + v * (y1 - y0))
        for op in FONT[ch]:
            if op[0] == "l":
                line(P(*op[1]), P(*op[2]))
            else:  # bowl: from stem at v0..v1, reaching u=xr
                _, v0, v1, xr = op
                (ax, ay), (bx, by) = P(0.05, v0), P(xr, v1)
                h = by - ay
                line((ax, ay), (bx - h / 2, ay))
                line((ax, by), (bx - h / 2, by))
                d.arc([bx - h, ay, bx, by], start=-90, end=90, fill=c, width=sw)


def finish(img, size):
    """Downsample and quantise alpha to 1 bit (RGBA5551 like the originals)."""
    small = img.resize(size, Image.LANCZOS)
    px = small.load()
    for y in range(size[1]):
        for x in range(size[0]):
            r, g, b, a = px[x, y]
            px[x, y] = (r, g, b, 255) if a >= 128 else (0, 0, 0, 0)
    return small


def outlined(img, lay, grow):
    """Composite a dark outline around `lay`'s strokes, then the strokes, onto img."""
    a = lay.split()[3].filter(ImageFilter.MaxFilter(grow))
    o = Image.new("RGBA", img.size, (8, 8, 10, 255))
    o.putalpha(Image.composite(a, Image.new("L", img.size, 0), img.split()[3]))
    img.alpha_composite(o)
    img.alpha_composite(lay)


def ball_icon(ch, colour, S=32):
    """Dark glossy ball with a coloured letter (our own drawing)."""
    N = S * K
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    px = img.load()
    cx = cy = (N - 1) / 2.0
    r = N * 0.47
    for y in range(N):
        for x in range(N):
            dx, dy = (x - cx) / r, (y - cy) / r
            d2 = dx * dx + dy * dy
            if d2 > 1.0:
                continue
            shade = 0.18 + 0.30 * max(0.0, 1.0 - ((dx + 0.35) ** 2 + (dy + 0.45) ** 2) ** 0.5)
            v = int(255 * max(0.03, shade * (1.0 - 0.6 * d2 ** 4)))
            px[x, y] = (v, v, min(255, int(v * 1.05)), 255)
    d = ImageDraw.Draw(img)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(90, 90, 96, 255), width=int(K * 0.9))
    lay = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    m = 0.29 if ch != "B" else 0.33
    draw_text(ImageDraw.Draw(lay), ch, (N * m, N * 0.25, N * (1 - m), N * 0.75), colour, int(N * 0.115))
    outlined(img, lay, K * 2 + 1)
    return finish(img, (S, S))


def pill_icon(text, S=32):
    """Grey rounded bar (30x14 inside a 32x32 cell, like the originals) with cyan text."""
    N = S * K
    img = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    x0, y0, x1, y1 = 1 * K, 9 * K, 31 * K, 23 * K
    body = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    bp = body.load()
    for y in range(y0, y1):
        for x in range(x0, x1):
            u = (y - y0) / (y1 - y0)            # vertical cylinder shading, light band near the top
            v = 0.30 + 0.45 * max(0.0, 1.0 - abs(u - 0.38) * 2.2)
            g = int(255 * v)
            bp[x, y] = (g, g, min(255, g + 6), 255)
    mask = Image.new("L", (N, N), 0)
    ImageDraw.Draw(mask).rounded_rectangle([x0, y0, x1 - 1, y1 - 1], radius=3 * K, fill=255)
    img.paste(body, (0, 0), mask)
    ImageDraw.Draw(img).rounded_rectangle([x0, y0, x1 - 1, y1 - 1], radius=3 * K, outline=(20, 20, 24, 255), width=int(K * 1.2))
    lay = Image.new("RGBA", (N, N), (0, 0, 0, 0))
    draw_text(ImageDraw.Draw(lay), text, (8.5 * K, 12 * K, 23.5 * K, 20 * K), CYAN, int(K * 1.9))
    outlined(img, lay, K + 1)
    return finish(img, (S, S))


def atlas(cells):
    """2x2 atlas of 32x32 cells: [top-left, top-right, bottom-left, bottom-right]."""
    a = Image.new("RGBA", (64, 64), (0, 0, 0, 0))
    for i, c in enumerate(cells):
        a.paste(c, ((i % 2) * 32, (i // 2) * 32))
    return a


def to5551(img):
    px = img.load()
    w, h = img.size
    out = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            out.append(0 if a == 0 else (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000)
    return out


def build():
    A, B, X, Y = (ball_icon(c, col) for c, col in (("A", GREEN), ("B", RED), ("X", BLUE), ("Y", YELLOW)))
    # (original hash, name, image)
    return [
        (0x29743f21e4ca54f2, "cross_to_a", A),          # menu icons, 32x32
        (0x621360d53e29be91, "circle_to_b", B),
        (0x947a456c55c8f526, "square_to_x", X),
        (0xf362c9f3d68262ea, "triangle_to_y", Y),
        (0xc405d90315e2c44b, "hud_face_atlas", atlas([B, X, Y, A])),   # circle, square / triangle, cross
        (0x99cd5ea9981aaec6, "hud_shoulder_atlas",                       # L1, L2 / R1, R2
         atlas([pill_icon("LB"), pill_icon("LT"), pill_icon("RB"), pill_icon("RT")])),
    ]


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    adir = os.path.join(root, "assets", "button_prompts", "xbox")
    os.makedirs(adir, exist_ok=True)
    lines = ["// Generated by tools/gen_button_prompts.py - do not edit.",
             "// Original art (procedural balls/pills + stroke letters) replacing the game's PSMCT16 button textures.",
             "struct BlackKatButtonTex { uint64_t origHash; const char *name; uint16_t w, h; const uint16_t *px; };"]
    entries = []
    for i, (h, name, img) in enumerate(build()):
        img.save(os.path.join(adir, name + ".png"))
        vals = to5551(img)
        lines.append("static const uint16_t kBk_%s[%d] = {%s};" % (name, len(vals), ",".join("0x%04x" % v for v in vals)))
        entries.append("    {0x%016xull, \"%s\", %d, %d, kBk_%s}," % (h, name, img.size[0], img.size[1], name))
    lines.append("static const BlackKatButtonTex kBlackKatXboxPrompts[] = {")
    lines += entries
    lines.append("};")
    inc = os.path.join(root, "src", "overrides", "blackkat_button_prompts.inc")
    os.makedirs(os.path.dirname(inc), exist_ok=True)
    with open(inc, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("wrote", inc, "and", adir)


if __name__ == "__main__":
    main()
