# ELF2 Dual-Profile SDK Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build reproducible `buildroot-check` and `jammy-deploy` profiles for `rk3588`, with the deployment profile strictly linking Jammy-compatible FFmpeg-Rockchip 6, RKMPP, Paho MQTT C, libmodbus, RKNN and RGA.

**Architecture:** Keep the official Buildroot SDK immutable for fast compile checks. Assemble a separate Jammy sysroot from the vendor Desktop image, overlay matching development files, then install a minimal FFmpeg-Rockchip build pinned to commit `99d938d6ea32c31457bb68a5a2177c94d13a4854`. The repository stores scripts, tests and manifests only; multi-gigabyte SDK data stays under `~/.local/opt/elf2-sdk`.

**Tech Stack:** Bash, GNU Make, GCC 10.3 AArch64 cross toolchain, Rockchip `rkImageMaker`/`afptool`, ext4 `debugfs`, Debian packages, FFmpeg-Rockchip 6, ELF `readelf`/`file`.

---

## Scope and file map

This plan covers host-side environment construction and offline verification. Board runtime validation remains a separate hardware-gated activity; the plan creates its checklist but does not delete `ubuntu1`.

Files created or changed:

- Create `rk3588/mk/elf2_profiles.mk`: the only source of Profile-specific compiler and sysroot paths.
- Modify `rk3588/Makefile`: consume Profile settings, expose strict feature detection and run the expanded test suite.
- Create `rk3588/scripts/lib/elf2_common.sh`: shared validation, PATH sanitization, hashing and safe staging helpers.
- Create `rk3588/scripts/build_elf2.sh`: clean, deterministic Profile build entry point.
- Create `rk3588/scripts/capture_rockchip_tools.sh`: copy the minimal firmware unpack tools from `ubuntu1` without modifying the VDI.
- Create `rk3588/scripts/setup_elf2_jammy_sdk.sh`: verify/unpack Desktop `update.img` and assemble the base Jammy sysroot.
- Create `rk3588/scripts/install_elf2_jammy_deps.sh`: install arm64 `.deb` packages and vendor development overlays.
- Create `rk3588/scripts/build_ffmpeg6_rkmpp.sh`: fetch, verify and build the pinned FFmpeg-Rockchip source.
- Create `rk3588/scripts/check_elf_deps.sh`: recursively resolve `DT_NEEDED` inside one sysroot.
- Create `rk3588/scripts/verify_elf2_sdk.sh`: run feature, ABI, closure and clean-build verification.
- Create `rk3588/tests/test_profile_config.sh`.
- Create `rk3588/tests/test_build_wrapper.sh`.
- Create `rk3588/tests/test_capture_rockchip_tools.sh`.
- Create `rk3588/tests/test_jammy_setup.sh`.
- Create `rk3588/tests/test_jammy_deps.sh`.
- Create `rk3588/tests/test_ffmpeg6_rkmpp_config.sh`.
- Create `rk3588/tests/test_elf_dependency_check.sh`.
- Create `rk3588/tests/probes/media_probe.c`, `mqtt_probe.c`, `modbus_probe.c`, `rknn_rga_probe.cc`.
- Modify `rk3588/README.md`: document both profiles and make the deployment boundary explicit.
- Create `rk3588/docs/elf2-sdk-recovery.md`: exact environment reconstruction procedure.
- Create `rk3588/docs/elf2-board-validation.md`: hardware validation and VM deletion gate.

## Fixed inputs

```text
Desktop image:
  /mnt/f/ELF 2开发板资料包/03-镜像/00-eMMC镜像/Desktop镜像/update.img
Vendor MD5:
  2999ff852d2e89bf9078ba9e201a61b2
Migration root:
  /home/chx/migrations/ubuntu1-elf2-2026-10-29
GCC 10.3:
  extracted/gcc-arm-10.3-aarch64-toolchain
Rockchip tools in VM:
  /home/c/linux/ELF2/work/ELF2-linux-source/tools/linux/Linux_Pack_Firmware/rockdev
FFmpeg-Rockchip repository:
  https://github.com/nyanmisaka/ffmpeg-rockchip.git
FFmpeg-Rockchip commit:
  99d938d6ea32c31457bb68a5a2177c94d13a4854
Commit archive SHA-256:
  0779552be0813989764275e411a85c14d68efffca1d3e98641e3d4bfb5ac8bad
```

### Task 1: Introduce the explicit Profile contract

**Files:**
- Create: `rk3588/mk/elf2_profiles.mk`
- Create: `rk3588/tests/test_profile_config.sh`
- Modify: `rk3588/Makefile:1-55,83-113`

- [ ] **Step 1: Write the failing Profile test**

