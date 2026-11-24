#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

make -s -C "$project_dir" check-env
make -s -B -C "$project_dir" aliyun_mqtt.o encoder.o main.o

echo "optional dependency fallbacks compile"
