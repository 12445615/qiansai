#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
probe_source="$project_dir/tests/probes/recovery_cross_compile_probe.cc"
production_sources=(
    "$project_dir/recovery/src/canonical.cc"
    "$project_dir/recovery/src/crypto_openssl.cc"
    "$project_dir/recovery/src/compiler.cc"
    "$project_dir/recovery/src/proof.cc"
    "$project_dir/recovery/src/runtime.cc"
)
host_dir="$project_dir/build/host/recovery/probes"
target_dir="$project_dir/build/target/recovery/probes"
host_cxx=${HOST_CXX:-g++}
cross_cxx=${RECOVERY_CROSS_CXX:-${CROSS_COMPILE:-}g++}
sysroot=${RECOVERY_SYSROOT:-${SYSROOT:-}}

if [[ -z "$sysroot" ]]; then
    echo "error: RECOVERY_SYSROOT or SYSROOT is required" >&2
    exit 1
fi

mkdir -p "$host_dir" "$target_dir"

common_flags=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -I"$project_dir/recovery/include")
host_command=("$host_cxx" "${common_flags[@]}" "${production_sources[@]}" "$probe_source" -lcrypto -o "$host_dir/recovery_cross_compile_probe")
target_flags=("${common_flags[@]}")
if [[ -d "$sysroot/usr/include/aarch64-linux-gnu" ]]; then
    target_flags+=("-I$sysroot/usr/include/aarch64-linux-gnu")
fi
target_link_flags=()
if [[ -d "$sysroot/usr/lib/aarch64-linux-gnu" ]]; then
    target_link_flags+=("-L$sysroot/usr/lib/aarch64-linux-gnu" "-Wl,-rpath-link,$sysroot/usr/lib/aarch64-linux-gnu")
fi
target_command=("$cross_cxx" "${target_flags[@]}" --sysroot="$sysroot" "${production_sources[@]}" "$probe_source" "${target_link_flags[@]}" -lcrypto -o "$target_dir/recovery_cross_compile_probe")

printf 'HOST_COMPILER=%s\n' "$host_cxx"
printf 'HOST_LINK_COMMAND='; printf '%q ' "${host_command[@]}"; printf '\n'
"${host_command[@]}"
"$host_dir/recovery_cross_compile_probe"
echo 'HOST_RAW_RESULT=compile/link/run PASS'

printf 'TARGET_COMPILER=%s\n' "$cross_cxx"
printf 'TARGET_SYSROOT=%s\n' "$sysroot"
printf 'TARGET_LIBRARY=%s\n' "$sysroot/usr/lib/aarch64-linux-gnu/libcrypto.so"
printf 'TARGET_LINK_COMMAND='; printf '%q ' "${target_command[@]}"; printf '\n'
"${target_command[@]}"
file "$target_dir/recovery_cross_compile_probe"
echo 'TARGET_RAW_RESULT=compile/link PASS; runtime NOT_RUN'
