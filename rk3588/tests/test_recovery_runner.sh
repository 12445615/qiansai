#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="$repo_dir/build/host/recovery/runner-contract"
runner="$build_dir/recovery_runner_contract"
cxx=${HOST_CXX:-g++}

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

mkdir -p "$build_dir"
"$cxx" -std=c++20 -Wall -Wextra -Wpedantic -Werror \
    -I"$repo_dir/tests/recovery" \
    "$repo_dir/tests/recovery/test_main.cc" \
    "$repo_dir/tests/recovery/fixtures/runner_contract_fixture.cc" \
    -o "$runner"

list_output=$($runner --list)
test "$(grep -c '^runner_contract_' <<<"$list_output")" -eq 5 ||
    fail "--list did not list every fixture exactly once"
if grep -Eq '^\[(PASS|FAIL)\]' <<<"$list_output"; then
    fail "--list executed a test"
fi

pass_output=$($runner --filter runner_contract_pass)
test "$(grep -c '^\[PASS\] runner_contract_pass$' <<<"$pass_output")" -eq 1 ||
    fail "filtered passing test did not emit exactly one PASS line"
test "$(grep -Ec '^\[(PASS|FAIL)\]' <<<"$pass_output")" -eq 1 ||
    fail "--filter executed an unselected test"

vector_output=$($runner --filter runner_contract_nonstreamable_pass)
test "$(grep -c '^\[PASS\] runner_contract_nonstreamable_pass$' <<<"$vector_output")" -eq 1 ||
    fail "non-streamable equality did not pass"

set +e
failure_output=$($runner --filter runner_contract_equality_failure 2>&1)
failure_status=$?
set -e
test "$failure_status" -ne 0 || fail "assertion failure returned success"
test "$(grep -Ec '^\[FAIL\] runner_contract_equality_failure:' <<<"$failure_output")" -eq 1 ||
    fail "failing test did not emit exactly one FAIL line"
test "$(grep -Ec '^\[(PASS|FAIL)\]' <<<"$failure_output")" -eq 1 ||
    fail "failure filter executed an unselected test"
grep -Eq 'runner_contract_fixture\.cc:[0-9]+:' <<<"$failure_output" ||
    fail "equality failure did not report file:line"

set +e
vector_failure_output=$($runner --filter runner_contract_vector_equality_failure 2>&1)
vector_failure_status=$?
set -e
test "$vector_failure_status" -ne 0 || fail "non-streamable equality failure returned success"
test "$(grep -Ec '^\[FAIL\] runner_contract_vector_equality_failure:' \
    <<<"$vector_failure_output")" -eq 1 ||
    fail "non-streamable equality failure did not emit exactly one FAIL line"
grep -Eq 'runner_contract_fixture\.cc:[0-9]+:' <<<"$vector_failure_output" ||
    fail "non-streamable equality failure did not report file:line"
grep -Fq 'std::vector<int>({1, 2}) == std::vector<int>({2, 1})' \
    <<<"$vector_failure_output" ||
    fail "non-streamable equality failure omitted its expressions"

echo "PASS: recovery runner contract"
