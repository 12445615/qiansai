#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cxx=${HOST_CXX:-g++}
flags=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -I"${root_dir}/recovery/include")
work_dir="${root_dir}/build/host/recovery/compile_fail"
mkdir -p "${work_dir}"

expect_failure() {
    local source=$1
    local pattern
    pattern=$(sed -n 's@^// EXPECT-DIAG: @@p' "${source}")
    if [[ -z "${pattern}" ]]; then
        echo "FAIL: missing EXPECT-DIAG: ${source}" >&2
        return 1
    fi
    local log="${work_dir}/$(basename "${source}").log"
    if "${cxx}" "${flags[@]}" -c "${source}" -o "${work_dir}/forbidden.o" >"${log}" 2>&1; then
        echo "FAIL: expected compilation failure: ${source}" >&2
        return 1
    fi
    local first_error
    first_error=$(grep -m1 -E 'error:' "${log}" || true)
    if [[ -z "${first_error}" ]] || ! grep -Eiq "${pattern}" <<<"${first_error}"; then
        echo "FAIL: compilation failed for an unexpected reason: ${source}" >&2
        sed -n '1,120p' "${log}" >&2
        return 1
    fi
    echo "PASS: expected compile-time rejection: $(basename "${source}")"
}

for source in "${root_dir}"/tests/recovery/compile_fail/*.cc; do
    expect_failure "${source}"
done
