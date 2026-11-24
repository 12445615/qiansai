#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
main_c="$repo_root/rk3588/main.c"
encoder_c="$repo_root/rk3588/encoder.c"

assert_line() {
    local pattern="$1"
    local file="$2"
    local message="$3"
    if ! grep -Eq "$pattern" "$file"; then
        printf 'FAIL: %s\n' "$message" >&2
        return 1
    fi
}

assert_absent() {
    local pattern="$1"
    local file="$2"
    local message="$3"
    if grep -Eq "$pattern" "$file"; then
        printf 'FAIL: %s\n' "$message" >&2
        return 1
    fi
}

assert_line '^#define[[:space:]]+WIDTH[[:space:]]+1920([[:space:]]|$)' "$main_c" \
    'main.c must use a 1920-pixel capture width'
assert_line '^#define[[:space:]]+HEIGHT[[:space:]]+1080([[:space:]]|$)' "$main_c" \
    'main.c must use a 1080-pixel capture height'
assert_line '^#define[[:space:]]+STREAM_FPS_DEFAULT[[:space:]]+60([[:space:]]|$)' "$main_c" \
    'the fallback stream/encoder frame rate must be 60 fps'
assert_line '^#define[[:space:]]+RTMP_URL[[:space:]]+"rtmp://192\.168\.1\.101:1935/live/test"' "$main_c" \
    'the deployment RTMP endpoint must target the WSL relay'
assert_line 'avcodec_find_encoder_by_name\("h264_rkmpp"\)' "$encoder_c" \
    'the encoder must explicitly request h264_rkmpp'

assert_absent '^#define[[:space:]]+WIDTH[[:space:]]+1280([[:space:]]|$)' "$main_c" \
    'the legacy 1280-pixel width must not remain active'
assert_absent '^#define[[:space:]]+HEIGHT[[:space:]]+720([[:space:]]|$)' "$main_c" \
    'the legacy 720-pixel height must not remain active'
assert_absent '^#define[[:space:]]+STREAM_FPS_DEFAULT[[:space:]]+30([[:space:]]|$)' "$main_c" \
    'the legacy 30 fps fallback must not remain active'

printf 'PASS: RK3588 source contract is 1920x1080 at 60 fps using h264_rkmpp\n'
