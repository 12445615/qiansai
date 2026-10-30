#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_dir/scripts/lib/elf2_common.sh"
elf2_sanitize_path

image='/mnt/f/ELF 2开发板资料包/03-镜像/00-eMMC镜像/Desktop镜像/update.img'
md5_file='/mnt/f/ELF 2开发板资料包/03-镜像/00-eMMC镜像/Desktop镜像/MD5.txt'
tools_dir="$HOME/.local/opt/elf2-sdk/sources/rockdev-tools/rockdev"
toolchain_source='/home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/gcc-arm-10.3-aarch64-toolchain'
toolchain_root="$HOME/.local/opt/elf2-sdk/toolchains/gcc-arm-10.3-aarch64"
install_root="$HOME/.local/opt/elf2-sdk/profiles/jammy-deploy"
check_only=0
toolchain_archive_sha256=8b79c0ba3d0f83d58200c9d0b96f253bbaeab1d03e3851701d082903720c1545

usage() {
    cat <<'EOF'
Usage: setup_elf2_jammy_sdk.sh [options]
  --image PATH
  --md5-file PATH
  --tools-dir PATH
  --toolchain-source PATH
  --toolchain-root PATH
  --install-root PATH
  --check
EOF
}

while (($#)); do
    case "$1" in
        --image|--md5-file|--tools-dir|--toolchain-source|--toolchain-root|--install-root)
            (($# >= 2)) || { echo "error: $1 requires a value" >&2; exit 2; }
            case "$1" in
                --image) image=$2 ;;
                --md5-file) md5_file=$2 ;;
                --tools-dir) tools_dir=$2 ;;
                --toolchain-source) toolchain_source=$2 ;;
                --toolchain-root) toolchain_root=$2 ;;
                --install-root) install_root=$2 ;;
            esac
            shift 2
            ;;
        --check) check_only=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

verify_inputs() {
    elf2_require_file "$image"
    elf2_require_file "$md5_file"
    elf2_require_file "$tools_dir/rkImageMaker"
    elf2_require_file "$tools_dir/afptool"
    test -x "$tools_dir/rkImageMaker" || elf2_die "rkImageMaker is not executable"
    test -x "$tools_dir/afptool" || elf2_die "afptool is not executable"
    elf2_require_file "$toolchain_source/bin/aarch64-none-linux-gnu-gcc"
    elf2_require_dir "$toolchain_source/aarch64-none-linux-gnu/libc"
    local expected actual
    expected=$(awk 'NF { print $1; exit }' "$md5_file")
    actual=$(md5sum "$image" | awk '{print $1}')
    [[ "$expected" == "$actual" ]] || elf2_die "Desktop image MD5 mismatch: expected $expected, got $actual"
    echo "Desktop image MD5 OK: $actual"
    echo "Rockchip unpack tools OK: $tools_dir"
    echo "GCC source OK: $toolchain_source"
}

verify_inputs

if [[ -d "$toolchain_root" ]]; then
    elf2_require_file "$toolchain_root/bin/aarch64-none-linux-gnu-gcc"
    elf2_require_dir "$toolchain_root/aarch64-none-linux-gnu/libc"
    echo "GCC toolchain already installed: $toolchain_root"
elif ((check_only)); then
    echo "GCC toolchain destination not installed (check mode; no copy performed): $toolchain_root"
else
    mkdir -p "$(dirname "$toolchain_root")"
    toolchain_stage=$(mktemp -d /tmp/elf2-toolchain.XXXXXX)
    trap 'rm -rf -- "$toolchain_stage"' EXIT
    cp -a "$toolchain_source/." "$toolchain_stage/"
    elf2_require_file "$toolchain_stage/bin/aarch64-none-linux-gnu-gcc"
    "$toolchain_stage/bin/aarch64-none-linux-gnu-gcc" --version >/dev/null
    mv "$toolchain_stage" "$toolchain_root"
    trap - EXIT
    echo "Installed GCC toolchain: $toolchain_root"
