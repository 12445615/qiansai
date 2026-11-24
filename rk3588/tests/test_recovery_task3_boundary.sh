#!/usr/bin/env bash
set -euo pipefail

root_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
files=(
    recovery/include/recovery/compiler.hpp
    recovery/include/recovery/canonical.hpp
    recovery/include/recovery/types.hpp
    recovery/include/recovery/program.hpp
    recovery/include/recovery/crypto.hpp
    recovery/src/compiler.cc
    recovery/src/canonical.cc
    recovery/src/crypto_openssl.cc
)
pattern='OwnedFd|OwnedProcessExitBinding|pidfd_open|waitid|poll\(|<sys/pidfd\.h>|<sys/wait\.h>'
matches=0
for file in "${files[@]}"; do
    if output=$(grep -nE "${pattern}" "${root_dir}/${file}"); then
        while IFS= read -r line; do
            printf '%s:%s\n' "${file}" "${line}" >&2
        done <<<"${output}"
        matches=1
    fi
done
if (( matches != 0 )); then
    echo "FAIL: Task 3 fd/runtime boundary violated" >&2
    exit 1
fi
echo "PASS: Task 3 compiler/canonical boundary has zero fd/runtime symbols"