Create `tests/test_profile_config.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
sdk_home=$(mktemp -d)
log=$(mktemp)
trap 'rm -rf -- "$sdk_home"; rm -f -- "$log"' EXIT

default_config=$(make -s -C "$project_dir" print-config)
grep -Fxq 'PROFILE=buildroot-check' <<<"$default_config"
grep -Fxq 'PROFILE_STRICT=0' <<<"$default_config"

mkdir -p "$sdk_home/toolchains/gcc-arm-10.3-aarch64/bin"
mkdir -p "$sdk_home/profiles/jammy-deploy/sysroot"
jammy_config=$(make -s -C "$project_dir" print-config \
    PROFILE=jammy-deploy ELF2_SDK_HOME="$sdk_home")
grep -Fxq 'PROFILE=jammy-deploy' <<<"$jammy_config"
grep -Fxq 'PROFILE_STRICT=1' <<<"$jammy_config"
grep -Fq "$sdk_home/toolchains/gcc-arm-10.3-aarch64/bin/aarch64-none-linux-gnu-gcc" <<<"$jammy_config"
grep -Fq "SYSROOT=$sdk_home/profiles/jammy-deploy/sysroot" <<<"$jammy_config"

if make -s -C "$project_dir" print-config PROFILE=unknown >"$log" 2>&1; then
    echo 'unknown profile unexpectedly succeeded' >&2
    exit 1
fi
grep -Fq 'unknown ELF2 profile: unknown' "$log"

echo 'profile selection is explicit'
```

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
bash tests/test_profile_config.sh
```

Expected: FAIL because `print-config` does not contain `PROFILE=buildroot-check`.

- [ ] **Step 3: Add the Profile definition file**

Create `mk/elf2_profiles.mk`:

```make
PROFILE ?= buildroot-check
ELF2_SDK_HOME ?= $(HOME)/.local/opt/elf2-sdk

ifeq ($(PROFILE),buildroot-check)
PROFILE_STRICT := 0
ELF2_SDK_ROOT ?= $(ELF2_SDK_HOME)/aarch64-buildroot-linux-gnu_sdk-buildroot
TARGET_TRIPLE ?= aarch64-buildroot-linux-gnu
TOOLCHAIN_BIN ?= $(ELF2_SDK_ROOT)/bin
CROSS_COMPILE ?= $(TOOLCHAIN_BIN)/$(TARGET_TRIPLE)-
SYSROOT ?= $(ELF2_SDK_ROOT)/$(TARGET_TRIPLE)/sysroot
else ifeq ($(PROFILE),jammy-deploy)
PROFILE_STRICT := 1
TOOLCHAIN_ROOT ?= $(ELF2_SDK_HOME)/toolchains/gcc-arm-10.3-aarch64
TOOLCHAIN_BIN ?= $(TOOLCHAIN_ROOT)/bin
CROSS_COMPILE ?= $(TOOLCHAIN_BIN)/aarch64-none-linux-gnu-
SYSROOT ?= $(ELF2_SDK_HOME)/profiles/jammy-deploy/sysroot
else
$(error unknown ELF2 profile: $(PROFILE))
endif
```

- [ ] **Step 4: Wire the Makefile to the Profile file**

Replace the current hard-coded SDK block with:

```make
ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
include $(ROOT_DIR)/mk/elf2_profiles.mk

CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
```

Extend the library search order and feature variables:

```make
USR_LOCAL_LIB_DIR := $(SYSROOT)/usr/local/lib
MULTIARCH_LIB_DIR := $(SYSROOT)/usr/lib/aarch64-linux-gnu
ROOT_MULTIARCH_LIB_DIR := $(SYSROOT)/lib/aarch64-linux-gnu
USR_LIB_DIR := $(SYSROOT)/usr/lib
LIB_SEARCH_DIRS := $(strip \
    $(if $(wildcard $(USR_LOCAL_LIB_DIR)),$(USR_LOCAL_LIB_DIR)) \
    $(if $(wildcard $(MULTIARCH_LIB_DIR)),$(MULTIARCH_LIB_DIR)) \
    $(if $(wildcard $(ROOT_MULTIARCH_LIB_DIR)),$(ROOT_MULTIARCH_LIB_DIR)) \
    $(if $(wildcard $(USR_LIB_DIR)),$(USR_LIB_DIR)))

MPP_LIB := $(call find_lib,librockchip_mpp.so)
FFMPEG_ABI := $(shell if test -r "$(SYSROOT_INC)/libavcodec/version_major.h"; then \
    sed -n 's/^#define LIBAVCODEC_VERSION_MAJOR[[:space:]]*//p' \
    "$(SYSROOT_INC)/libavcodec/version_major.h"; fi)
```

Add these lines to `print-config`:

```make
	@echo "PROFILE=$(PROFILE)"
	@echo "PROFILE_STRICT=$(PROFILE_STRICT)"
	@echo "MPP_LIB=$(MPP_LIB)"
	@echo "FFMPEG_ABI=$(FFMPEG_ABI)"
