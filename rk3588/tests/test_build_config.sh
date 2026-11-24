#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

config=$(make -s -C "$project_dir" print-config)

if grep -Fq '/home/why/' <<<"$config"; then
    echo "build config still contains the former developer's absolute path" >&2
    exit 1
fi

grep -Fq 'ELF2_SDK_ROOT=' <<<"$config"
grep -Fq 'SYSROOT=' <<<"$config"
grep -Fq 'SYSROOT_LIB=' <<<"$config"
grep -Fq 'CC=' <<<"$config"
grep -Fq 'PAHO_MQTT=' <<<"$config"
grep -Fq 'LIBMODBUS=' <<<"$config"
grep -Fq 'RKMPP_ENCODER=' <<<"$config"

split_sysroot=$(mktemp -d)
trap 'rm -rf -- "$split_sysroot"' EXIT
mkdir -p "$split_sysroot/usr/lib/aarch64-linux-gnu" "$split_sysroot/usr/lib"
touch "$split_sysroot/usr/lib/aarch64-linux-gnu/libavcodec.so"
touch "$split_sysroot/usr/lib/librknnrt.so" "$split_sysroot/usr/lib/librga.so"

split_config=$(make -s -C "$project_dir" print-config SYSROOT="$split_sysroot")
grep -Fq "LIB_SEARCH_DIRS=$split_sysroot/usr/lib/aarch64-linux-gnu $split_sysroot/usr/lib" <<<"$split_config"
grep -Fq "RKNN_LIB=$split_sysroot/usr/lib/librknnrt.so" <<<"$split_config"
grep -Fq "RGA_LIB=$split_sysroot/usr/lib/librga.so" <<<"$split_config"

echo "build config is relocatable"
