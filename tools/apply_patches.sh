#!/usr/bin/env bash
# Apply BlackKatRecomp's PS2Recomp changes to third_party/PS2Recomp.
#
#   tools/apply_patches.sh            apply the patch series (idempotent)
#   tools/apply_patches.sh --check    dry run: report applied / pending / conflicting, change nothing
#   tools/apply_patches.sh --reset    restore the submodule to the pinned commit (asks first; -y skips)
#   -q / --quiet                      only print problems
#
# The series is patches/ps2recomp-series/series (one file per line, applied in order). Each patch
# owns its files outright, so no two patches touch the same file. A patch that already applies in
# reverse is treated as applied and skipped. Anything else that does not apply stops the run with
# exit code 1 and the git apply error, without touching the remaining patches.
#
# Exit codes: 0 everything applied (or applied now); 1 conflict / error; 2 --check found pending patches.
# Runs under bash, so calling it from fish or zsh is fine: ./tools/apply_patches.sh
set -euo pipefail

mode=apply; yes=0; quiet=0
for a in "$@"; do
    case "$a" in
        --check|-n|--dry-run) mode=check ;;
        --reset) mode=reset ;;
        -y|--yes) yes=1 ;;
        -q|--quiet) quiet=1 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "apply_patches: unknown option '$a' (see --help)" >&2; exit 1 ;;
    esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sub="$root/third_party/PS2Recomp"
series_dir="$root/patches/ps2recomp-series"
series="$series_dir/series"
say() { [ "$quiet" = 1 ] || echo "$@"; }
die() { echo "apply_patches: $*" >&2; exit 1; }

[ -f "$series" ] || die "missing $series"
cd "$root"
pin="$(git ls-tree HEAD third_party/PS2Recomp | awk '{print $3}')"
[ -n "$pin" ] || die "could not read the PS2Recomp pin from the superproject"

if [ ! -e "$sub/.git" ]; then
    [ "$mode" = check ] && { echo "apply_patches: submodule not initialised (run: git submodule update --init third_party/PS2Recomp)" >&2; exit 2; }
    say "Initialising third_party/PS2Recomp at $pin"
    git submodule update --init third_party/PS2Recomp
fi
head="$(git -C "$sub" rev-parse HEAD)"

mapfile -t patches < <(grep -v '^[[:space:]]*\(#\|$\)' "$series")

if [ "$mode" = reset ]; then
    say "This restores third_party/PS2Recomp to $pin:"
    say "  - tracked files lose every local change (git checkout -f)"
    say "  - files the series creates are deleted"
    say "  - other untracked files (e.g. *.bak backups) are left alone"
    [ "$quiet" = 1 ] || git -C "$sub" status --short | grep -v '\.bak' | sed 's/^/    /' || true
    if [ "$yes" != 1 ]; then
        read -r -p "Proceed? [y/N] " ans
        case "$ans" in y|Y|yes|YES) ;; *) echo "Aborted."; exit 1 ;; esac
    fi
    for p in "${patches[@]}"; do
        # files created by the patch: "--- /dev/null" followed by "+++ b/<path>"
        awk '/^--- \/dev\/null/{n=1; next} n && /^\+\+\+ b\//{sub(/^\+\+\+ b\//,""); print; n=0; next} {n=0}' "$series_dir/$p" |
            while read -r f; do [ -f "$sub/$f" ] && rm -f -- "$sub/$f" && say "  removed $f"; done
    done
    git -C "$sub" checkout -q -f "$pin"
    say "third_party/PS2Recomp is at $pin with no local changes to tracked files."
    exit 0
fi

if [ "$head" != "$pin" ]; then
    if [ -z "$(git -C "$sub" status --porcelain --untracked-files=no)" ] && [ "$mode" = apply ]; then
        say "Submodule is at $head, pinned $pin: checking out the pin"
        git submodule update third_party/PS2Recomp
    else
        die "submodule HEAD $head is not the pinned $pin (and has local changes). Use --reset or check out the pin."
    fi
fi

applied=0; pending=0; done_now=0
for p in "${patches[@]}"; do
    f="$series_dir/$p"
    [ -f "$f" ] || die "series lists $p but $f does not exist"
    if git -C "$sub" apply --check --whitespace=nowarn "$f" 2>/dev/null; then
        if [ "$mode" = check ]; then
            say "pending   $p"; pending=$((pending+1))
        else
            git -C "$sub" apply --whitespace=nowarn "$f"
            say "applied   $p"; done_now=$((done_now+1))
        fi
    elif git -C "$sub" apply --check -R --whitespace=nowarn "$f" 2>/dev/null; then
        say "ok        $p (already applied)"; applied=$((applied+1))
    else
        echo "CONFLICT  $p: it neither applies nor is already applied. git apply says:" >&2
        git -C "$sub" apply --check --whitespace=nowarn "$f" 2>&1 | sed 's/^/    /' >&2 || true
        echo "Fix the submodule (e.g. tools/apply_patches.sh --reset, then run again) or refresh the patch." >&2
        exit 1
    fi
done

if [ "$mode" = check ]; then
    say "check: $applied applied, $pending pending, 0 conflicts"
    [ "$pending" = 0 ] || exit 2
else
    say "done: $done_now applied now, $applied already applied"
fi
