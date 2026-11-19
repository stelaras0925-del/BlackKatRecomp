#!/usr/bin/env bash
# Extract the NTSC-U boot ELF and IOP modules from a disc image you own.
# VIV archives stay on the disc. Nothing here is fetched from the network.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${ROOT}/game"
EXPECTED_MD5="e6ea18b09f214dd276078872926ca10d"
EXPECTED_SIZE=6439444

usage() {
    cat <<EOF
Usage: tools/extract_iso.sh <disc-image>

Checks SLUS_203.65 (md5 ${EXPECTED_MD5}, ${EXPECTED_SIZE} bytes) and copies
SYSTEM.CNF, that ELF, and every .IRX / .IMG into ${DEST}/, keeping paths
relative to the disc root. .VIV archives (FILES.VIV, NTSCVID.VIV, VID2.VIV,
VID3.VIV, and any other .VIV) are not extracted.

Requires 7z (p7zip or 7-zip) or, if 7z is not installed, xorriso.
EOF
}

die() {
    echo "extract_iso: $*" >&2
    exit 1
}

if [[ $# -ne 1 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    if [[ $# -ne 1 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
        [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && exit 0
    fi
    exit 2
fi

ISO="$1"
[[ -f "$ISO" ]] || die "not a file: $ISO"
ISO="$(realpath "$ISO")"

if command -v 7z >/dev/null 2>&1; then
    TOOL="7z"
elif command -v 7zz >/dev/null 2>&1; then
    TOOL="7zz"
elif command -v xorriso >/dev/null 2>&1; then
    TOOL="xorriso"
else
    die "need 7z (p7zip / 7-zip) or xorriso"
fi

WORKDIR="$(mktemp -d)"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

LIST="${WORKDIR}/files.txt"

if [[ "$TOOL" == "xorriso" ]]; then
    xorriso -indev "$ISO" -find / -type f 2>/dev/null \
        | sed -e "s/^'//" -e "s/'$//" -e 's#^/##' \
        >"$LIST"
else
    # 7z l -slt prints the archive itself first (no Folder field), then each
    # member. Keep files only (Folder = -).
    "$TOOL" l -slt "$ISO" \
        | awk '
            /^Path = / {
                if (path != "" && folder == "-") print path
                path = substr($0, 8)
                folder = ""
                next
            }
            /^Folder = / { folder = substr($0, 10) }
            END { if (path != "" && folder == "-") print path }
        ' \
        >"$LIST"
fi

[[ -s "$LIST" ]] || die "no files listed in $ISO (tool: $TOOL)"

normalize() {
    local path="$1"
    path="${path#/}"
    path="${path%;1}"
    printf '%s' "$path"
}

keep_file() {
    local base
    base="$(basename "$1")"
    base="${base%;1}"
    local lower="${base,,}"
    case "$lower" in
        *.viv) return 1 ;;
        system.cnf | slus_203.65) return 0 ;;
        *.irx | *.img) return 0 ;;
    esac
    return 1
}

# Lines are "<iso path><TAB><path under game/>". The iso path keeps a ";1"
# version suffix when the listing tool still has it; the destination drops it.
: >"${WORKDIR}/selected.txt"
while IFS= read -r raw || [[ -n "$raw" ]]; do
    [[ -z "$raw" ]] && continue
    rel="$(normalize "$raw")"
    if keep_file "$rel"; then
        printf '%s\t%s\n' "$raw" "$rel" >>"${WORKDIR}/selected.txt"
    fi
done <"$LIST"

[[ -s "${WORKDIR}/selected.txt" ]] || die "no SYSTEM.CNF, SLUS_203.65, IRX, or IMG files in $ISO"

STAGE="${WORKDIR}/stage"
mkdir -p "$STAGE"

extract_one() {
    local raw="$1"
    local rel="$2"
    local out="${STAGE}/${rel}"
    mkdir -p "$(dirname "$out")"
    if [[ "$TOOL" == "xorriso" ]]; then
        xorriso -osirrox on -indev "$ISO" -extract "/${raw#/}" "$out" >/dev/null
    else
        local tmp
        tmp="$(mktemp -d)"
        # 7z uses exit status 1 for warnings such as extra data after the image.
        set +e
        "$TOOL" x -y "-o${tmp}" "$ISO" "$raw" >/dev/null
        local status=$?
        set -e
        if [[ $status -gt 1 ]]; then
            rm -rf "$tmp"
            die "7z failed (exit $status) extracting $raw"
        fi
        if [[ ! -f "${tmp}/${raw}" ]]; then
            rm -rf "$tmp"
            die "7z did not write ${raw}"
        fi
        mv "${tmp}/${raw}" "$out"
        rm -rf "$tmp"
    fi
}

while IFS=$'\t' read -r raw rel; do
    extract_one "$raw" "$rel"
done <"${WORKDIR}/selected.txt"

mapfile -t ELVES < <(find "$STAGE" -type f -iname 'SLUS_203.65' ! -iname '*.viv')
[[ ${#ELVES[@]} -gt 0 ]] || die "SLUS_203.65 was selected but is not in the extract tree"
if [[ ${#ELVES[@]} -ne 1 ]]; then
    printf 'extract_iso: more than one SLUS_203.65:\n' >&2
    printf '  %s\n' "${ELVES[@]}" >&2
    exit 1
fi

ELF="${ELVES[0]}"
ACTUAL_SIZE="$(stat -c%s "$ELF")"
ACTUAL_MD5="$(md5sum "$ELF" | awk '{print $1}')"
if [[ "$ACTUAL_SIZE" != "$EXPECTED_SIZE" || "$ACTUAL_MD5" != "$EXPECTED_MD5" ]]; then
    die "SLUS_203.65 is not NTSC-U v1.00 (size ${ACTUAL_SIZE}, md5 ${ACTUAL_MD5}; expected size ${EXPECTED_SIZE}, md5 ${EXPECTED_MD5}). Nothing was written to ${DEST}."
fi

mkdir -p "$DEST"
while IFS=$'\t' read -r raw rel; do
    mkdir -p "${DEST}/$(dirname "$rel")"
    cp -a "${STAGE}/${rel}" "${DEST}/${rel}"
    printf '%s\n' "$rel"
done <"${WORKDIR}/selected.txt" >"${WORKDIR}/names.txt"

echo "Verified SLUS_203.65 md5 ${ACTUAL_MD5} (${ACTUAL_SIZE} bytes)."
echo "Extracted into ${DEST} (VIV archives skipped):"
sed 's/^/  /' "${WORKDIR}/names.txt"

CNF="$(find "$DEST" -type f -iname 'SYSTEM.CNF' | head -n 1 || true)"
if [[ -n "$CNF" ]]; then
    echo "SYSTEM.CNF (${CNF}):"
    sed 's/^/  /' "$CNF"
    if ! grep -q 'SLUS_203.65' "$CNF"; then
        echo "extract_iso: warning: SYSTEM.CNF does not mention SLUS_203.65" >&2
    fi
    if ! grep -qi 'VMODE.*NTSC' "$CNF"; then
        echo "extract_iso: warning: SYSTEM.CNF does not say VMODE NTSC" >&2
    fi
fi
