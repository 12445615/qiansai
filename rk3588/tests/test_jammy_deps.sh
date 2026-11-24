#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
pkgroot="$tmp/pkgroot"
mkdir -p "$pkgroot" "$tmp/packages" "$tmp/sysroot/usr/lib/aarch64-linux-gnu" \
    "$tmp/buildroot/usr/include/rknn" "$tmp/buildroot/usr/include/rga" "$tmp/buildroot/usr/include/libdrm" \
    "$tmp/mpp/include" "$tmp/mpp" "$tmp/freetype"

make_so() {
    local name=$1 out=$2
    printf 'void elf2_fixture(void) {}\n' >"$tmp/fixture.c"
    gcc -shared -fPIC -Wl,-soname,"$name" -o "$out" "$tmp/fixture.c"
}
for pair in 'librockchip_mpp.so.1' 'librga.so.2' 'librknnrt.so' 'libdrm.so.2' 'libfreetype.so.6'; do
    make_so "$pair" "$tmp/sysroot/usr/lib/aarch64-linux-gnu/$pair"
done
ln -s librockchip_mpp.so.1 "$tmp/sysroot/usr/lib/aarch64-linux-gnu/librockchip_mpp.so"
ln -s librga.so.2 "$tmp/sysroot/usr/lib/aarch64-linux-gnu/librga.so"
printf 'rknn header\n' >"$tmp/buildroot/usr/include/rknn/rknn_api.h"
printf 'rga header\n' >"$tmp/buildroot/usr/include/rga/im2d.h"
printf 'drm header\n' >"$tmp/buildroot/usr/include/libdrm/drm.h"
printf 'xf86drm header\n' >"$tmp/buildroot/usr/include/xf86drm.h"
printf 'mpp header\n' >"$tmp/mpp/include/rk_mpi.h"
printf 'prefix=/usr/local\n' >"$tmp/mpp/rockchip_mpp.pc"
printf 'freetype header\n' >"$tmp/freetype/ft2build.h"

make_pkg() {
    local name=$1 arch=$2
    local root="$pkgroot/$name"
    mkdir -p "$root/DEBIAN"
    printf 'Package: %s\nVersion: 1.0\nArchitecture: %s\nMaintainer: fixture\nDescription: fixture\n' "$name" "$arch" >"$root/DEBIAN/control"
    dpkg-deb --build --root-owner-group "$root" "$tmp/packages/${name}_1.0_${arch}.deb" >/dev/null
}
make_pkg libpaho-mqtt1.3 arm64
mkdir -p "$pkgroot/libpaho-mqtt1.3/usr/lib/aarch64-linux-gnu"
make_so libpaho-mqtt3c.so.1 "$pkgroot/libpaho-mqtt1.3/usr/lib/aarch64-linux-gnu/libpaho-mqtt3c.so.1"
make_pkg libpaho-mqtt-dev arm64
mkdir -p "$pkgroot/libpaho-mqtt-dev/usr/include" "$pkgroot/libpaho-mqtt-dev/usr/lib/aarch64-linux-gnu"
printf 'MQTTClient\n' >"$pkgroot/libpaho-mqtt-dev/usr/include/MQTTClient.h"
ln -s libpaho-mqtt3c.so.1 "$pkgroot/libpaho-mqtt-dev/usr/lib/aarch64-linux-gnu/libpaho-mqtt3c.so"
make_pkg libmodbus5 arm64
mkdir -p "$pkgroot/libmodbus5/usr/lib/aarch64-linux-gnu"
make_so libmodbus.so.5 "$pkgroot/libmodbus5/usr/lib/aarch64-linux-gnu/libmodbus.so.5"
make_pkg libmodbus-dev arm64
mkdir -p "$pkgroot/libmodbus-dev/usr/include/modbus" "$pkgroot/libmodbus-dev/usr/lib/aarch64-linux-gnu"
printf 'modbus\n' >"$pkgroot/libmodbus-dev/usr/include/modbus/modbus.h"
ln -s libmodbus.so.5 "$pkgroot/libmodbus-dev/usr/lib/aarch64-linux-gnu/libmodbus.so"
make_pkg libssl-dev arm64
mkdir -p "$pkgroot/libssl-dev/usr/include/openssl" "$pkgroot/libssl-dev/usr/include/aarch64-linux-gnu/openssl"
printf 'openssl evp fixture\n' >"$pkgroot/libssl-dev/usr/include/openssl/evp.h"
printf 'openssl config fixture\n' >"$pkgroot/libssl-dev/usr/include/aarch64-linux-gnu/openssl/opensslconf.h"
for pkg in libcurl4-openssl-dev libcurl4 libsqlite3-dev libsqlite3-0; do
    make_pkg "$pkg" arm64
done

for pkg in libssl-dev libpaho-mqtt-dev libpaho-mqtt1.3 libmodbus-dev libmodbus5; do
    dpkg-deb --build --root-owner-group "$pkgroot/$pkg" "$tmp/packages/${pkg}_1.0_arm64.deb" >/dev/null
done

mkdir -p "$tmp/sysroot/usr/include"
bash "$project_dir/scripts/install_elf2_jammy_deps.sh" \
    --sysroot "$tmp/sysroot" \
    --package-dir "$tmp/packages" \
    --buildroot-sysroot "$tmp/buildroot" \
    --mpp-include "$tmp/mpp/include" \
    --mpp-pc "$tmp/mpp/rockchip_mpp.pc" \
    --freetype-include "$tmp/freetype" \
    --rknn-runtime "$tmp/sysroot/usr/lib/aarch64-linux-gnu/librknnrt.so"

test -f "$tmp/sysroot/usr/include/MQTTClient.h"
test -f "$tmp/sysroot/usr/include/openssl/evp.h"
test -f "$tmp/sysroot/usr/include/modbus/modbus.h"
test -L "$tmp/sysroot/usr/lib/aarch64-linux-gnu/libpaho-mqtt3c.so"
test -L "$tmp/sysroot/usr/lib/aarch64-linux-gnu/libmodbus.so"
test -f "$tmp/sysroot/usr/local/include/rockchip/rk_mpi.h"
test -L "$tmp/sysroot/usr/lib/aarch64-linux-gnu/librockchip_mpp.so"
test -f "$tmp/sysroot/usr/include/rknn/rknn_api.h"
test -f "$tmp/sysroot/usr/include/rga/im2d.h"
test -f "$tmp/sysroot/usr/include/xf86drm.h"
test -f "$tmp/sysroot/manifests/development-overlay.txt"

echo 'Jammy development overlay installs arm64 dependencies'
