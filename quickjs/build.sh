#!/usr/bin/env bash
set -euo pipefail

# 切换到项目根目录
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# Linux 用静态 musl（原来的默认产物）；macOS 用系统 clang 和 libSystem。
case "$(uname -s)" in
    Darwin)
        cmake -B build-macos
        cmake --build build-macos -j --target app-macos
        echo "外壳产物: $ROOT/build-macos/app-macos"
        ;;
    *)
        cmake -B build-musl -DQJSA_LIBC=musl
        cmake --build build-musl -j --target app-linux
        echo "外壳产物: $ROOT/build-musl/app-linux"
        ;;
esac
