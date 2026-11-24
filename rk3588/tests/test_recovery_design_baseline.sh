#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
authoritative_path=/Users/worldengine/Downloads/DMABUFF.md
design_doc=${RECOVERY_DESIGN_DOC:-$authoritative_path}
manifest="$repo_dir/docs/recovery/recovery-design-baseline.md"
expected_sha=62FE0D72ABB22E3CEC70ADAFED8EDE44D79EB00043A3A8AEE0E4CDEB7ADCEAF3

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

test -f "$manifest" || fail "baseline manifest is missing: $manifest"
test -f "$design_doc" || fail "frozen design is missing: $design_doc"

actual_sha=$( (sha256sum "$design_doc" 2>/dev/null || shasum -a 256 "$design_doc") | awk '{print toupper($1)}')
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
