#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build_elf2.sh --profile PROFILE [--jobs N] [--check]

Profiles: buildroot-check, jammy-deploy
EOF
}

profile=buildroot-check
jobs=1
check_only=0
while (($#)); do
    case "$1" in
        --profile)
            (($# >= 2)) || { echo 'error: --profile requires a value' >&2; exit 2; }
            profile=$2
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || { echo 'error: --jobs requires a value' >&2; exit 2; }
            [[ $2 =~ ^[1-9][0-9]*$ ]] || { echo 'error: --jobs must be a positive integer' >&2; exit 2; }
            jobs=$2
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
            echo "error: unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "$profile" in
    buildroot-check|jammy-deploy) ;;
    *) echo "error: unknown ELF2 profile: $profile" >&2; exit 2 ;;
esac

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$project_dir/scripts/lib/elf2_common.sh"
elf2_sanitize_path

make_bin=${MAKE_BIN:-make}
"$make_bin" -C "$project_dir" PROFILE="$profile" clean
if ((check_only)); then
    exec "$make_bin" -C "$project_dir" PROFILE="$profile" check-env
fi
exec "$make_bin" -C "$project_dir" PROFILE="$profile" -j"$jobs" all
