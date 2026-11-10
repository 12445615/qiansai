#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
authoritative_path=/mnt/e/Obsidian_warehouse/嵌入式/1-Project/RK3588工业安全监测/2026-11-02-DMA-BUF恢复与证明内核设计.md
design_doc=${RECOVERY_DESIGN_DOC:-$authoritative_path}
manifest="$repo_dir/docs/recovery/recovery-design-baseline.md"
expected_sha=8B7C5DFD8AE8753CA05A06D130EC995735AB2B89B068861745B14BF5A265D9A8

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

test -f "$manifest" || fail "baseline manifest is missing: $manifest"
test -f "$design_doc" || fail "frozen design is missing: $design_doc"

actual_sha=$(sha256sum "$design_doc" | awk '{print toupper($1)}')
test "$actual_sha" = "$expected_sha" || fail "frozen design SHA-256 changed: $actual_sha"

grep -Fqx -- "- Authoritative path: \`$authoritative_path\`" "$manifest" ||
    fail "manifest does not freeze the authoritative path"
grep -Fqx -- "- SHA-256: \`$expected_sha\`" "$manifest" ||
    fail "manifest does not freeze the expected SHA-256"

for heading in \
    '## 18. 反向测试矩阵' \
    '## 19. 真机 capability 待验证清单' \
    '## 20. 编译闭合检查清单'
do
    awk -v heading="$heading" 'index($0, heading) == 1 { found = 1 } END { exit !found }' \
        "$design_doc" || fail "required heading is missing: $heading"
    grep -Fqx -- "- Required heading: \`$heading\`" "$manifest" ||
        fail "manifest does not freeze required heading: $heading"
done

echo "PASS: recovery design baseline"
