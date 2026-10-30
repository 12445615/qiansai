#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sdk_home=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf -- "$sdk_home"; rm -f -- "$log"' EXIT

default_config=$(make -s -C "$project_dir" print-config)
grep -Fxq 'PROFILE=buildroot-check' <<<"$default_config"
grep -Fxq 'PROFILE_STRICT=0' <<<"$default_config"

mkdir -p "$sdk_home/toolchains/gcc-arm-10.3-aarch64/bin"
mkdir -p "$sdk_home/profiles/jammy-deploy/sysroot"
jammy_config=$(make -s -C "$project_dir" print-config \
    PROFILE=jammy-deploy ELF2_SDK_HOME="$sdk_home")
grep -Fxq 'PROFILE=jammy-deploy' <<<"$jammy_config"
grep -Fxq 'PROFILE_STRICT=1' <<<"$jammy_config"
grep -Fq "$sdk_home/toolchains/gcc-arm-10.3-aarch64/bin/aarch64-none-linux-gnu-gcc" <<<"$jammy_config"
grep -Fq "SYSROOT=$sdk_home/profiles/jammy-deploy/sysroot" <<<"$jammy_config"

if make -s -C "$project_dir" print-config PROFILE=unknown >"$log" 2>&1; then
    echo 'unknown profile unexpectedly succeeded' >&2
    exit 1
fi
grep -Fq 'unknown ELF2 profile: unknown' "$log"

echo 'profile selection is explicit'