```

Add `bash tests/test_profile_config.sh` to the `test` target.

- [ ] **Step 5: Run Profile and existing tests**

Run:

```bash
bash tests/test_profile_config.sh
make test
```

Expected: PASS; existing Buildroot checks still emit their three documented warnings.

- [ ] **Step 6: Commit**

```bash
git add rk3588/mk/elf2_profiles.mk rk3588/Makefile rk3588/tests/test_profile_config.sh
git commit -m "build: add explicit ELF2 SDK profiles"
```

### Task 2: Add the deterministic build wrapper

**Files:**
- Create: `rk3588/scripts/lib/elf2_common.sh`
- Create: `rk3588/scripts/build_elf2.sh`
- Create: `rk3588/tests/test_build_wrapper.sh`
- Modify: `rk3588/Makefile:110-113`

- [ ] **Step 1: Write the failing PATH-cleaning test**

Create `tests/test_build_wrapper.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

cat >"$tmp/fake-make" <<'EOF'
#!/usr/bin/env bash
printf 'PATH=%s\nARGS=%s\n' "$PATH" "$*" >>"$BUILD_WRAPPER_LOG"
EOF
chmod +x "$tmp/fake-make"

BUILD_WRAPPER_LOG="$tmp/log" MAKE_BIN="$tmp/fake-make" \
PATH="/mnt/d/anaconda/Library/bin:/usr/bin:/bin" \
bash "$project_dir/scripts/build_elf2.sh" --profile jammy-deploy --jobs 3

! grep -Fq '/mnt/' "$tmp/log"
grep -Fq 'PROFILE=jammy-deploy clean' "$tmp/log"
grep -Fq 'PROFILE=jammy-deploy -j3 all' "$tmp/log"

echo 'build wrapper sanitizes PATH and performs a clean build'
```

- [ ] **Step 2: Run the test and verify RED**

Run `bash tests/test_build_wrapper.sh`.

Expected: FAIL because `scripts/build_elf2.sh` does not exist.

- [ ] **Step 3: Create the shared shell library**

Create `scripts/lib/elf2_common.sh`:

```bash
#!/usr/bin/env bash

elf2_die() {
    echo "error: $*" >&2
    return 1
}

elf2_sanitize_path() {
    export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
}

elf2_require_file() {
    [[ -f "$1" ]] || elf2_die "required file is missing: $1"
}

elf2_require_dir() {
    [[ -d "$1" ]] || elf2_die "required directory is missing: $1"
}

elf2_assert_safe_staging_dir() {
    case "$1" in
        /tmp/elf2-*) return 0 ;;
        *) elf2_die "unsafe staging directory: $1" ;;
    esac
}

elf2_sha256() {
    sha256sum "$1" | awk '{print $1}'
}
```

- [ ] **Step 4: Create the wrapper**

Create `scripts/build_elf2.sh` with argument parsing for exactly `--profile`, `--jobs`, `--check`, and help. After parsing:

```bash
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_dir/scripts/lib/elf2_common.sh"
elf2_sanitize_path

make_bin=${MAKE_BIN:-make}
"$make_bin" -C "$project_dir" PROFILE="$profile" clean
if ((check_only)); then
    exec "$make_bin" -C "$project_dir" PROFILE="$profile" check-env
fi
exec "$make_bin" -C "$project_dir" PROFILE="$profile" -j"$jobs" all
```

Reject profiles other than `buildroot-check` and `jammy-deploy` before invoking Make.

- [ ] **Step 5: Run the wrapper test and Buildroot smoke build**

```bash
bash tests/test_build_wrapper.sh
./scripts/build_elf2.sh --profile buildroot-check --jobs 8
file main
```

Expected: test PASS; `main` is an AArch64 ELF.

- [ ] **Step 6: Add the wrapper test to `make test` and commit**

```bash
git add rk3588/scripts/lib/elf2_common.sh rk3588/scripts/build_elf2.sh \
  rk3588/tests/test_build_wrapper.sh rk3588/Makefile
git commit -m "build: add deterministic ELF2 build wrapper"
```

### Task 3: Preserve the minimal Rockchip unpack tools

**Files:**
- Create: `rk3588/scripts/capture_rockchip_tools.sh`
- Create: `rk3588/tests/test_capture_rockchip_tools.sh`
- Modify: `rk3588/Makefile`

- [ ] **Step 1: Write the failing tool-capture test**

The test creates a fake `guestfish` that copies three executable fixtures into the requested destination. It invokes:

```bash
GUESTFISH_BIN="$tmp/fake-guestfish" \
bash scripts/capture_rockchip_tools.sh \
  --vdi "$tmp/ubuntu1.vdi" \
  --output-dir "$tmp/install"
