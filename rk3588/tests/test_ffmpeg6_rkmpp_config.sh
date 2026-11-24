#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
config=$(bash "$project_dir/scripts/build_ffmpeg6_rkmpp.sh" --print-config \
    --sysroot /fixture/sysroot --toolchain-root /fixture/toolchain)
grep -Fxq 'COMMIT=99d938d6ea32c31457bb68a5a2177c94d13a4854' <<<"$config"
grep -Fxq 'SOURCE_SHA256=0779552be0813989764275e411a85c14d68efffca1d3e98641e3d4bfb5ac8bad' <<<"$config"
for flag in \
    --enable-cross-compile --enable-shared --disable-static --disable-programs \
    --disable-doc --disable-autodetect --disable-everything --enable-rkmpp \
    --enable-libdrm --enable-encoder=h264_rkmpp --enable-encoder=aac \
    --enable-muxer=flv --enable-muxer=mpegts --enable-protocol=file \
    --enable-protocol=pipe --enable-protocol=tcp --enable-protocol=rtmp; do
    grep -Fxq -- "$flag" <<<"$config"
done
echo 'FFmpeg-Rockchip configuration is pinned'
