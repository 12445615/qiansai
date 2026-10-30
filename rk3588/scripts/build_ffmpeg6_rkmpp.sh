#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_dir/scripts/lib/elf2_common.sh"
elf2_sanitize_path

commit=99d938d6ea32c31457bb68a5a2177c94d13a4854
source_url="https://github.com/nyanmisaka/ffmpeg-rockchip/archive/${commit}.tar.gz"
source_sha256=0779552be0813989764275e411a85c14d68efffca1d3e98641e3d4bfb5ac8bad
sysroot="$HOME/.local/opt/elf2-sdk/profiles/jammy-deploy/sysroot"
toolchain_root="$HOME/.local/opt/elf2-sdk/toolchains/gcc-arm-10.3-aarch64"
source_cache="$HOME/.local/opt/elf2-sdk/sources/ffmpeg-rockchip-${commit}.tar.gz"
jobs=1
print_only=0
configure_only=0

usage() {
    cat <<'EOF'
Usage: build_ffmpeg6_rkmpp.sh [--print-config] [--sysroot PATH]
       [--toolchain-root PATH] [--source-cache PATH] [--jobs N]
EOF
}

while (($#)); do
    case "$1" in
        --print-config) print_only=1; shift ;;
        --configure-only) configure_only=1; shift ;;
        --sysroot|--toolchain-root|--source-cache)
            (($# >= 2)) || { echo "error: $1 requires a value" >&2; exit 2; }
            case "$1" in
                --sysroot) sysroot=$2 ;;
                --toolchain-root) toolchain_root=$2 ;;
                --source-cache) source_cache=$2 ;;
            esac
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || { echo 'error: --jobs requires a value' >&2; exit 2; }
            [[ $2 =~ ^[1-9][0-9]*$ ]] || { echo 'error: --jobs must be positive' >&2; exit 2; }
            jobs=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

cross_prefix="$toolchain_root/bin/aarch64-none-linux-gnu-"
config_flags=(
    --enable-cross-compile
    --enable-shared
    --disable-static
    --disable-programs
    --disable-doc
    --disable-autodetect
    --disable-everything
    --enable-rkmpp
    --enable-version3
    --enable-libdrm
    --enable-encoder=h264_rkmpp
    --enable-encoder=aac
    --enable-muxer=flv
    --enable-muxer=mpegts
    --enable-protocol=file
    --enable-protocol=pipe
    --enable-protocol=tcp
    --enable-protocol=rtmp
    --prefix=/usr/local
    --libdir=/usr/local/lib
    --incdir=/usr/local/include
    --arch=aarch64
    --target-os=linux
    --cross-prefix="$cross_prefix"
    --pkg-config=pkg-config
    --sysroot="$sysroot"
    --enable-network
    --enable-swresample
    --enable-parser=h264
    --enable-parser=aac
    --enable-bsf=h264_mp4toannexb
    --enable-bsf=aac_adtstoasc
)

if ((print_only)); then
    printf 'COMMIT=%s\n' "$commit"
    printf 'SOURCE_URL=%s\n' "$source_url"
    printf 'SOURCE_SHA256=%s\n' "$source_sha256"
    printf '%s\n' "${config_flags[@]}"
    exit 0
fi

elf2_require_dir "$sysroot"
elf2_require_file "${cross_prefix}gcc"
mkdir -p "$(dirname "$source_cache")"
if [[ ! -f "$source_cache" ]]; then
    curl -L --fail --retry 3 -o "$source_cache" "$source_url"
fi
actual_sha=$(elf2_sha256 "$source_cache")
[[ "$actual_sha" == "$source_sha256" ]] || elf2_die "FFmpeg source SHA-256 mismatch: $actual_sha"

work=$(mktemp -d /tmp/elf2-ffmpeg.XXXXXX)
elf2_assert_safe_staging_dir "$work"
cleanup() { rm -rf -- "$work"; }
trap cleanup EXIT
tar -xzf "$source_cache" -C "$work"
source_dir=$(find "$work" -mindepth 1 -maxdepth 1 -type d -print -quit)
elf2_require_dir "$source_dir"
build_dir="$work/build"
mkdir "$build_dir"
export PKG_CONFIG_SYSROOT_DIR="$sysroot"
export PKG_CONFIG_LIBDIR="$sysroot/usr/local/lib/pkgconfig:$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_PATH=
extra_cflags="-I$sysroot/usr/include -I$sysroot/usr/local/include"
extra_ldflags="-L$sysroot/usr/local/lib -L$sysroot/usr/lib/aarch64-linux-gnu"
(cd "$build_dir" && "$source_dir/configure" "${config_flags[@]}" \
    --extra-cflags="$extra_cflags" --extra-ldflags="$extra_ldflags")
if ((configure_only)); then
    echo "FFmpeg-Rockchip configure passed: $build_dir"
    exit 0
fi
make -C "$build_dir" -j"$jobs"
components="$build_dir/config_components.h"
for macro in CONFIG_H264_RKMPP_ENCODER CONFIG_AAC_ENCODER CONFIG_FLV_MUXER CONFIG_MPEGTS_MUXER CONFIG_RTMP_PROTOCOL; do
    grep -Fxq "#define $macro 1" "$components" || elf2_die "FFmpeg component missing: $macro"
done
stage="$work/install"
mkdir "$stage"
make -C "$build_dir" DESTDIR="$stage" install
for soname in libavcodec.so.60 libavformat.so.60 libavutil.so.58 libswresample.so.4; do
    find "$stage/usr/local/lib" -maxdepth 1 -name "$soname" -print -quit | grep -q . || elf2_die "staged FFmpeg library missing: $soname"
done
mkdir -p "$sysroot/usr/local"
cp -a "$stage/usr/local/." "$sysroot/usr/local/"
mkdir -p "$sysroot/manifests"
{
    printf 'COMMIT=%s\nSOURCE_URL=%s\nSOURCE_SHA256=%s\n' "$commit" "$source_url" "$source_sha256"
    printf 'CONFIGURE_SHA256=%s\n' "$(sha256sum "$build_dir/ffbuild/config.log" | awk '{print $1}')"
} >"$sysroot/manifests/ffmpeg-rockchip.txt"
{
    for macro in CONFIG_H264_RKMPP_ENCODER CONFIG_AAC_ENCODER CONFIG_FLV_MUXER CONFIG_MPEGTS_MUXER CONFIG_RTMP_PROTOCOL; do
        printf '%s=1\n' "$macro"
    done
} >"$sysroot/manifests/ffmpeg-rockchip-components.txt"
echo "Installed FFmpeg-Rockchip 6: $sysroot/usr/local"