```

Assert:

```bash
test -x "$tmp/install/rockdev/afptool"
test -x "$tmp/install/rockdev/rkImageMaker"
test -x "$tmp/install/rockdev/unpack.sh"
grep -Fq 'afptool' "$tmp/install/manifest.sha256"
grep -Fq 'rkImageMaker' "$tmp/install/manifest.sha256"
```

- [ ] **Step 2: Run the test and verify RED**

Expected: FAIL because `capture_rockchip_tools.sh` does not exist.

- [ ] **Step 3: Implement read-only capture**

The script defaults to:

```bash
vdi=/mnt/e/VirtualBox/VMS/ubuntu1/ubuntu1.vdi
guest_dir=/home/c/linux/ELF2/work/ELF2-linux-source/tools/linux/Linux_Pack_Firmware/rockdev
output_dir=$HOME/.local/opt/elf2-sdk/sources/rockdev-tools
guestfish_bin=${GUESTFISH_BIN:-guestfish}
```

Use a validated `/tmp/elf2-rockdev.*` staging directory and this read-only operation:

```bash
"$guestfish_bin" --ro -a "$vdi" -i copy-out "$guest_dir" "$stage"
```

Validate `afptool`, `rkImageMaker`, and `unpack.sh`; hash the complete copied directory; then atomically rename the stage into `output_dir`. Refuse to overwrite an existing directory whose manifest does not match.

- [ ] **Step 4: Run the unit test**

Run `bash tests/test_capture_rockchip_tools.sh`.

Expected: PASS.

- [ ] **Step 5: Capture and verify the real tools**

Run as WSL root because `supermin` cannot read `/boot/vmlinuz-*` as the normal user:

```powershell
wsl.exe -d Ubuntu-22.04 -u root -- bash -lc '
  cd /home/chx/Linux/qiansai/rk3588 &&
  ./scripts/capture_rockchip_tools.sh \
    --vdi /mnt/e/VirtualBox/VMS/ubuntu1/ubuntu1.vdi \
    --output-dir /home/chx/.local/opt/elf2-sdk/sources/rockdev-tools &&
  chown -R chx:chx /home/chx/.local/opt/elf2-sdk/sources/rockdev-tools
'
```

Expected: both tools are x86-64 ELF executables and the manifest verifies with `sha256sum -c`.

- [ ] **Step 6: Commit**

```bash
git add rk3588/scripts/capture_rockchip_tools.sh \
  rk3588/tests/test_capture_rockchip_tools.sh rk3588/Makefile
git commit -m "build: preserve Rockchip firmware unpack tools"
```

### Task 4: Assemble the base Jammy sysroot from the Desktop image

**Files:**
- Create: `rk3588/scripts/setup_elf2_jammy_sdk.sh`
- Create: `rk3588/tests/test_jammy_setup.sh`
- Modify: `rk3588/Makefile`

- [ ] **Step 1: Write the failing synthetic-image test**

`tests/test_jammy_setup.sh` must:

1. Create a 64 MiB ext4 fixture using `truncate` and `mkfs.ext4 -F -d`.
2. Put fixture files at `/etc/os-release`, `/lib/aarch64-linux-gnu/libc.so.6`, `/usr/lib/aarch64-linux-gnu/librknnrt.so`, and `/var/lib/dpkg/status`.
3. Provide fake `rkImageMaker` and `afptool` scripts; the second copies the ext4 fixture to `rootfs.img`.
4. Provide a fake GCC sysroot containing `usr/include/stdio.h` and `usr/lib/crt1.o`.
5. Run the setup script with explicit fixture paths.
6. Assert that the resulting sysroot contains both the GCC development files and the rootfs runtime files, plus `manifests/base-inputs.sha256`.

Run:

```bash
bash tests/test_jammy_setup.sh
```

Expected: FAIL because the setup script does not exist.

- [ ] **Step 2: Implement argument and input validation**

Support these options:

```text
--image PATH
--md5-file PATH
--tools-dir PATH
--toolchain-source PATH
--toolchain-root PATH
--install-root PATH
--check
```

Defaults:

```bash
image='/mnt/f/ELF 2开发板资料包/03-镜像/00-eMMC镜像/Desktop镜像/update.img'
md5_file='/mnt/f/ELF 2开发板资料包/03-镜像/00-eMMC镜像/Desktop镜像/MD5.txt'
tools_dir="$HOME/.local/opt/elf2-sdk/sources/rockdev-tools/rockdev"
toolchain_source='/home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/gcc-arm-10.3-aarch64-toolchain'
toolchain_root="$HOME/.local/opt/elf2-sdk/toolchains/gcc-arm-10.3-aarch64"
install_root="$HOME/.local/opt/elf2-sdk/profiles/jammy-deploy"
```

Compare the first field of `MD5.txt` with `md5sum "$image"`; require the source GCC compiler and its `aarch64-none-linux-gnu/libc` directory. If `toolchain_root` does not exist, copy `toolchain_source` into a sibling staging directory, verify `aarch64-none-linux-gnu-gcc --version`, and atomically rename it to `toolchain_root`. If it already exists, verify it and do not overwrite it. Record the source archive hash `8b79c0ba3d0f83d58200c9d0b96f253bbaeab1d03e3851701d082903720c1545` in the Jammy manifest.

- [ ] **Step 3: Implement update image extraction**

Use noninteractive commands rather than vendor `unpack.sh`:

```bash
"$tools_dir/rkImageMaker" -unpack "$image" "$stage/outer"
"$tools_dir/afptool" -unpack "$stage/outer/firmware.img" "$stage/parts"
```

Accept exactly one regular file named `rootfs.img` or `jammy-rootfs.img`. Validate it with `file` and `dumpe2fs -h`; fail instead of guessing if the format is not ext4.

- [ ] **Step 4: Implement base sysroot assembly**

1. Copy the GCC sysroot into `$stage/sysroot` using `cp -a`.
2. Use `debugfs -R 'rdump ...'` to overlay these rootfs paths when present:

```text
/lib/aarch64-linux-gnu
/usr/lib/aarch64-linux-gnu
/usr/local/lib
/usr/local/include
```

3. Dump `/etc/os-release` and `/var/lib/dpkg/status` into the manifest directory.
4. Record image MD5, tool SHA-256, compiler version, rootfs UUID, OS release and every copied input in `manifests/base-inputs.sha256` and `manifests/base-environment.txt`.
5. Atomically rename the completed stage to `install_root/sysroot`; never mutate an existing validated sysroot in place.

- [ ] **Step 5: Run the synthetic test and shell syntax check**

```bash
bash -n scripts/setup_elf2_jammy_sdk.sh
bash tests/test_jammy_setup.sh
```

Expected: PASS.

- [ ] **Step 6: Run `--check` against real inputs**

```bash
./scripts/setup_elf2_jammy_sdk.sh --check
```

Expected: Desktop MD5, unpack tools and GCC toolchain all report OK; no 7.9 GiB extraction happens in check mode.

- [ ] **Step 7: Commit**

```bash
git add rk3588/scripts/setup_elf2_jammy_sdk.sh \
  rk3588/tests/test_jammy_setup.sh rk3588/Makefile
