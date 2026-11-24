#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
fragment="$repo_dir/mk/recovery.mk"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

test -f "$fragment" || fail "recovery Make fragment is missing: $fragment"

config=$(make -s -C "$repo_dir" recovery-print-config) ||
    fail "recovery-print-config target is unavailable"
grep -Fqx 'RECOVERY_CXX_STANDARD=c++20' <<<"$config" ||
    fail "recovery-print-config did not report C++20"
grep -Fqx "RECOVERY_BUILD_DIR=$repo_dir/build/host/recovery" <<<"$config" ||
    fail "recovery outputs are not rooted below build/host/recovery"

dry_run=$(make -Bn -C "$repo_dir" recovery-test) ||
    fail "recovery-test target is unavailable"
grep -Fq -- '-std=c++20' <<<"$dry_run" ||
    fail "recovery-test compile command does not use C++20"
grep -Fq -- '-MMD' <<<"$dry_run" || fail "recovery compile does not generate dependencies"
grep -Fq -- '-MP' <<<"$dry_run" || fail "recovery compile lacks phony dependency targets"
if grep -Eq -- '(^|[[:space:]])-std=c\+\+11([[:space:]]|$)' <<<"$dry_run"; then
    fail "recovery-test inherited the application C++11 standard"
fi

if grep -Eq '\$\((CXXFLAGS)\)|\$\{CXXFLAGS\}' "$fragment"; then
    fail "recovery Make fragment references root CXXFLAGS"
fi

while IFS= read -r output; do
    case "$output" in
        "$repo_dir"/build/host/recovery/*) ;;
        *) fail "recovery artifact escapes build/host/recovery: $output" ;;
    esac
done < <(awk '{ for (i = 1; i < NF; ++i) if ($i == "-o") print $(i + 1) }' <<<"$dry_run")

grep -Fq -- '-include $(RECOVERY_TEST_DEPFILES)' "$fragment" ||
    fail "recovery dependency files are not included"

make -s -C "$repo_dir" recovery-test >/dev/null || fail "recovery-test execution failed"
for artifact in recovery_tests test_main.o test_main.d test_smoke.o test_smoke.d; do
    test -f "$repo_dir/build/host/recovery/$artifact" ||
        fail "expected recovery artifact is missing: $artifact"
done

echo "PASS: isolated recovery build contract"
