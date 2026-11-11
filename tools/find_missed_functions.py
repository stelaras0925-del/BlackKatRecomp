#!/usr/bin/env python3
"""Find guest function starts that the Ghidra export (and so ps2_recomp) missed.

A candidate is any code address with no slot in the current generated function
table (build/generated/register_functions.cpp), i.e. the runtime would report
[guest-branch:missing-target] if it were ever reached by JALR/JR. Sources:
  data   every aligned word in the data sections pointing into .text (vtables,
         callback / function-pointer tables, ctor/dtor lists). .gcc_except_table
         is skipped (C++ landing pads are labels inside functions).
  code   lui/addiu and lui/ori pairs, jal targets, j (tail-call) targets.
  sweep  every uncovered gap in the Ghidra map is walked; each address that
         follows a terminator (jr / j / b + delay slot, optional nop padding)
         is a candidate even with no visible reference.
Jump tables are excluded without relying on the Ghidra map: a run of >= 2
consecutive data words whose targets lie within 0x800 bytes of each other, with a
`jr <reg != ra>` in the 0x200 bytes before the lowest target.

Classification:
  function  follows a terminator (+delay, +nop padding) and has a prologue
            (addiu sp,sp,-N in its first 3 insns) or reaches jr ra within 64
            insns (leaf); if inside a Ghidra range, no branch of that range from
            before the candidate crosses it.  -> CSV row
  label     referenced from code (lui pair), does not follow a terminator, and
            lies inside a newly found function (setjmp/longjmp resume labels)
            -> [general].entry_points hint (resume point of its owner)
  rejected  anything else (coincidental constants, mid-function addresses)
End of a function = the next known start (Ghidra row or accepted candidate), so
no existing row is ever covered by a new authoritative range.
Output: build/ghidra/missed_candidates.csv
"""
import argparse, bisect, csv, os, re, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JR_RA = 0x03E00008