git commit -m "build: assemble ELF2 Jammy base sysroot"
```

### Task 5: Install Jammy development overlays

**Files:**
- Create: `rk3588/scripts/install_elf2_jammy_deps.sh`
- Create: `rk3588/tests/test_jammy_deps.sh`
- Modify: `rk3588/Makefile`

- [ ] **Step 1: Write the failing dependency installer test**

Build minimal arm64 fixture packages with `dpkg-deb --build` for:

```text
libpaho-mqtt-dev, libpaho-mqtt1.3
libmodbus-dev, libmodbus5
libcurl4-openssl-dev, libcurl4
libsqlite3-dev, libsqlite3-0
```

Create a synthetic sysroot with versioned RKNN, RGA, MPP, DRM and FreeType runtime libraries. Run the installer and assert:

```bash
test -f "$sysroot/usr/include/MQTTClient.h"
test -f "$sysroot/usr/include/modbus/modbus.h"
test -L "$sysroot/usr/lib/aarch64-linux-gnu/libpaho-mqtt3c.so"
test -L "$sysroot/usr/lib/aarch64-linux-gnu/libmodbus.so"
test -f "$sysroot/usr/local/include/rockchip/rk_mpi.h"
test -L "$sysroot/usr/local/lib/librockchip_mpp.so"
test -f "$sysroot/usr/include/rknn/rknn_api.h"
test -f "$sysroot/usr/include/rga/im2d.h"
```

- [ ] **Step 2: Run the test and verify RED**

Expected: FAIL because `install_elf2_jammy_deps.sh` does not exist.

- [ ] **Step 3: Implement Debian package installation**

Default package source:

```bash
package_dir=/home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/Project/camera_flow/packages/arm64
```

For each required package glob, require exactly one file, verify `Architecture: arm64` with `dpkg-deb -f`, record SHA-256, and extract into a staging overlay with `dpkg-deb -x`. Reject amd64 or architecture-independent replacements for the required native libraries.

- [ ] **Step 4: Install vendor headers without replacing runtime libraries**

Sources:

```text
MPP headers:
  /home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/mpp-include
MPP pkg-config:
  /home/chx/migrations/ubuntu1-elf2-2026-10-29/extracted/usr-local-lib/pkgconfig/rockchip_mpp.pc
RKNN/RGA/DRM headers:
  ~/.local/opt/elf2-sdk/aarch64-buildroot-linux-gnu_sdk-buildroot/
  aarch64-buildroot-linux-gnu/sysroot/usr/include
FreeType headers:
  rk3588/third_party/elf2_sysroot/usr/include/freetype2
```

Require the Jammy rootfs to already contain versioned `librknnrt`, `librga`, `librockchip_mpp`, `libdrm`, and `libfreetype` runtime libraries. Create only missing unversioned development symlinks, pointing to the existing versioned Jammy libraries. Do not copy Buildroot runtime `.so` files into the Jammy sysroot.

- [ ] **Step 5: Validate SONAMEs and write the overlay manifest**

Use `readelf -d` to require:

```text
libpaho-mqtt3c.so.1
libmodbus.so.5
librockchip_mpp.so.1
librga.so.2
```

Write package versions, header sources, Build IDs and SHA-256 values to `profiles/jammy-deploy/manifests/development-overlay.txt`.

- [ ] **Step 6: Run the test and commit**

```bash
bash tests/test_jammy_deps.sh
git add rk3588/scripts/install_elf2_jammy_deps.sh \
  rk3588/tests/test_jammy_deps.sh rk3588/Makefile
