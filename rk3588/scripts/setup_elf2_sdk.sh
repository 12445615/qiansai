#!/usr/bin/env bash
set -euo pipefail

default_archive='/mnt/f/ELF 2开发板资料包/05-常用工具/01-编译工具安装脚本/aarch64-buildroot-linux-gnu_sdk-buildroot.tar.gz'
archive_path=${ELF2_SDK_ARCHIVE:-$default_archive}
install_parent=${ELF2_SDK_INSTALL_DIR:-"$HOME/.local/opt/elf2-sdk"}
check_only=0
sdk_dir_name='aarch64-buildroot-linux-gnu_sdk-buildroot'

usage() {
    cat <<'EOF'
Usage: setup_elf2_sdk.sh [options]

Install and relocate the official ELF2 Buildroot cross SDK from the Windows F drive.

Options:
  --archive PATH      Override the SDK tarball path.
  --install-dir PATH  Override the installation parent directory.
  --check             Validate the archive and current installation only.
  -h, --help          Show this help.

Environment overrides:
  ELF2_SDK_ARCHIVE
  ELF2_SDK_INSTALL_DIR
EOF
}

while (($# > 0)); do
    case "$1" in
        --archive)
            [[ $# -ge 2 ]] || { echo 'error: --archive requires a path' >&2; exit 2; }
            archive_path=$2
            shift 2
            ;;
        --install-dir)
            [[ $# -ge 2 ]] || { echo 'error: --install-dir requires a path' >&2; exit 2; }
            install_parent=$2
            shift 2
            ;;
        --check)
            check_only=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

sdk_root="$install_parent/$sdk_dir_name"
compiler="$sdk_root/bin/aarch64-buildroot-linux-gnu-gcc"
sysroot="$sdk_root/aarch64-buildroot-linux-gnu/sysroot"

verify_archive() {
    if [[ ! -r "$archive_path" ]]; then
        echo "error: ELF2 SDK archive not found: $archive_path" >&2
        echo 'hint: connect/mount the F drive or pass --archive PATH' >&2
        return 1
    fi
    echo "ELF2 SDK archive OK: $archive_path"
}

verify_installation() {
    local required_paths=(
        "$compiler"
        "$sysroot/usr/include/rknn/rknn_api.h"
        "$sysroot/usr/lib/librknnrt.so"
        "$sysroot/usr/include/rga/im2d.h"
        "$sysroot/usr/lib/librga.so"
        "$sysroot/usr/include/libavcodec/avcodec.h"
        "$sysroot/usr/lib/libavcodec.so"
    )
    local required_path

    for required_path in "${required_paths[@]}"; do
        if [[ ! -e "$required_path" ]]; then
            echo "error: incomplete ELF2 SDK installation, missing: $required_path" >&2
            return 1
        fi
    done
    if [[ ! -x "$compiler" ]]; then
        echo "error: ELF2 cross compiler is not executable: $compiler" >&2
        return 1
    fi

    echo "ELF2 SDK installation OK: $sdk_root"
    "$compiler" --version | sed -n '1p'
}

verify_archive

if ((check_only)); then
    verify_installation
    exit 0
fi

if [[ -d "$sdk_root" ]]; then
    verify_installation
    echo 'ELF2 SDK is already installed; no files changed.'
    exit 0
fi

mkdir -p "$install_parent"
echo "Extracting ELF2 SDK to: $install_parent"
tar -xzf "$archive_path" -C "$install_parent"

if [[ ! -x "$sdk_root/relocate-sdk.sh" ]]; then
    echo "error: SDK archive did not create the expected directory: $sdk_root" >&2
    exit 1
fi

"$sdk_root/relocate-sdk.sh"
verify_installation
