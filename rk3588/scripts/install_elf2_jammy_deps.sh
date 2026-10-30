#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_dir/scripts/lib/elf2_common.sh"
elf2_sanitize_path

sysroot="$HOME/.local/opt/elf2-sdk/profiles/jammy-deploy/sysroot"
package_dir='/home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/Project/camera_flow/packages/arm64'
buildroot_sysroot="$HOME/.local/opt/elf2-sdk/aarch64-buildroot-linux-gnu_sdk-buildroot/aarch64-buildroot-linux-gnu/sysroot"
mpp_include='/home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/mpp-include'
mpp_pc='/home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/usr-local-lib/pkgconfig/rockchip_mpp.pc'
freetype_include="$project_dir/third_party/elf2_sysroot/usr/include/freetype2"
rknn_runtime='/home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/Project/camera_flow/third_party/rknn_runtime/lib/librknnrt.so'
check_only=0

while (($#)); do
    case "$1" in
        --sysroot|--package-dir|--buildroot-sysroot|--mpp-include|--mpp-pc|--freetype-include|--rknn-runtime)
            (($# >= 2)) || { echo "error: $1 requires a value" >&2; exit 2; }
            case "$1" in
                --sysroot) sysroot=$2 ;;
                --package-dir) package_dir=$2 ;;
                --buildroot-sysroot) buildroot_sysroot=$2 ;;
                --mpp-include) mpp_include=$2 ;;
                --mpp-pc) mpp_pc=$2 ;;
                --freetype-include) freetype_include=$2 ;;
                --rknn-runtime) rknn_runtime=$2 ;;
            esac
            shift 2
            ;;
        --check) check_only=1; shift ;;
        -h|--help) echo 'Usage: install_elf2_jammy_deps.sh [--sysroot PATH] [--package-dir PATH]'; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; exit 2 ;;
    esac
done

elf2_require_dir "$sysroot"
elf2_require_dir "$package_dir"
elf2_require_dir "$buildroot_sysroot"
elf2_require_dir "$mpp_include"
elf2_require_file "$mpp_pc"
elf2_require_dir "$freetype_include"
elf2_require_file "$rknn_runtime"