git commit -m "build: install Jammy deployment dependencies"
```

### Task 6: Build the pinned minimal FFmpeg-Rockchip 6

**Files:**
- Create: `rk3588/scripts/build_ffmpeg6_rkmpp.sh`
- Create: `rk3588/tests/test_ffmpeg6_rkmpp_config.sh`
- Modify: `rk3588/Makefile`

- [ ] **Step 1: Write the failing configuration contract test**

Run the future script in print-only mode:

```bash
config=$(bash scripts/build_ffmpeg6_rkmpp.sh --print-config \
  --sysroot /fixture/sysroot --toolchain-root /fixture/toolchain)
```

Assert the full commit, archive SHA-256 and these flags:

```text
--enable-cross-compile
--enable-shared
--disable-static
--disable-programs
--disable-doc
--disable-autodetect
--disable-everything
--enable-rkmpp
--enable-libdrm
--enable-encoder=h264_rkmpp
--enable-encoder=aac
--enable-muxer=flv
--enable-muxer=mpegts
--enable-protocol=file
--enable-protocol=pipe
--enable-protocol=tcp
--enable-protocol=rtmp
```

- [ ] **Step 2: Run the test and verify RED**

Expected: FAIL because `build_ffmpeg6_rkmpp.sh` does not exist.

- [ ] **Step 3: Implement pinned source acquisition**

Use:

```bash
source_url='https://github.com/nyanmisaka/ffmpeg-rockchip/archive/99d938d6ea32c31457bb68a5a2177c94d13a4854.tar.gz'
source_sha256='0779552be0813989764275e411a85c14d68efffca1d3e98641e3d4bfb5ac8bad'
```

Download to `~/.local/opt/elf2-sdk/sources`, verify the SHA before extraction, and refuse a mismatched cached archive. Do not use the upstream `ffmpeg-6.0.tar.xz` for this build.

- [ ] **Step 4: Implement the minimal out-of-tree configuration**

Set:

```bash
export PKG_CONFIG_SYSROOT_DIR="$sysroot"
export PKG_CONFIG_LIBDIR="$sysroot/usr/local/lib/pkgconfig:$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/lib/pkgconfig"
export PKG_CONFIG_PATH=
```

Configure with the flags asserted by the test plus:

```text
--prefix=/usr/local
--libdir=/usr/local/lib
--incdir=/usr/local/include
--arch=aarch64
--target-os=linux
--cross-prefix=<toolchain>/bin/aarch64-none-linux-gnu-
--sysroot=<jammy sysroot>
--enable-network
--enable-swresample
--enable-parser=h264
--enable-parser=aac
--enable-bsf=h264_mp4toannexb
--enable-bsf=aac_adtstoasc
```

Do not enable RKRGA filters in this minimal build; the application already calls RGA directly, while `h264_rkmpp` only requires RKMPP and libdrm.

- [ ] **Step 5: Build into staging and verify components before install**

```bash
make -j"$jobs"
grep -Fxq '#define CONFIG_H264_RKMPP_ENCODER 1' config_components.h
grep -Fxq '#define CONFIG_AAC_ENCODER 1' config_components.h
make DESTDIR="$stage" install
```

Require staged SONAMEs `libavcodec.so.60`, `libavformat.so.60`, `libavutil.so.58`, and `libswresample.so.4`. Install the staged `/usr/local` tree atomically into the Jammy sysroot and write source/configuration hashes to `manifests/ffmpeg-rockchip.txt`. Generate `manifests/ffmpeg-rockchip-components.txt` from the completed build with exactly these required lines:

```text
CONFIG_H264_RKMPP_ENCODER=1
CONFIG_AAC_ENCODER=1
CONFIG_FLV_MUXER=1
CONFIG_MPEGTS_MUXER=1
CONFIG_RTMP_PROTOCOL=1
```

Create the file by reading `config_components.h`; fail if any macro is missing or not equal to `1`.

- [ ] **Step 6: Run the contract test and a real configure**

```bash
bash tests/test_ffmpeg6_rkmpp_config.sh
./scripts/build_ffmpeg6_rkmpp.sh --configure-only
```

Expected: contract PASS; real configure finds `rockchip_mpp` and `libdrm`, and enables both encoders.

- [ ] **Step 7: Commit**

```bash
git add rk3588/scripts/build_ffmpeg6_rkmpp.sh \
  rk3588/tests/test_ffmpeg6_rkmpp_config.sh rk3588/Makefile
