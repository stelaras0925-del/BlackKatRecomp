#!/usr/bin/env python3
"""Apply config/sdk_names.csv to the Ghidra export and the ps2_recomp TOML.

How names reach ps2_recomp (read from the pinned PS2Recomp source):
  * ps2xRecomp/src/lib/ps2_recompiler.cpp parses every [general].stubs entry
    "Name@0xADDR" with parseFunctionSelector(). The address goes into
    m_stubFunctionStarts and the name into m_stubHandlerBindingsByStart, so the
    function starting at ADDR is emitted as a call to ps2_stubs::/ps2_syscalls::
    Name, provided hasResolvedStubHandler() finds Name in
    ps2xRuntime/include/ps2_call_list.h (exact, or with one leading '_' added
    or removed). Otherwise it warns and recompiles the guest code.
  * isStubFunction() ALSO stubs any function whose Ghidra CSV name resolves to
    a runtime handler, even with no TOML entry. So a CSV rename to a handler
    name is itself a binding. This script therefore never writes a
    handler-resolving name into the CSV unless that row is meant to be stubbed.

Actions in config/sdk_names.csv:
  stub      rename in the CSV and add Name@ADDR to [general].stubs
  rename    rename in the CSV only (refused if the name resolves to a handler)
  optional  like stub, but only with --with-optional (libc helpers that work
            fine as recompiled guest code)
  record    documentation only, nothing is written

Missed functions (config/missed_functions.csv, from tools/find_missed_functions.py):
  kind=function  appended to the Ghidra CSV as "name,start,end,size". The name
                 must not be auto-generated (FUN_/sub_/LAB_/DAT_): ps2xRecomp's
                 ElfParser::extractFunctions treats named rows with bounds as
                 authoritative, and drops auto-named rows that start inside an
                 authoritative range. A new start inside an existing range needs
                 no trimming of the containing row: overlapping ranges are
                 decoded separately and the function table registers each
                 function's own start first (FunctionTableEmitter), so the new
                 function owns its start slot. End is capped at the next
                 existing start so no existing row is ever covered.
  kind=label     added to [general].entry_points (a resume point inside its
                 owner's switch, e.g. a setjmp/longjmp landing address).
  --missed-min-confidence low|medium|high (default low = all), --no-missed.

The script always starts from the *.bak originals (made on the first run), so
it is safe to re-run after editing the mapping.
"""
import argparse
import csv
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MARK_BEGIN = "  # --- begin sdk_names.csv (tools/apply_sdk_names.py) ---"
MARK_END = "  # --- end sdk_names.csv ---"
EP_BEGIN = "  # --- begin missed_functions.csv labels (tools/apply_sdk_names.py) ---"
EP_END = "  # --- end missed_functions.csv labels ---"
AUTO_PREFIXES = ("FUN_", "sub_", "LAB_", "DAT_")


def load_handlers(call_list: Path):
    text = call_list.read_text()
    return set(re.findall(r"X\((\w+)\)", text))


def resolve(name, handlers):
    """Mirror ps2_runtime_calls::resolveNameWithOptionalLeadingUnderscoreAlias."""
    if name in handlers:
        return name
    if name.startswith("_") and name[1:] in handlers:
        return name[1:]
    if ("_" + name) in handlers:
        return "_" + name
    return None


