#!/usr/bin/env bash
set -euo pipefail

# 切换到项目根目录
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

cmake -B build-musl -DQJSA_LIBC=musl
cmake --build build-musl -j --target app-linux