git commit -m "build: add pinned FFmpeg Rockchip toolchain"
```

### Task 7: Make `jammy-deploy` fail closed

**Files:**
- Modify: `rk3588/Makefile:29-66,83-108`
- Modify: `rk3588/tests/test_profile_config.sh`
- Create: `rk3588/tests/test_jammy_strict_env.sh`

- [ ] **Step 1: Write strict-environment failure tests**

Create fixture sysroots that omit one dependency at a time. For every fixture, run:

```bash
make -s check-env PROFILE=jammy-deploy SYSROOT="$fixture" \
  CROSS_COMPILE="$fake_toolchain/bin/aarch64-none-linux-gnu-"
```

Assert nonzero exit and exact messages for missing Paho, libmodbus, MPP, RKNN, RGA, FFmpeg ABI 60, and `h264_rkmpp_encoder` evidence.

- [ ] **Step 2: Run the tests and verify RED**

Expected: at least the Paho fixture incorrectly succeeds with only a warning.

- [ ] **Step 3: Replace string-only RKMPP detection**

The prior `strings ... | grep h264_rkmpp` check accepts decoder-only builds. For `jammy-deploy`, require all of:

```text
libavcodec/version_major.h declares 60
libavcodec.so has SONAME libavcodec.so.60
manifests/ffmpeg-rockchip.txt pins the approved commit
manifests/ffmpeg-rockchip-components.txt contains CONFIG_H264_RKMPP_ENCODER=1
manifests/ffmpeg-rockchip-components.txt contains CONFIG_AAC_ENCODER=1
```

Expose `RKMPP_ENCODER=available` only when these checks agree.

- [ ] **Step 4: Enforce strict checks in `check-env`**

When `PROFILE_STRICT=1`, return nonzero unless:

```text
PAHO_MQTT=enabled
LIBMODBUS=enabled
RKMPP_ENCODER=available
RKNN_LIB is present
RGA_LIB is present
MPP_LIB is present
FFMPEG_ABI=60
```

Keep the current warning behavior only for `buildroot-check`.

- [ ] **Step 5: Remove `--allow-shlib-undefined` from strict links**

Use:

```make
ifeq ($(PROFILE_STRICT),1)
LDFLAGS += -Wl,--no-allow-shlib-undefined
else
LDFLAGS += -Wl,--allow-shlib-undefined
endif
```

- [ ] **Step 6: Run both Profile tests and commit**

```bash
bash tests/test_profile_config.sh
bash tests/test_jammy_strict_env.sh
make test
git add rk3588/Makefile rk3588/tests/test_profile_config.sh \
  rk3588/tests/test_jammy_strict_env.sh
git commit -m "build: enforce strict Jammy deployment features"
```

### Task 8: Verify headers, linkability and the complete ELF dependency closure

**Files:**
- Create: `rk3588/scripts/check_elf_deps.sh`
- Create: `rk3588/scripts/verify_elf2_sdk.sh`
- Create: `rk3588/tests/test_elf_dependency_check.sh`
- Create: `rk3588/tests/probes/media_probe.c`
- Create: `rk3588/tests/probes/mqtt_probe.c`
- Create: `rk3588/tests/probes/modbus_probe.c`
- Create: `rk3588/tests/probes/rknn_rga_probe.cc`
- Modify: `rk3588/Makefile`

- [ ] **Step 1: Write the dependency-checker RED test**

Use the existing Buildroot cross compiler to create a tiny AArch64 shared library and executable in a temporary sysroot. First omit the shared library and assert:

```text
missing DT_NEEDED: libfixture.so.1
```

Then add the library and assert the checker returns zero. Also assert an x86-64 `/bin/true` input is rejected as the wrong machine.

- [ ] **Step 2: Implement recursive `DT_NEEDED` resolution**

`check_elf_deps.sh` accepts `--sysroot` and one or more ELF files. It must:

1. Confirm every input is AArch64 with `readelf -h`.
2. Parse `DT_NEEDED` with `readelf -d`.
3. Search only these sysroot locations:

```text
/usr/local/lib
/usr/lib/aarch64-linux-gnu
/lib/aarch64-linux-gnu
/usr/lib
/lib
```

4. Recurse through each resolved library using an associative-array visited set.
5. Report every missing SONAME and return nonzero.
6. Compare the highest required `GLIBC_*` version with the highest version exported by the sysroot `libc.so.6` using `sort -V`.

- [ ] **Step 3: Add real compile probes**

Each probe must call one real public API so it cannot pass on headers alone:

```c
/* media_probe.c */
avcodec_find_encoder_by_name("h264_rkmpp");
avcodec_find_encoder_by_name("aac");

/* mqtt_probe.c */
MQTTClient_create(NULL, "tcp://127.0.0.1:1883", "probe",
                  MQTTCLIENT_PERSISTENCE_NONE, NULL);

