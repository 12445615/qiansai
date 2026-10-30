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
