#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_dir/scripts/lib/elf2_common.sh"
elf2_sanitize_path

vdi=/mnt/e/VirtualBox/VMS/ubuntu1/ubuntu1.vdi
guest_dir=/home/c/linux/ELF2/work/ELF2-linux-source/tools/linux/Linux_Pack_Firmware/rockdev
output_dir=${HOME}/.local/opt/elf2-sdk/sources/rockdev-tools
guestfish_bin=${GUESTFISH_BIN:-guestfish}

usage() {
    cat <<'EOF'
Usage: capture_rockchip_tools.sh [--vdi PATH] [--guest-dir PATH]
       [--output-dir PATH]
EOF
}

while (($#)); do
    case "$1" in
        --vdi|--guest-dir|--output-dir)
            (($# >= 2)) || { echo "error: $1 requires a value" >&2; exit 2; }
            case "$1" in
                --vdi) vdi=$2 ;;
                --guest-dir) guest_dir=$2 ;;
                --output-dir) output_dir=$2 ;;
            esac
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

elf2_require_file "$vdi"
command -v "$guestfish_bin" >/dev/null 2>&1 || elf2_die "guestfish not found: $guestfish_bin"
mkdir -p "$(dirname "$output_dir")"

if [[ -d "$output_dir" ]]; then
    if [[ -f "$output_dir/manifest.sha256" ]] && (cd "$output_dir" && sha256sum -c manifest.sha256 >/dev/null); then
        echo "Rockchip tools already captured: $output_dir"
        exit 0
    fi
    elf2_die "refusing to overwrite existing unverified directory: $output_dir"
fi

stage=$(mktemp -d /tmp/elf2-rockdev.XXXXXX)
elf2_assert_safe_staging_dir "$stage"
cleanup() { rm -rf -- "$stage"; }
trap cleanup EXIT

"$guestfish_bin" --ro -a "$vdi" -i copy-out "$guest_dir" "$stage"

if [[ -d "$stage/rockdev" ]]; then
    rockdev_dir=$stage/rockdev
else
    rockdev_dir=$stage
fi
for tool in afptool rkImageMaker unpack.sh; do
    test -x "$rockdev_dir/$tool" || elf2_die "captured Rockchip tool is missing or not executable: $tool"
done

if [[ "$rockdev_dir" != "$stage/rockdev" ]]; then
    mkdir "$stage/rockdev"
    for tool in afptool rkImageMaker unpack.sh; do
        mv "$stage/$tool" "$stage/rockdev/$tool"
    done
fi
find "$stage/rockdev" -type f -printf '%P\n' | sort | while IFS= read -r rel; do
    (cd "$stage" && sha256sum "rockdev/$rel")
done >"$stage/manifest.sha256"

mv "$stage" "$output_dir"
trap - EXIT
echo "Captured Rockchip tools: $output_dir"
