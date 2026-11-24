#!/usr/bin/env python3
"""Build the vector replacement for Black Kat's front-end menu font (Vulkan renderer, PS2X_VK_FONT).

The game's menu font is a 512x512 PSMT4 atlas (outline = CLUT index 1, fill = index 9) with an "FNTS" glyph table
(version 202, 12-byte entries: code, w, h, u, v, advance, xoff, yoff, flag). Each glyph is drawn as a quad that
samples the atlas at (u+1, v+1)..(u+1+w, v+1+h) 1:1, first as a black shadow at +3,+3, then with a vertical
vertex-colour gradient. The runtime (gs_vk_font.inl) recognises the atlas upload by hash, maps every glyph quad back
to its table entry and redraws it from this pre-rendered atlas, keeping the game's pen positions and advances, its
CLUT colours (outline/fill), the vertex gradient and the shadow pass.

Inputs: a RAM dump of the front end (BLACKKAT_RAMDUMP=<sec> while the main menu is up) and an OFL TrueType font.
Output: <out>.p2f (header + glyph metrics + PackBits-compressed RG8 atlas at K x: R = fill, G = fill+outline)
        and <out>_preview.png (small preview).
Usage: python3 tools/gen_menu_font.py <ramdump.bin[.gz]> <font.ttf> <out_basename> [--k 8] [--outline 1.3]
Only fontTools-free Pillow + numpy are needed.
"""
import sys, struct, gzip, argparse
import numpy as np
from PIL import Image, ImageDraw, ImageFont

FNTS_ADDR, ATLAS_ADDR, ATLAS_W, ATLAS_H = 0x12a9380, 0x12a9800, 512, 512
OUT_IDX, FILL_IDX, CBP_OFF = 1, 9, 0x200       # CLUT indices; CBP = TBP + 0x200 (2bc0 -> 2dc0)

def hash64(p):  # = ps2x_texrep::hash64
    M = (1 << 64) - 1; h = 0xcbf29ce484222325 ^ len(p)
    for i in range(0, len(p) - len(p) % 8, 8):
        h ^= int.from_bytes(p[i:i + 8], 'little'); h = (h * 0x100000001b3) & M; h ^= h >> 29
    for b in p[len(p) - len(p) % 8:]:
        h ^= b; h = (h * 0x100000001b3) & M
    return h

