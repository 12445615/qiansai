#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
sysroot="$tmp/sysroot"
toolchain="$tmp/toolchain"
mkdir -p "$sysroot/usr/include/rknn" "$sysroot/usr/include/rga" "$sysroot/usr/include/modbus" \
    "$sysroot/usr/lib/aarch64-linux-gnu" "$sysroot/usr/local/lib" "$toolchain/bin"
printf '#!/bin/sh\nexit 0\n' >"$toolchain/bin/aarch64-none-linux-gnu-gcc"
chmod +x "$toolchain/bin/aarch64-none-linux-gnu-gcc"
printf 'rknn\n' >"$sysroot/usr/include/rknn/rknn_api.h"
printf 'rga\n' >"$sysroot/usr/include/rga/im2d.h"
printf 'mqtt\n' >"$sysroot/usr/include/MQTTClient.h"
printf 'modbus\n' >"$sysroot/usr/include/modbus/modbus.h"
mkdir -p "$sysroot/usr/include/libavcodec"
printf '#define LIBAVCODEC_VERSION_MAJOR 60\n' >"$sysroot/usr/include/libavcodec/version_major.h"
printf 'void fixture(void) {}\n' >"$tmp/fixture.c"
for spec in \
    'libavcodec.so.60 libavcodec.so' \
    'libpaho-mqtt3c.so.1 libpaho-mqtt3c.so' \
    'libmodbus.so.5 libmodbus.so' \
    'librockchip_mpp.so.1 librockchip_mpp.so' \
    'librknnrt.so librknnrt.so' \
    'librga.so.2 librga.so'; do
    set -- $spec
    gcc -shared -fPIC -Wl,-soname,"$1" -o "$sysroot/usr/lib/aarch64-linux-gnu/$1" "$tmp/fixture.c"
    if [[ "$1" != "$2" ]]; then ln -s "$1" "$sysroot/usr/lib/aarch64-linux-gnu/$2"; fi
done
mkdir -p "$sysroot/manifests"
printf 'COMMIT=99d938d6ea32c31457bb68a5a2177c94d13a4854\n' >"$sysroot/manifests/ffmpeg-rockchip.txt"
printf 'CONFIG_H264_RKMPP_ENCODER=1\nCONFIG_AAC_ENCODER=1\n' >"$sysroot/manifests/ffmpeg-rockchip-components.txt"

run_fail() {
    local needle=$1
    if make -s -C "$project_dir" check-env PROFILE=jammy-deploy \
        SYSROOT="$sysroot" CROSS_COMPILE="$toolchain/bin/aarch64-none-linux-gnu-" \
        >"$tmp/out" 2>&1; then
        echo "strict check unexpectedly passed: $needle" >&2
        exit 1
    fi
    grep -Fq "$needle" "$tmp/out"
}

rm "$sysroot/usr/include/MQTTClient.h"
run_fail 'Paho MQTT C is required'
printf 'mqtt\n' >"$sysroot/usr/include/MQTTClient.h"
rm "$sysroot/usr/include/modbus/modbus.h"
run_fail 'libmodbus is required'
printf 'modbus\n' >"$sysroot/usr/include/modbus/modbus.h"
rm "$sysroot/usr/lib/aarch64-linux-gnu/librockchip_mpp.so"
run_fail 'RKMPP runtime library is required'
ln -s librockchip_mpp.so.1 "$sysroot/usr/lib/aarch64-linux-gnu/librockchip_mpp.so"
rm "$sysroot/usr/include/rknn/rknn_api.h"
run_fail 'RKNN header missing'
printf 'rknn\n' >"$sysroot/usr/include/rknn/rknn_api.h"
rm "$sysroot/usr/include/libavcodec/version_major.h"
run_fail 'FFmpeg ABI 60 is required'
printf '#define LIBAVCODEC_VERSION_MAJOR 60\n' >"$sysroot/usr/include/libavcodec/version_major.h"
printf 'CONFIG_AAC_ENCODER=1\n' >"$sysroot/manifests/ffmpeg-rockchip-components.txt"
run_fail 'h264_rkmpp_encoder evidence is missing'

echo 'Jammy strict environment fails closed'