def load_elf(path):
    d = open(path, 'rb').read()
    shoff = struct.unpack_from('<I', d, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
    raw = [struct.unpack_from('<IIIIII', d, shoff + i * shentsize) for i in range(shnum)]
    stro = raw[shstrndx][4]
    secs = []
    for name, typ, flags, addr, off, size in raw:
        n = d[stro + name:d.index(b'\0', stro + name)].decode()
        secs.append(dict(name=n, type=typ, flags=flags, addr=addr, off=off, size=size))
    return d, secs

def is_term(w):
    return (w & 0xFC1FFFFF) == 0x00000008 or (w >> 26) == 2 or (w & 0xFFFF0000) == 0x10000000

def branch_target(pc, w):
    op = w >> 26; imm = w & 0xFFFF; simm = imm - 0x10000 if imm & 0x8000 else imm
    rt = (w >> 16) & 0x1F; fmt = (w >> 21) & 0x1F
    if op in (4, 5, 6, 7, 20, 21, 22, 23) or (op == 1 and rt in (0, 1, 2, 3, 16, 17, 18, 19)) \
       or (op in (0x10, 0x11, 0x12) and fmt == 8):
        return pc + 4 + (simm << 2)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', default=os.path.join(ROOT, 'game/SLUS_203.65'))
    ap.add_argument('--csv', default=os.path.join(ROOT, 'build/ghidra/slus_203.65_ghidra.csv.bak'))
    ap.add_argument('--table', default=os.path.join(ROOT, 'build/generated/register_functions.cpp'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build/ghidra/missed_candidates.csv'))
    a = ap.parse_args()

    data, secs = load_elf(a.elf)
    text = next(s for s in secs if s['name'] == '.text')
    T0, T1 = text['addr'], text['addr'] + text['size']
    def tw(addr):
        return struct.unpack_from('<I', data, text['off'] + addr - T0)[0] if T0 <= addr < T1 else 0

    ranges = []
    with open(a.csv) as f:
        r = csv.reader(f); next(r)
        for row in r:
            if len(row) >= 4:
                ranges.append((int(row[1], 16), int(row[2], 16), row[0]))
    starts = {s for s, e, n in ranges}
    rstarts = sorted(starts)
    merged = []
    for s, e, n in sorted(ranges):
        if merged and s <= merged[-1][1]: merged[-1][1] = max(merged[-1][1], e)
        else: merged.append([s, e])
    mstarts = [m[0] for m in merged]
    def covered(x):
        i = bisect.bisect_right(mstarts, x) - 1
        return i >= 0 and merged[i][0] <= x < merged[i][1]
    def containers(x):
        return [(s, e, n) for s, e, n in ranges if s < x < e]

    reg = set()
    if os.path.exists(a.table):
        reg = {int(m.group(1), 16) for m in re.finditer(r'// 0x([0-9a-f]+)', open(a.table).read())}
    print(f'text 0x{T0:x}-0x{T1:x}; csv rows {len(ranges)}; table slots {len(reg)}')

    refs = {}
    def add(t, kind):
        if T0 <= t < T1 and t % 4 == 0: refs.setdefault(t, []).append(kind)

    # data words
    dw = {}
    for s in secs:
        if s['type'] != 1 or s['flags'] & 4 or s['addr'] == 0 or s['name'] == '.gcc_except_table':
            continue
        for off in range(0, s['size'] - 3, 4):
            v = struct.unpack_from('<I', data, s['off'] + off)[0]
            if T0 <= v < T1 and v % 4 == 0: dw[s['addr'] + off] = v
    locs = sorted(dw); jt = set(); i = 0
    while i < len(locs):
        j = i
        while j + 1 < len(locs) and locs[j + 1] == locs[j] + 4: j += 1
        # split the run into windows of nearby targets
        k = i
        while k <= j:
            m = k
            while m + 1 <= j and abs(dw[locs[m + 1]] - dw[locs[k]]) < 0x800: m += 1
            if m > k:
                lo = min(dw[locs[q]] for q in range(k, m + 1))
                if any((tw(pc) & 0xFC1FFFFF) == 8 and ((tw(pc) >> 21) & 31) != 31 for pc in range(lo - 0x200, lo, 4)):
                    jt.update(dw[locs[q]] for q in range(k, m + 1))
            k = m + 1
        i = j + 1
    for loc, v in dw.items():
        if v not in jt: add(v, 'data@%x' % loc)
    # code refs
    lui = {}
    for pc in range(T0, T1, 4):
        w = tw(pc); op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; imm = w & 0xFFFF
        if op in (2, 3):
            add(((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2), ('jal@%x' if op == 3 else 'j@%x') % pc)
            continue
        if op == 0x0F: lui[rt] = (imm << 16, pc); continue
        if op in (9, 0x0D) and rs in lui and pc - lui[rs][1] <= 64:
            hi = lui[rs][0]
            v = ((hi + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF) if op == 9 else (hi | imm)
            add(v, ('lui/addiu@%x' if op == 9 else 'lui/ori@%x') % pc)
        if w == JR_RA: lui.clear()

    def after_term(t):
        q = 1
        while q < 16 and tw(t - 4 * q) == 0 and not is_term(tw(t - 4 * (q + 1))): q += 1
        # t-4*q is the delay slot (possibly a nop) when t-4*(q+1) is a terminator
        return is_term(tw(t - 4 * (q + 1))) or (q > 1 and is_term(tw(t - 4 * q)))
    # gap sweep
    for g0, g1 in zip([T0] + [m[1] for m in merged], [m[0] for m in merged] + [T1]):
        pc = max(g0, T0)
        while pc < min(g1, T1):
            if tw(pc) != 0 and after_term(pc) and pc not in starts:
                add(pc, 'gap-sweep')
            pc += 4

    def shape(t):
        prologue = False
        for q in range(3):
            w = tw(t + 4 * q)
            if (w & 0xFFFF8000) == 0x27BD8000: prologue = True; break
            if w == JR_RA: break
        leaf = any(tw(t + 4 * k) == JR_RA for k in range(64))
        return 'prologue' if prologue else ('leaf' if leaf else 'none')

    cands = sorted(t for t in refs if t not in reg and t not in starts and t not in jt)
    print(f'referenced/swept code addrs {len(refs)}; jump-table targets excluded {len(jt)}; unregistered candidates {len(cands)}')
    info = {}
    for t in cands:
        sh = shape(t); at = after_term(t); gap = not covered(t)
        crossed = False
        for s, e, n in containers(t):
            if n.startswith('caseD_') or n.startswith('default'): continue
            for pc in range(s, min(e, t), 4):
                bt = branch_target(pc, tw(pc))
                if bt is not None and bt > t: crossed = True; break
            if crossed: break
        if gap and not crossed:
            gi = bisect.bisect_right(mstarts, t) - 1
            gstart = merged[gi][1] if gi >= 0 else T0
            for pc in range(max(gstart, t - 0x2000), t, 4):
                bt = branch_target(pc, tw(pc))
                if bt is not None and t <= bt < t + 0x2000 and bt != t:
                    crossed = True; break
        kinds = refs[t]
        if kinds == ['gap-sweep'] and (tw(t - 8) & 0xFFFF0000) == 0x10000000 and sh != 'prologue':
            at = False  # after an unconditional `b` inside a gap: likely a block, not a function
        only_sweep = kinds == ['gap-sweep']
        nonsweep = [k for k in kinds if k != 'gap-sweep']
        if sh == 'none' and nonsweep and any(tw(t + 4 * k) == JR_RA for k in range(1024)):
            sh = 'long-leaf'  # referenced code with no frame that returns within 4 KB
        prev_jal = (tw(t - 8) >> 26) == 3
        if not at and prev_jal and sh == 'prologue' and (tw(t) & 0xFFFF8000) == 0x27BD8000 \
                and any(k.startswith('data@') for k in nonsweep):
            at = True  # previous function ends in a no-return call (jal + delay slot)
        if at and sh != 'none' and not crossed and not (only_sweep and sh == 'leaf' and tw(t) == 0):
            cls = 'function'
        else:
            cls = 'rejected'
        info[t] = dict(shape=sh, after=at, gap=gap, crossed=crossed, cls=cls, kinds=kinds)
    funcs = sorted(t for t in cands if info[t]['cls'] == 'function')
    # labels inside newly found functions (setjmp-style resume addresses)
    for t in cands:
        if info[t]['cls'] == 'rejected' and info[t]['gap'] and any(k.startswith('lui/') for k in info[t]['kinds']):
            i = bisect.bisect_right(funcs, t) - 1
            if i >= 0 and t - funcs[i] < 0x400:
                info[t]['cls'] = 'label'
    bounds = sorted(set(rstarts) | set(funcs))
    rows = []
    for t in cands:
        c = info[t]
        i = bisect.bisect_right(bounds, t)
        end = bounds[i] if i < len(bounds) else T1
        conf = ''
        if c['cls'] == 'function':
            nk = [k for k in c['kinds'] if k != 'gap-sweep']
            conf = 'high' if (nk and c['shape'] == 'prologue') or len(nk) >= 2 else ('medium' if nk else 'low')
        rows.append([f'0x{t:08X}', f'0x{end:08X}', c['cls'], conf, 'gap' if c['gap'] else 'inside',
                     c['shape'], 'after_term' if c['after'] else '', 'crossed' if c['crossed'] else '',
                     len(c['kinds']), ';'.join(c['kinds'][:6])])
    with open(a.out, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['address', 'end', 'class', 'confidence', 'where', 'shape', 'boundary', 'crossed', 'nrefs', 'refs'])
        w.writerows(rows)
    from collections import Counter
    print('classes', Counter((r[2], r[3]) for r in rows))
    print('written', a.out)

if __name__ == '__main__':
    main()
