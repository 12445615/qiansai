#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_dir/scripts/lib/elf2_common.sh"
elf2_sanitize_path
profile=jammy-deploy
jobs=1
while (($#)); do
    case "$1" in
        --profile) (($# >= 2)) || { echo 'error: --profile requires a value' >&2; exit 2; }; profile=$2; shift 2 ;;
        --jobs) (($# >= 2)) || { echo 'error: --jobs requires a value' >&2; exit 2; }; jobs=$2; shift 2 ;;
        -h|--help) echo 'Usage: verify_elf2_sdk.sh [--profile PROFILE] [--jobs N]'; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ "$profile" == jammy-deploy ]] || { echo 'error: verifier currently requires --profile jammy-deploy' >&2; exit 2; }

make_cmd=(make -C "$project_dir" PROFILE="$profile")
"${make_cmd[@]}" check-env
"${make_cmd[@]}" clean
"${make_cmd[@]}" -j"$jobs" all

sysroot=$(make -s -C "$project_dir" PROFILE="$profile" print-config | sed -n 's/^SYSROOT=//p')
cc=$(make -s -C "$project_dir" PROFILE="$profile" print-config | sed -n 's/^CC=//p')
cxx=$(make -s -C "$project_dir" PROFILE="$profile" print-config | sed -n 's/^CXX=//p')
elf2_require_dir "$sysroot"
probe_dir="$project_dir/tests/probes"
probe_build=$(mktemp -d /tmp/elf2-probes.XXXXXX)
elf2_assert_safe_staging_dir "$probe_build"
cleanup() { rm -rf -- "$probe_build"; }
trap cleanup EXIT
common=(--sysroot="$sysroot" -I"$sysroot/usr/include" -I"$sysroot/usr/local/include" \
    -I"$sysroot/usr/local/include/rockchip" -L"$sysroot/usr/local/lib" \
    -L"$sysroot/usr/lib/aarch64-linux-gnu" -Wl,-rpath-link,"$sysroot/usr/local/lib" \
    -Wl,-rpath-link,"$sysroot/usr/lib/aarch64-linux-gnu")
"$cc" "${common[@]}" "$probe_dir/media_probe.c" -lavcodec -o "$probe_build/media_probe"
"$cc" "${common[@]}" "$probe_dir/mqtt_probe.c" -lpaho-mqtt3c -o "$probe_build/mqtt_probe"
"$cc" "${common[@]}" "$probe_dir/modbus_probe.c" -lmodbus -o "$probe_build/modbus_probe"
"$cxx" "${common[@]}" "$probe_dir/rknn_rga_probe.cc" -lrknnrt -lrga -o "$probe_build/rknn_rga_probe"

for probe in "$probe_build"/*; do
    file "$probe" | grep -Fq 'ARM aarch64' || { echo "probe is not AArch64: $probe" >&2; exit 1; }
    "$project_dir/scripts/check_elf_deps.sh" --sysroot "$sysroot" "$probe"
done

direct_needed=(libavformat.so.60 libavcodec.so.60 libpaho-mqtt3c.so.1 libmodbus.so.5 librknnrt.so librga.so.2)
for needed in "${direct_needed[@]}"; do
    readelf -d "$project_dir/main" | grep -Fq "[$needed]" || { echo "main missing direct dependency: $needed" >&2; exit 1; }
done
"$project_dir/scripts/check_elf_deps.sh" --sysroot "$sysroot" "$project_dir/main"
mkdir -p "$sysroot/manifests"
{
    echo "PROFILE=$profile"
    echo "SYSROOT=$sysroot"
    echo "MAIN=$project_dir/main"
    echo 'PROBES=media_probe mqtt_probe modbus_probe rknn_rga_probe'
    echo 'DEPENDENCY_CLOSURE=pass'
} >"$sysroot/manifests/verification.txt"
echo "ELF2 SDK verification passed: $sysroot"