def packbits(data):
    out = bytearray(); i = 0; n = len(data)
    while i < n:
        j = i
        while j + 1 < n and data[j + 1] == data[i] and j - i < 128: j += 1
        if j > i:                                   # run of j-i+1 (2..129)
            out.append(126 + (j - i + 1)); out.append(data[i]); i = j + 1
        else:
            k = i
            while k < n and k - i < 128 and not (k + 2 < n and data[k] == data[k + 1] == data[k + 2]): k += 1
            k = max(k, i + 1)
            out.append(k - i - 1); out += data[i:k]; i = k
    return bytes(out)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ram'); ap.add_argument('ttf'); ap.add_argument('out')
    ap.add_argument('--k', type=int, default=8); ap.add_argument('--outline', type=float, default=1.3)
    ap.add_argument('--width', type=int, default=4096); ap.add_argument('--match', default='mean')
    ap.add_argument('--xstretch', type=float, default=1.1); ap.add_argument('--place', default='adv')
    a = ap.parse_args(); K = a.k
    R = (gzip.open if a.ram.endswith('.gz') else open)(a.ram, 'rb').read()
    assert R[FNTS_ADDR:FNTS_ADDR + 4] == b'FNTS', 'FNTS table not found at %x' % FNTS_ADDR
    ver, cnt = struct.unpack_from('<HH', R, FNTS_ADDR + 8)
    assert ver == 202, ver
    ents = [struct.unpack_from('<HBBHHBbBB', R, FNTS_ADDR + 0x20 + 12 * i) for i in range(cnt)]
    raw = R[ATLAS_ADDR:ATLAS_ADDR + ATLAS_W * ATLAS_H // 2]
    h = hash64(raw)
    A = np.zeros(ATLAS_W * ATLAS_H, np.uint8); r = np.frombuffer(raw, np.uint8)
    A[0::2] = r & 15; A[1::2] = r >> 4; A = A.reshape(ATLAS_H, ATLAS_W)
    G = []
    for (code, w, hh, u, v, adv, xo, yo, fl) in ents:
        if w <= 1 or hh <= 1: continue
        cell = A[v + 1:v + 1 + hh, u + 1:u + 1 + w]
        G.append(dict(code=code, ch=chr(code), w=w, h=hh, u=u + 1, v=v + 1, adv=adv, xo=xo, yo=yo, fill=cell == FILL_IDX))
    def rows(g):
        r = np.nonzero(g['fill'].any(1))[0]; return (g['yo'] + r.min(), g['yo'] + r.max() + 1) if len(r) else None
    lc = [g for g in G if g['ch'] in 'acemnorsuvwz' and rows(g)]
    base = float(np.median([rows(g)[1] for g in lc]))
    xh = float(np.median([rows(g)[1] - rows(g)[0] for g in lc]))
    caps = [g for g in G if g['ch'] in 'EFHIKLMNTZ' and rows(g)]
    cap = float(np.median([rows(g)[1] - rows(g)[0] for g in caps]))
    font0 = ImageFont.truetype(a.ttf, 1000)
    xh_font = -font0.getbbox('xvwz', anchor='ls')[1] / 1000.0
    cap_font = -font0.getbbox('EFHIKLMNTZ', anchor='ls')[1] / 1000.0
    # 1x pixel size: geometric mean of the sizes matching the game's fill x-height and cap height
    sx, sc = xh / xh_font, cap / cap_font
    size = {'x': sx, 'cap': sc}.get(a.match, (sx * sc) ** 0.5)
    print('x-height %.1f cap %.1f -> size x %.2f cap %.2f' % (xh, cap, sx, sc))
    font = ImageFont.truetype(a.ttf, max(8, round(size * K)))
    sw = max(1, round(a.outline * K)); P = 2 * K         # outline stroke, mip-safe padding (8x: 16 px)
    print('baseline %.1f x-height %.1f -> size %.2f px (1x), %d glyphs, atlas hash %016x' % (base, xh, size, len(G), h))
    tiles = []
    for g in G:
        Wc = 6 * font.size; Hc = 4 * font.size
        U = Image.new('L', (Wc, Hc), 0); F = Image.new('L', (Wc, Hc), 0)
        ox, oy = Wc // 3, Hc * 2 // 3
        ImageDraw.Draw(U).text((ox, oy), g['ch'], font=font, fill=255, anchor='ls', stroke_width=sw, stroke_fill=255)
        ImageDraw.Draw(F).text((ox, oy), g['ch'], font=font, fill=255, anchor='ls')
        if a.xstretch != 1.0:   # mild horizontal extension: the game's advances are ~22% wider than IM FELL's
            Ws = round(Wc * a.xstretch); U = U.resize((Ws, Hc), Image.LANCZOS); F = F.resize((Ws, Hc), Image.LANCZOS)
            ox = round(ox * a.xstretch)
        bx = U.getbbox()
        if not bx: continue
        x0, y0, x1, y1 = bx[0] - P, bx[1] - P, bx[2] + P, bx[3] + P
        u_ = np.asarray(U.crop((x0, y0, x1, y1))); f_ = np.asarray(F.crop((x0, y0, x1, y1)))
        # place: ink centre on the original quad centre (keeps the game's spacing), baseline on the game's baseline
        if a.place == 'adv':    # centre the new advance box in the game's advance box (pen-relative), even spacing
            na = font.getlength(g['ch']) * a.xstretch / K
            nx0 = (g['adv'] - na) / 2.0 + (x0 - ox) / K
        else:                   # 'ink': ink centre on the original quad centre
            cx = g['xo'] + g['w'] / 2.0
            inkc = ((bx[0] + bx[2]) / 2.0 - ox) / K
            nx0 = cx - inkc + (x0 - ox) / K
        ny0 = base + (y0 - oy) / K
        tiles.append(dict(g=g, U=u_, F=f_, nx0=nx0, ny0=ny0, nw=(x1 - x0) / K, nh=(y1 - y0) / K))
    # shelf pack
    AW = a.width; x = y = sh = 0
    for t in sorted(tiles, key=lambda t: -t['U'].shape[0]):
        hh, ww = t['U'].shape
        if x + ww > AW: x = 0; y += sh; sh = 0
        t['ax'], t['ay'] = x, y; x += ww; sh = max(sh, hh)
    AH = (y + sh + 3) // 4 * 4
    atl = np.zeros((AH, AW, 2), np.uint8)
    for t in tiles:
        hh, ww = t['U'].shape
        atl[t['ay']:t['ay'] + hh, t['ax']:t['ax'] + ww, 0] = t['F']
        atl[t['ay']:t['ay'] + hh, t['ax']:t['ax'] + ww, 1] = t['U']
    hdr = struct.pack('<8sIIIIQIIIIff', b'P2XFONT1', 1, AW, AH, len(tiles), h, ATLAS_W, ATLAS_H, OUT_IDX | (FILL_IDX << 8), CBP_OFF, float(K), float(base))
    body = b''
    for t in sorted(tiles, key=lambda t: t['g']['code']):
        g = t['g']
        body += struct.pack('<HHHHHhhhffffHHHH', g['code'], g['u'], g['v'], g['w'], g['h'], g['xo'], g['yo'], g['adv'],
                            t['nx0'], t['ny0'], t['nw'], t['nh'], t['ax'], t['ay'], t['U'].shape[1], t['U'].shape[0])
    pix = packbits(atl.tobytes())
    open(a.out + '.p2f', 'wb').write(hdr + body + struct.pack('<I', len(pix)) + pix)
    prev = np.zeros((AH, AW, 3), np.uint8); prev[..., 0] = prev[..., 1] = atl[..., 0]; prev[..., 2] = atl[..., 1]
    Image.fromarray(prev).resize((AW // 4, AH // 4), Image.LANCZOS).save(a.out + '_preview.png', optimize=True)
    print('atlas %dx%d RG8 at %dx, %d glyphs, %d bytes compressed (%.1f MB raw)' % (AW, AH, K, len(tiles), len(pix), AW * AH * 2 / 1e6))

if __name__ == '__main__':
    main()
