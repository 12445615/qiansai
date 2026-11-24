#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

fixture_tree="$tmp/root-tree"
mkdir -p "$fixture_tree/etc" "$fixture_tree/lib/aarch64-linux-gnu" \
    "$fixture_tree/usr/lib/aarch64-linux-gnu" "$fixture_tree/usr/local/lib" \
    "$fixture_tree/usr/local/include" "$fixture_tree/var/lib/dpkg"
printf 'ID=ubuntu\nVERSION_ID="22.04"\n' >"$fixture_tree/etc/os-release"
printf 'rootfs libc\n' >"$fixture_tree/lib/aarch64-linux-gnu/libc.so.6"
printf 'rootfs rknn\n' >"$fixture_tree/usr/lib/aarch64-linux-gnu/librknnrt.so"
printf 'Package: fixture\n' >"$fixture_tree/var/lib/dpkg/status"
rootfs_fixture="$tmp/rootfs.img"
truncate -s 64M "$rootfs_fixture"
mkfs.ext4 -F -q -d "$fixture_tree" "$rootfs_fixture"

printf 'fake update image\n' >"$tmp/update.img"
printf '%s  update.img\n' "$(md5sum "$tmp/update.img" | awk '{print $1}')" >"$tmp/MD5.txt"
mkdir -p "$tmp/tools" "$tmp/toolchain/bin" "$tmp/toolchain/aarch64-none-linux-gnu/libc/usr/include" \
    "$tmp/toolchain/aarch64-none-linux-gnu/libc/usr/lib"
cat >"$tmp/tools/rkImageMaker" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cp "$FIXTURE_EXT4" "$3/firmware.img"
EOF
cat >"$tmp/tools/afptool" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
mkdir -p "$3"
cp "$FIXTURE_EXT4" "$3/rootfs.img"
EOF
chmod +x "$tmp/tools/rkImageMaker" "$tmp/tools/afptool"
cat >"$tmp/toolchain/bin/aarch64-none-linux-gnu-gcc" <<'EOF'
#!/usr/bin/env bash
echo 'aarch64-none-linux-gnu-gcc (GCC) 10.3.1 fixture'
EOF
chmod +x "$tmp/toolchain/bin/aarch64-none-linux-gnu-gcc"
printf 'fixture stdio\n' >"$tmp/toolchain/aarch64-none-linux-gnu/libc/usr/include/stdio.h"
printf 'crt1\n' >"$tmp/toolchain/aarch64-none-linux-gnu/libc/usr/lib/crt1.o"

FIXTURE_EXT4="$rootfs_fixture" bash "$project_dir/scripts/setup_elf2_jammy_sdk.sh" \
    --image "$tmp/update.img" \
    --md5-file "$tmp/MD5.txt" \
    --tools-dir "$tmp/tools" \
    --toolchain-source "$tmp/toolchain" \
    --toolchain-root "$tmp/installed-toolchain" \
    --install-root "$tmp/install"

test -f "$tmp/install/sysroot/usr/include/stdio.h"
test -f "$tmp/install/sysroot/usr/lib/crt1.o"
test -f "$tmp/install/sysroot/lib/aarch64-linux-gnu/libc.so.6"
test -f "$tmp/install/sysroot/usr/lib/aarch64-linux-gnu/librknnrt.so"
test -f "$tmp/install/manifests/base-inputs.sha256"
test -f "$tmp/install/manifests/base-environment.txt"

echo 'Jammy base sysroot assembly is reproducible'
