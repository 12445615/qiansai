#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
setup_script="$project_dir/scripts/setup_elf2_sdk.sh"

check_output=$(bash "$setup_script" --check)
grep -Fq 'ELF2 SDK archive OK' <<<"$check_output"
grep -Fq 'ELF2 SDK installation OK' <<<"$check_output"

missing_log=$(mktemp)
if bash "$setup_script" --check --archive /definitely/missing/elf2-sdk.tar.gz >"$missing_log" 2>&1; then
    echo "setup script unexpectedly accepted a missing archive" >&2
    rm -f "$missing_log"
    exit 1
fi
grep -Fq 'ELF2 SDK archive not found' "$missing_log"
rm -f "$missing_log"

echo "ELF2 SDK setup checks behave as expected"