packages=(
    'libpaho-mqtt-dev_*_arm64.deb'
    'libpaho-mqtt1.3_*_arm64.deb'
    'libmodbus-dev_*_arm64.deb'
    'libmodbus5_*_arm64.deb'
    'libcurl4-openssl-dev_*_arm64.deb'
    'libcurl4_*_arm64.deb'
    'libsqlite3-dev_*_arm64.deb'
    'libsqlite3-0_*_arm64.deb'
)
selected=()
for pattern in "${packages[@]}"; do
    mapfile -t matches < <(compgen -G "$package_dir/$pattern" || true)
    (( ${#matches[@]} == 1 )) || elf2_die "expected exactly one package for $pattern, found ${#matches[@]}"
    pkg=${matches[0]}
    [[ "$(dpkg-deb -f "$pkg" Architecture)" == arm64 ]] || elf2_die "package is not arm64: $pkg"
    selected+=("$pkg")
done

if ((check_only)); then
    printf 'package OK: %s\n' "${selected[@]}"
    exit 0
fi

overlay=$(mktemp -d /tmp/elf2-jammy-deps.XXXXXX)
elf2_assert_safe_staging_dir "$overlay"
cleanup() { rm -rf -- "$overlay"; }
trap cleanup EXIT
cp -a "$sysroot/." "$overlay/"
for pkg in "${selected[@]}"; do
    dpkg-deb -x "$pkg" "$overlay"
done

mkdir -p "$overlay/usr/local/include/rockchip" "$overlay/usr/local/lib/pkgconfig"
cp -a "$mpp_include/." "$overlay/usr/local/include/rockchip/"
cp -a "$mpp_pc" "$overlay/usr/local/lib/pkgconfig/"
sed -i "s#^libdir=.*#libdir=/usr/lib/aarch64-linux-gnu#" "$overlay/usr/local/lib/pkgconfig/rockchip_mpp.pc"
mkdir -p "$overlay/usr/include/rknn" "$overlay/usr/include/rga" "$overlay/usr/include/libdrm" "$overlay/usr/include/freetype2"
cp -a "$buildroot_sysroot/usr/include/rknn/." "$overlay/usr/include/rknn/"
cp -a "$buildroot_sysroot/usr/include/rga/." "$overlay/usr/include/rga/"
if [[ -d "$buildroot_sysroot/usr/include/libdrm" ]]; then cp -a "$buildroot_sysroot/usr/include/libdrm/." "$overlay/usr/include/libdrm/"; fi
for header in "$buildroot_sysroot"/usr/include/xf86drm*.h; do
    [[ -e "$header" ]] || continue
    cp -a "$header" "$overlay/usr/include/"
done
cp -a "$freetype_include/." "$overlay/usr/include/freetype2/"

libdir="$overlay/usr/lib/aarch64-linux-gnu"
mkdir -p "$libdir"
if ! find "$overlay/usr" \( -type f -o -type l \) -name 'librknnrt.so*' -print -quit | grep -q .; then
    cp -a "$rknn_runtime" "$libdir/librknnrt.so"
fi
for runtime in librknnrt.so libdrm.so.2 libfreetype.so.6; do
    find "$overlay/usr" \( -type f -o -type l \) -name "$runtime*" -print -quit | grep -q . || elf2_die "required Jammy runtime is missing: $runtime"
done
declare -A sonames=(
    [libpaho-mqtt3c.so]=libpaho-mqtt3c.so.1
    [libmodbus.so]=libmodbus.so.5
    [librockchip_mpp.so]=librockchip_mpp.so.1
    [librga.so]=librga.so.2
)
for link in "${!sonames[@]}"; do
    target=${sonames[$link]}
    if [[ ! -e "$libdir/$target" ]]; then
        mapfile -t candidates < <(find "$overlay/usr" \( -type f -o -type l \) -name "$target*" | sort)
        (( ${#candidates[@]} >= 1 )) || elf2_die "required Jammy runtime is missing: $target"
        cp -a "${candidates[0]}" "$libdir/$target"
    fi
    [[ -e "$libdir/$link" ]] || ln -s "$target" "$libdir/$link"
done

for pair in \
    'libpaho-mqtt3c.so.1|libpaho-mqtt3c.so.1' \
    'libmodbus.so.5|libmodbus.so.5' \
    'librockchip_mpp.so.1|librockchip_mpp.so.1' \
    'librga.so.2|librga.so.2'; do
    lib=${pair%%|*}
    expected=${pair##*|}
    readelf -d "$libdir/$lib" 2>/dev/null | grep -Eq "SONAME[^]]*$expected" || elf2_die "SONAME mismatch or non-ELF runtime: $lib"
done

manifest="$overlay/manifests/development-overlay.txt"
mkdir -p "$(dirname "$manifest")"
{
    echo 'packages:'
    for pkg in "${selected[@]}"; do
        printf '%s %s %s\n' "$(basename "$pkg")" "$(dpkg-deb -f "$pkg" Version)" "$(sha256sum "$pkg" | awk '{print $1}')"
    done
    echo 'vendor_headers:'
    printf 'mpp=%s\n' "$mpp_include"
    printf 'mpp_pkgconfig=%s\n' "$mpp_pc"
    printf 'rknn=%s\n' "$buildroot_sysroot/usr/include/rknn"
    printf 'rga=%s\n' "$buildroot_sysroot/usr/include/rga"
    printf 'freetype=%s\n' "$freetype_include"
    printf 'rknn_runtime=%s\n' "$rknn_runtime"
} >"$manifest"

cp -a "$overlay/." "$sysroot/"
trap - EXIT
rm -rf -- "$overlay"
echo "Installed Jammy development overlay: $sysroot"
