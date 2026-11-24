#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
sdk_sysroot=${ELF2_TEST_SYSROOT:-$HOME/.local/opt/elf2-sdk/aarch64-buildroot-linux-gnu_sdk-buildroot/aarch64-buildroot-linux-gnu/sysroot}
cross=${ELF2_TEST_CROSS:-$HOME/.local/opt/elf2-sdk/aarch64-buildroot-linux-gnu_sdk-buildroot/bin/aarch64-buildroot-linux-gnu-}
fixture="$tmp/sysroot"
mkdir -p "$fixture/lib/aarch64-linux-gnu" "$fixture/usr/lib/aarch64-linux-gnu"
ln -s "$sdk_sysroot/lib/ld-linux-aarch64.so.1" "$fixture/lib/ld-linux-aarch64.so.1"
ln -s "$sdk_sysroot/lib/libc.so.6" "$fixture/lib/libc.so.6"
printf 'int fixture(void) { return 7; }\n' >"$tmp/fixture.c"
printf 'extern int fixture(void); int main(void) { return fixture(); }\n' >"$tmp/main.c"
"${cross}gcc" -fPIC -shared -Wl,-soname,libfixture.so.1 -o "$tmp/libfixture.so.1" "$tmp/fixture.c"
"${cross}gcc" -o "$tmp/probe" "$tmp/main.c" "$tmp/libfixture.so.1"
if bash "$project_dir/scripts/check_elf_deps.sh" --sysroot "$fixture" "$tmp/probe" >"$tmp/out" 2>&1; then
    echo 'dependency check unexpectedly passed with missing fixture' >&2
    exit 1
fi
grep -Fq 'missing DT_NEEDED: libfixture.so.1' "$tmp/out"
cp "$tmp/libfixture.so.1" "$fixture/usr/lib/aarch64-linux-gnu/libfixture.so.1"
bash "$project_dir/scripts/check_elf_deps.sh" --sysroot "$fixture" "$tmp/probe"
if bash "$project_dir/scripts/check_elf_deps.sh" --sysroot "$fixture" /bin/true >"$tmp/out" 2>&1; then
    echo 'x86-64 input unexpectedly accepted' >&2
    exit 1
fi
grep -Fq 'wrong ELF machine' "$tmp/out"

echo 'ELF dependency closure checker works'
