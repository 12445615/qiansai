#!/usr/bin/env bash
set -euo pipefail

sysroot=
declare -a inputs=()
while (($#)); do
    case "$1" in
        --sysroot) (($# >= 2)) || { echo 'error: --sysroot requires a path' >&2; exit 2; }; sysroot=$2; shift 2 ;;
        -h|--help) echo 'Usage: check_elf_deps.sh --sysroot PATH ELF...'; exit 0 ;;
        --) shift; inputs+=("$@"); break ;;
        -*) echo "error: unknown option: $1" >&2; exit 2 ;;
        *) inputs+=("$1"); shift ;;
    esac
done
[[ -n "$sysroot" ]] || { echo 'error: --sysroot is required' >&2; exit 2; }
(( ${#inputs[@]} > 0 )) || { echo 'error: at least one ELF is required' >&2; exit 2; }

declare -a search_dirs=(
    "$sysroot/usr/local/lib"
    "$sysroot/usr/lib/aarch64-linux-gnu"
    "$sysroot/lib/aarch64-linux-gnu"
    "$sysroot/usr/lib"
    "$sysroot/lib"
)
declare -A visited=()
missing=0

resolve_lib() {
    local name=$1 dir
    for dir in "${search_dirs[@]}"; do
        if [[ -e "$dir/$name" ]]; then printf '%s\n' "$dir/$name"; return 0; fi
    done
    return 1
}

check_machine() {
    local file=$1
    if ! readelf -h "$file" 2>/dev/null | grep -Eq 'Machine:[[:space:]]+AArch64'; then
        echo "wrong ELF machine (expected AArch64): $file" >&2
        return 1
    fi
}

required_glibc_version() {
    readelf --version-info "$1" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -n 1 || true
}

walk() {
    local file=$1 dep resolved required provided
    [[ -n "${visited[$file]+x}" ]] && return 0
    visited["$file"]=1
    check_machine "$file" || { missing=1; return 0; }
    required=$(required_glibc_version "$file")
    if [[ -n "$required" ]] && [[ -e "$sysroot/lib/aarch64-linux-gnu/libc.so.6" ]]; then
        provided=$(required_glibc_version "$sysroot/lib/aarch64-linux-gnu/libc.so.6")
        if [[ -n "$provided" ]] && [[ "$(printf '%s\n%s\n' "$provided" "$required" | sort -V | tail -n 1)" != "$provided" ]]; then
            echo "GLIBC version unavailable: $required (sysroot provides $provided)" >&2
            missing=1
        fi
    fi
    while IFS= read -r dep; do
        [[ -n "$dep" ]] || continue
        if ! resolved=$(resolve_lib "$dep"); then
            echo "missing DT_NEEDED: $dep" >&2
            missing=1
            continue
        fi
        walk "$resolved"
    done < <(readelf -d "$file" 2>/dev/null | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
}

for input in "${inputs[@]}"; do
    [[ -f "$input" ]] || { echo "error: ELF input missing: $input" >&2; exit 2; }
    walk "$input"
done
exit "$missing"
