#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cxx=${HOST_CXX:-g++}
flags=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -I"${root_dir}/recovery/include")
work_dir="${root_dir}/build/host/recovery/compile_fail"
mkdir -p "${work_dir}"

expect_failure() {
    local source=$1
    local pattern=$2
    local log="${work_dir}/$(basename "${source}").log"
    if "${cxx}" "${flags[@]}" -c "${source}" -o "${work_dir}/forbidden.o" >"${log}" 2>&1; then
        echo "FAIL: expected compilation failure: ${source}" >&2
        return 1
    fi
    if ! grep -Eiq "${pattern}" "${log}"; then
        echo "FAIL: compilation failed for an unexpected reason: ${source}" >&2
        sed -n '1,120p' "${log}" >&2
        return 1
    fi
    echo "PASS: expected compile-time rejection: $(basename "${source}")"
}

expect_failure "${root_dir}/tests/recovery/compile_fail/permit_move_assign.cc" 'deleted.*operator=|operator=.*deleted|use of deleted function'
expect_failure "${root_dir}/tests/recovery/compile_fail/cross_type_predicate.cc" 'no matching function|no matching constructor|cannot convert|could not convert'
expect_failure "${root_dir}/tests/recovery/compile_fail/variant_too_many_alternatives.cc" 'canonical variant index exceeds one byte'
expect_failure "${root_dir}/tests/recovery/compile_fail/permit_test_access_forge.cc" 'PermitHandle.*private|is private within this context'
