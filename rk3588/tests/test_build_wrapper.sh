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