fi

if ((check_only)); then
    exit 0
fi

sysroot="$install_root/sysroot"
if [[ -e "$install_root" ]]; then
    elf2_die "Jammy install root already exists; refusing to mutate it: $install_root"
fi

mkdir -p "$(dirname "$install_root")"
stage=$(mktemp -d /tmp/elf2-jammy.XXXXXX)
elf2_assert_safe_staging_dir "$stage"
cleanup() { rm -rf -- "$stage"; }
trap cleanup EXIT
mkdir -p "$stage/outer" "$stage/parts" "$stage/sysroot" "$stage/manifests"

"$tools_dir/rkImageMaker" -unpack "$image" "$stage/outer"
elf2_require_file "$stage/outer/firmware.img"
"$tools_dir/afptool" -unpack "$stage/outer/firmware.img" "$stage/parts"
mapfile -t rootfs_images < <(find "$stage/parts" -type f \( -name rootfs.img -o -name jammy-rootfs.img \) -print)
(( ${#rootfs_images[@]} == 1 )) || elf2_die "expected exactly one rootfs.img or jammy-rootfs.img, found ${#rootfs_images[@]}"
rootfs_image=${rootfs_images[0]}
file "$rootfs_image" | grep -Eiq 'ext[234].*filesystem|filesystem.*ext[234]' || elf2_die "rootfs is not an ext4 filesystem: $rootfs_image"
dumpe2fs -h "$rootfs_image" >/dev/null 2>&1 || elf2_die "cannot inspect rootfs ext4 metadata: $rootfs_image"

cp -a "$toolchain_source/aarch64-none-linux-gnu/libc/." "$stage/sysroot/"
for spec in \
    '/lib/aarch64-linux-gnu|/lib' \
    '/usr/lib/aarch64-linux-gnu|/usr/lib' \
    '/usr/local/lib|/usr/local' \
    '/usr/local/include|/usr/local'; do
    root_path=${spec%%|*}
    local_parent=${spec##*|}
    mkdir -p "$stage/sysroot$local_parent"
    if debugfs -R "stat $root_path" "$rootfs_image" 2>/dev/null | grep -q 'Inode:'; then
        debugfs -R "rdump $root_path $stage/sysroot$local_parent" "$rootfs_image" >/dev/null 2>&1
    fi
done

# Keep the startup objects from the extracted Jammy rootfs and provide
# multiarch linker inputs for the GCC 10.3 driver.  The rootfs runtime libc
# remains authoritative; the linker scripts below avoid accidentally selecting
# the GCC bundle's older /lib64 glibc when resolving vendor libraries.
multiarch_lib="$stage/sysroot/lib/aarch64-linux-gnu"
multiarch_dev="$stage/sysroot/usr/lib/aarch64-linux-gnu"
driver_lib="$stage/sysroot/usr/lib64"
mkdir -p "$multiarch_lib" "$multiarch_dev" "$driver_lib"
for crt in crt1.o crti.o crtn.o; do
    if [[ -e "$multiarch_lib/$crt" ]]; then
        cp -a "$multiarch_lib/$crt" "$driver_lib/$crt"
    else
        gcc_crt="$toolchain_source/aarch64-none-linux-gnu/libc/usr/lib64/$crt"
        [[ -e "$gcc_crt" ]] && cp -a "$gcc_crt" "$driver_lib/$crt"
    fi
done
if [[ -e "$multiarch_lib/libc.so.6" && -e "$multiarch_dev/libc_nonshared.a" ]]; then
    for libc_script in "$multiarch_dev/libc.so" "$driver_lib/libc.so"; do
        printf '%s\n' \
            '/* ELF2 Jammy multiarch libc linker script */' \
            'OUTPUT_FORMAT(elf64-littleaarch64)' \
            'GROUP ( /lib/aarch64-linux-gnu/libc.so.6 /usr/lib/aarch64-linux-gnu/libc_nonshared.a AS_NEEDED ( /lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 ) )' \
            >"$libc_script"
    done
    for lib in pthread m rt dl util; do
        [[ -e "$multiarch_lib/lib$lib.so.0" ]] || continue
        ln -sfn "../../../lib/aarch64-linux-gnu/lib$lib.so.0" "$multiarch_dev/lib$lib.so"
        ln -sfn "../../lib/aarch64-linux-gnu/lib$lib.so.0" "$driver_lib/lib$lib.so"
    done
fi
# The GCC bundle contains an older libstdc++ (GLIBCXX_3.4.26).  Prefer the
# Jammy runtime extracted from the board image (GLIBCXX_3.4.30) for both link
# and deployment; keep the GCC C++ headers in place.
if [[ -e "$multiarch_lib/libstdc++.so.6" ]]; then
    rm -f "$driver_lib"/libstdc++.so "$driver_lib"/libstdc++.so.6 "$driver_lib"/libstdc++.so.6.* "$driver_lib"/libstdc++.so.6.*-gdb.py
    ln -sfn "../../lib/aarch64-linux-gnu/libstdc++.so.6" "$driver_lib/libstdc++.so"
    ln -sfn "../../lib/aarch64-linux-gnu/libstdc++.so.6" "$driver_lib/libstdc++.so.6"
    ln -sfn "../../../lib/aarch64-linux-gnu/libstdc++.so.6" "$multiarch_dev/libstdc++.so"
fi

manifest_dir="$stage/manifests"
if debugfs -R 'stat /etc/os-release' "$rootfs_image" 2>/dev/null | grep -q 'Inode:'; then
    debugfs -R "dump /etc/os-release $manifest_dir/os-release" "$rootfs_image" >/dev/null 2>&1
fi
if debugfs -R 'stat /var/lib/dpkg/status' "$rootfs_image" 2>/dev/null | grep -q 'Inode:'; then
    debugfs -R "dump /var/lib/dpkg/status $manifest_dir/dpkg-status" "$rootfs_image" >/dev/null 2>&1
fi

image_sha256=$(sha256sum "$image" | awk '{print $1}')
tool_sha256=$(sha256sum "$tools_dir/rkImageMaker" | awk '{print $1}')
afptool_sha256=$(sha256sum "$tools_dir/afptool" | awk '{print $1}')
compiler_version=$("$toolchain_source/bin/aarch64-none-linux-gnu-gcc" --version | sed -n '1p')
rootfs_uuid=$(dumpe2fs -h "$rootfs_image" 2>/dev/null | sed -n 's/^Filesystem UUID:[[:space:]]*//p' | sed -n '1p')
{
    printf 'IMAGE_MD5=%s\n' "$(md5sum "$image" | awk '{print $1}')"
    printf 'IMAGE_SHA256=%s\n' "$image_sha256"
    printf 'RKIMAGEMAKER_SHA256=%s\n' "$tool_sha256"
    printf 'AFPTOOL_SHA256=%s\n' "$afptool_sha256"
    printf 'TOOLCHAIN_SOURCE_SHA256=%s\n' "$toolchain_archive_sha256"
    printf 'ROOTFS_SHA256=%s\n' "$(sha256sum "$rootfs_image" | awk '{print $1}')"
    printf 'ROOTFS_UUID=%s\n' "$rootfs_uuid"
} >"$manifest_dir/base-inputs.sha256"
{
    printf 'COMPILER=%s\n' "$compiler_version"
    printf 'IMAGE=%s\n' "$image"
    printf 'ROOTFS=%s\n' "$rootfs_image"
    printf 'TOOLS_DIR=%s\n' "$tools_dir"
    printf 'TOOLCHAIN_SOURCE=%s\n' "$toolchain_source"
    if [[ -f "$manifest_dir/os-release" ]]; then cat "$manifest_dir/os-release"; fi
} >"$manifest_dir/base-environment.txt"

mv "$stage" "$install_root"
trap - EXIT
echo "Installed Jammy base sysroot: $sysroot"