/* modbus_probe.c */
modbus_t *ctx = modbus_new_rtu("/dev/null", 115200, 'N', 8, 1);
modbus_free(ctx);
```

`rknn_rga_probe.cc` must call `rknn_query` and `imcheck` with zero-initialized structures; it is linked but never run on x86 WSL.

- [ ] **Step 4: Implement the top-level verifier**

`verify_elf2_sdk.sh --profile jammy-deploy` performs:

```bash
make -C "$project_dir" PROFILE=jammy-deploy check-env
make -C "$project_dir" PROFILE=jammy-deploy clean
make -C "$project_dir" PROFILE=jammy-deploy -j"$jobs" all
```

It cross-compiles all four probes, runs `file` and `readelf -d`, then calls `check_elf_deps.sh` for the probes and `main`. Require `main` to contain these direct dependencies:

```text
libavformat.so.60
libavcodec.so.60
libpaho-mqtt3c.so.1
libmodbus.so.5
librknnrt.so
librga.so.2
```

Finally verify the complete closure includes `librockchip_mpp.so.1`.

- [ ] **Step 5: Run tests and the real Jammy verifier**

```bash
bash tests/test_elf_dependency_check.sh
./scripts/verify_elf2_sdk.sh --profile jammy-deploy --jobs 8
```

Expected: unit test PASS; verifier exits zero and writes `profiles/jammy-deploy/manifests/verification.txt`.

- [ ] **Step 6: Commit**

```bash
git add rk3588/scripts/check_elf_deps.sh rk3588/scripts/verify_elf2_sdk.sh \
  rk3588/tests/test_elf_dependency_check.sh rk3588/tests/probes rk3588/Makefile
git commit -m "test: verify ELF2 Jammy dependency closure"
```

### Task 9: Document recovery, board validation and the VM deletion gate

**Files:**
- Modify: `rk3588/README.md:3-61`
- Create: `rk3588/docs/elf2-sdk-recovery.md`
- Create: `rk3588/docs/elf2-board-validation.md`
- Modify: `rk3588/Makefile`

- [ ] **Step 1: Rewrite the README environment section**

Document these commands exactly:

```bash
./scripts/setup_elf2_sdk.sh
./scripts/build_elf2.sh --profile buildroot-check --jobs 8

./scripts/capture_rockchip_tools.sh
./scripts/setup_elf2_jammy_sdk.sh
./scripts/install_elf2_jammy_deps.sh
./scripts/build_ffmpeg6_rkmpp.sh
./scripts/verify_elf2_sdk.sh --profile jammy-deploy --jobs 8
```

State that `buildroot-check` is not deployable and `jammy-deploy` is not runtime-validated until the board checklist passes.

- [ ] **Step 2: Write the recovery document**

`docs/elf2-sdk-recovery.md` must list:

- all fixed input paths and hashes;
- required host packages (`e2fsprogs`, `libguestfs-tools`, `qemu-utils`, `file`, `binutils`, `dpkg-dev`, `pkg-config`);
- expected disk usage before extraction;
- every setup command in execution order;
- manifest verification commands;
- safe cleanup paths limited to script-owned `/tmp/elf2-*` and rebuildable SDK build directories.

- [ ] **Step 3: Write the board validation checklist**

`docs/elf2-board-validation.md` must provide commands and expected evidence for:

```text
V4L2/DMA-BUF capture
RKNN inference
RGA conversion
h264_rkmpp encode and RTMP push
AAC encoder discovery and a short encoded sample
MQTT publish/reconnect
Modbus acquisition
network-loss local segment recording and recovery
```

End with a seven-item VM deletion gate matching the approved design. The document must explicitly say that the SDK scripts never delete a VM.

- [ ] **Step 4: Run the complete local verification**

```bash
make test
./scripts/build_elf2.sh --profile buildroot-check --jobs 8
./scripts/verify_elf2_sdk.sh --profile jammy-deploy --jobs 8
git diff --check
```

Expected:

- all shell tests pass;
- both builds produce AArch64 ELF files;
- Buildroot emits only the documented fallback warnings;
- Jammy emits no fallback warning and resolves its entire dependency closure;
- no whitespace errors.

- [ ] **Step 5: Verify repository scope and commit**

```bash
git status --short
git diff --stat
git add rk3588/README.md rk3588/docs/elf2-sdk-recovery.md \
  rk3588/docs/elf2-board-validation.md rk3588/Makefile
git commit -m "docs: document ELF2 SDK recovery and board validation"
```

Do not stage `INTERVIEW.md`, `TECHNICAL_DOC.md`, user source changes, generated binaries, sysroots, downloaded archives or manifests under `~/.local/opt`.

## Completion boundary

The host-side plan is complete when Task 9 local verification passes. At that point report two separate statuses:

1. `jammy-deploy` host environment: reproducible, fully linked and offline-verified.
2. Board runtime: pending until every item in `docs/elf2-board-validation.md` has evidence.

Keep `ubuntu1` powered off but present until the board runtime status is complete and the final recovery manifest has been copied outside the VM storage directory.