def backup(path: Path) -> Path:
    bak = path.with_name(path.name + ".bak")
    if not bak.exists():
        shutil.copy2(path, bak)
        print(f"backup: {bak}")
    return bak


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mapping", default=ROOT / "config/sdk_names.csv", type=Path)
    ap.add_argument("--ghidra-csv", default=ROOT / "build/ghidra/slus_203.65_ghidra.csv", type=Path)
    ap.add_argument("--toml", default=ROOT / "build/ghidra/slus_203.65_recomp.toml", type=Path,
                    help="the TOML you pass to ps2_recomp")
    ap.add_argument("--call-list", default=ROOT / "third_party/PS2Recomp/ps2xRuntime/include/ps2_call_list.h", type=Path)
    ap.add_argument("--with-optional", action="store_true", help="also bind rows marked 'optional'")
    ap.add_argument("--min-confidence", choices=["high", "medium"], default="high",
                    help="lowest confidence to apply (default: high)")
    ap.add_argument("--missed", default=ROOT / "config/missed_functions.csv", type=Path)
    ap.add_argument("--no-missed", action="store_true", help="do not add config/missed_functions.csv rows")
    ap.add_argument("--missed-min-confidence", choices=["low", "medium", "high"], default="low")
    ap.add_argument("--elf", default=ROOT / "game/SLUS_203.65", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    handlers = load_handlers(args.call_list)
    allowed_conf = {"high"} if args.min_confidence == "high" else {"high", "medium"}

    csv_bak = backup(args.ghidra_csv)
    toml_bak = backup(args.toml)

    # Ghidra export: Name,Start,End,Size
    with open(csv_bak, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        rows = list(reader)
    by_start = {}
    for i, r in enumerate(rows):
        by_start.setdefault(int(r[1], 16), []).append(i)

    renames, stubs, warnings = {}, [], []
    counts = {"stub": 0, "rename": 0, "optional": 0, "record": 0, "skipped": 0}
    with open(args.mapping, newline="") as f:
        for m in csv.DictReader(f):
            addr = int(m["address"], 16)
            name, conf, action = m["name"].strip(), m["confidence"].strip(), m["action"].strip()
            if action == "record":
                counts["record"] += 1
                continue
            if conf not in allowed_conf:
                counts["skipped"] += 1
                continue
            if action == "optional":
                if not args.with_optional:
                    counts["skipped"] += 1
                    continue
                action = "stub"
                counts["optional"] += 1
            idxs = by_start.get(addr, [])
            func_idx = [i for i in idxs if not rows[i][0].startswith(("caseD_", "default"))]
            if not func_idx:
                warnings.append(f"{m['address']} {name}: no function starts there in the Ghidra CSV, skipped")
                counts["skipped"] += 1
                continue
            handler = resolve(name, handlers)
            if action == "stub":
                if handler is None:
                    warnings.append(f"{m['address']} {name}: no runtime handler, renamed only")
                    action = "rename"
                else:
                    stubs.append(f"{handler}@0x{addr:08X}")
                    counts["stub"] += 1
            elif action == "rename":
                if handler is not None:
                    warnings.append(f"{m['address']} {name}: resolves to runtime handler '{handler}', "
                                    "CSV rename refused (it would silently stub it)")
                    counts["skipped"] += 1
                    continue
                counts["rename"] += 1
            for i in func_idx:
                old = rows[i][0]
                if not old.startswith(("FUN_", "entry_")) and old != name:
                    warnings.append(f"{m['address']}: Ghidra already calls it '{old}', keeping that and not renaming to {name}")
                    continue
                renames[i] = name

    # Duplicate names in the CSV would generate duplicate C++ symbols? No: ps2_recomp
    # suffixes _0xADDR, but keep names unique anyway for readability.
    seen = {}
    for i, n in renames.items():
        seen.setdefault(n, []).append(rows[i][1])
    for n, addrs in seen.items():
        if len(addrs) > 1:
            warnings.append(f"name {n} used for several starts: {addrs}")

    for i, n in renames.items():
        rows[i][0] = n

    # Missed functions -> extra CSV rows / entry_points hints.
    added_rows, labels = [], []
    if not args.no_missed and args.missed.exists():
        import struct
        elf = args.elf.read_bytes()
        shoff = struct.unpack_from("<I", elf, 0x20)[0]
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)
        text_lo = text_hi = None
        for i in range(shnum):
            _, _, flags, addr, _, size = struct.unpack_from("<IIIIII", elf, shoff + i * shentsize)
            if flags & 4 and addr == 0x100000:
                text_lo, text_hi = addr, addr + size
        rank = {"low": 0, "medium": 1, "high": 2, "n/a": 2}
        need = rank[args.missed_min_confidence]
        existing_starts = sorted(int(r[1], 16) for r in rows)
        existing_set = set(existing_starts)
        names_in_csv = {r[0] for r in rows}
        import bisect
        skipped_m = 0
        with open(args.missed, newline="") as f:
            for m in csv.DictReader(f):
                addr = int(m["address"], 16)
                kind = m["kind"].strip()
                if rank.get(m["confidence"].strip(), 0) < need:
                    skipped_m += 1
                    continue
                if text_lo is None or not (text_lo <= addr < text_hi) or addr % 4:
                    warnings.append(f"{m['address']}: missed entry outside .text or unaligned, skipped")
                    continue
                if kind == "label":
                    labels.append(f"0x{addr:08X}")
                    continue
                name = m["name"].strip()
                end = int(m["end"], 16)
                if addr in existing_set:
                    warnings.append(f"{m['address']} {name}: already a start in the Ghidra CSV, skipped")
                    continue
                j = bisect.bisect_right(existing_starts, addr)
                nxt = existing_starts[j] if j < len(existing_starts) else text_hi
                if end > nxt:
                    warnings.append(f"{m['address']} {name}: end 0x{end:08X} capped to next start 0x{nxt:08X}")
                    end = nxt
                if end <= addr:
                    warnings.append(f"{m['address']} {name}: empty range, skipped")
                    continue
                if not name or name.startswith(AUTO_PREFIXES) or not re.fullmatch(r"[A-Za-z_]\w*", name):
                    warnings.append(f"{m['address']} {name!r}: needs a non-auto C identifier name, skipped")
                    continue
                if resolve(name, handlers) is not None:
                    warnings.append(f"{m['address']} {name}: resolves to a runtime handler (would be stubbed), skipped")
                    continue
                if name in names_in_csv:
                    warnings.append(f"{m['address']} {name}: name already used in the CSV, skipped")
                    continue
                names_in_csv.add(name)
                added_rows.append([name, f"0x{addr:08X}", f"0x{end:08X}", str(end - addr)])
        print(f"missed functions: {len(added_rows)} CSV rows, {len(labels)} entry_points labels"
              f" ({skipped_m} below --missed-min-confidence)")
        # new rows must not start inside one another either
        added_rows.sort(key=lambda r: int(r[1], 16))
        for a_, b_ in zip(added_rows, added_rows[1:]):
            if int(a_[2], 16) > int(b_[1], 16):
                warnings.append(f"{a_[1]} {a_[0]}: overlaps next missed row {b_[1]}, end capped")
                a_[2] = b_[1]
                a_[3] = str(int(b_[1], 16) - int(a_[1], 16))
    rows_out = sorted(rows + added_rows, key=lambda r: int(r[1], 16))

    # TOML: rebuild [general].stubs = [...] keeping the original entries.
    toml_text = toml_bak.read_text()
    mt = re.search(r"(?ms)^stubs\s*=\s*\[\n(.*?)^\]", toml_text)
    if not mt:
        sys.exit(f"could not find a multi-line 'stubs = [' array in {toml_bak}")
    body = mt.group(1)
    existing = re.findall(r'"([^"]+)"', body)
    existing_addrs = {e.split("@")[1].upper() for e in existing if "@" in e}
    new = [s for s in stubs if s.split("@")[1].upper() not in existing_addrs]
    dup = [s for s in stubs if s.split("@")[1].upper() in existing_addrs]
    for d in dup:
        warnings.append(f"{d}: address already bound in the TOML, kept the existing entry")
    block = body
    if new:
        block += MARK_BEGIN + "\n" + "".join(f'  "{s}",\n' for s in new) + MARK_END + "\n"
    toml_out = toml_text[:mt.start(1)] + block + toml_text[mt.end(1):]

    # [general].entry_points = [...] (labels from missed_functions.csv)
    me = re.search(r"(?m)^entry_points\s*=\s*\[(.*?)\]", toml_out, re.S)
    if not me:
        sys.exit("could not find 'entry_points = [...]' in the TOML")
    ep_existing = re.findall(r'"([^"]+)"', me.group(1))
    ep_new = [l for l in labels if l not in ep_existing]
    if ep_new:
        inner = "".join(f'  "{e}",\n' for e in ep_existing)
        inner += EP_BEGIN + "\n" + "".join(f'  "{e}",\n' for e in ep_new) + EP_END + "\n"
        toml_out = toml_out[:me.start()] + "entry_points = [\n" + inner + "]" + toml_out[me.end():]

    try:
        import tomllib
        parsed = tomllib.loads(toml_out)
        assert len(parsed["general"]["stubs"]) == len(existing) + len(new)
        assert len(parsed["general"]["entry_points"]) == len(ep_existing) + len(ep_new)
        print(f"TOML parses: {len(parsed['general']['stubs'])} stubs, "
              f"{len(parsed['general']['entry_points'])} entry_points")
    except ImportError:
        pass

    print(f"handlers known: {len(handlers)}")
    print(f"stub bindings added: {len(new)} (plus {len(existing)} already in the TOML)")
    print(f"CSV rows renamed: {len(renames)}")
    print(f"CSV rows: {len(rows)} from .bak + {len(added_rows)} missed = {len(rows_out)}")
    print("counts:", counts)
    for w in warnings:
        print("warning:", w)
    if args.dry_run:
        print("dry run, nothing written")
        return
    with open(args.ghidra_csv, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(header)
        w.writerows(rows_out)
    args.toml.write_text(toml_out)
    print(f"wrote {args.ghidra_csv}")
    print(f"wrote {args.toml}")


if __name__ == "__main__":
    main()
