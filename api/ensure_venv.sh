#!/usr/bin/env bash
# 确保 Python 后端有一个可用的虚拟环境，并把解释器路径打印到 stdout（最后一行）。
#
# 这里只处理「环境」：查找/创建 venv、安装 requirements.txt。真正的启动逻辑在
# run.sh（打包产物）或 run.py（源码树）里，避免职责混在一起。
#
# 目录选择：
#   api/.venv 已经存在（源码树开发）        → 直接用它，不做任何安装
#   VBLIS_VENV_DIR 有设置                   → 用它
#   否则                                    → $VBLIS_DATA_DIR/venv
#       Linux:  ~/.local/share/vblis/venv（尊重 XDG_DATA_HOME）
#       macOS:  ~/Library/Application Support/VBLIS/venv
#
# 平台差异：
#   Linux  torch 从 download.pytorch.org 的 cu130 索引装（CPU 机器也能用）；
#           requirements.txt / constraints.txt 原样使用。
#   macOS  torch 用 PyPI 轮子（自带 Metal/MPS 后端），并把 requirements.txt 里的
#           `--extra-index-url` 与 constraints.txt 里的 CUDA/Windows 专用 pin 过滤掉，
#           否则 pip 会因为找不到 mac 轮子而失败。
#
# 可用环境变量：
#   VBLIS_VENV_DIR          指定 venv 目录
#   VBLIS_SYSTEM_PY         指定用来创建 venv 的系统解释器（默认自动找 python3.11–3.14）
#   VBLIS_TORCH_SPEC        torch 的 pip 规格（默认 torch>=2.14.1，CPU 机器同样可用）
#   VBLIS_TORCH_INDEX       torch 的索引地址（默认 Linux cu130；macOS 默认空＝PyPI）
#   VBLIS_NO_VENV_INSTALL=1 禁止自动安装；环境不可用时直接报错退出
set -euo pipefail

API_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNAME_S="$(uname -s)"
UNAME_M="$(uname -m)"
IS_MAC=0
[ "$UNAME_S" = "Darwin" ] && IS_MAC=1

REQ="$API_DIR/requirements.txt"
CONSTRAINTS="$API_DIR/constraints.txt"

if [ "$IS_MAC" = "1" ]; then
    DATA_DIR="${VBLIS_DATA_DIR:-$HOME/Library/Application Support/VBLIS}"
    DEFAULT_TORCH_INDEX=""
else
    DATA_DIR="${VBLIS_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/vblis}"
    DEFAULT_TORCH_INDEX="https://download.pytorch.org/whl/cu130"
fi

TORCH_SPEC="${VBLIS_TORCH_SPEC:-torch>=2.14.1}"
# 注意用 `-` 而不是 `:-`：显式设置成空串（macOS 默认）也应当保持为空，即走 PyPI。
TORCH_INDEX="${VBLIS_TORCH_INDEX-$DEFAULT_TORCH_INDEX}"

log() { echo "[vblis] $*" >&2; }
die() { echo "[vblis] 错误：$*" >&2; exit 1; }

[ -f "$REQ" ] || die "找不到依赖清单 $REQ"

# 源码树开发：已经有 api/.venv 就直接用，不在用户数据目录里再造一份。
if [ -z "${VBLIS_VENV_DIR:-}" ] && [ -x "$API_DIR/.venv/bin/python" ]; then
    echo "$API_DIR/.venv/bin/python"
    exit 0
fi

# ── 按平台整理依赖清单 ──────────────────────────────────────────────────────
# macOS 上 pip 看不到 cu130 索引里的 torch，requirements.txt 的
# `--extra-index-url` 与 constraints.txt 里的 CUDA / Windows 专用 pin 会让安装直接
# 失败；这里生成一份过滤后的副本，Linux 路径保持原来的文件不动。
WORK_DIR=""
cleanup() { [ -n "$WORK_DIR" ] && rm -rf "$WORK_DIR"; }
trap cleanup EXIT

if [ "$IS_MAC" = "1" ]; then
    WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/vblis-venv.XXXXXX")"
    grep -v '^--' "$REQ" > "$WORK_DIR/requirements.txt"
    REQ="$WORK_DIR/requirements.txt"
    if [ -f "$CONSTRAINTS" ]; then
        # torch 及 CUDA/Windows 专有的 pin 在 macOS 上不可用（或没有对应轮子）
        grep -vE '^(torch|torchvision|triton|pywin32-ctypes|pefile|pyinstaller|pyinstaller-hooks-contrib|altgraph)==' \
            "$CONSTRAINTS" > "$WORK_DIR/constraints.txt" || true
        CONSTRAINTS="$WORK_DIR/constraints.txt"
    fi
fi

VENV_DIR="${VBLIS_VENV_DIR:-$DATA_DIR/venv}"
VENV_PY="$VENV_DIR/bin/python"
STAMP="$VENV_DIR/.vblis-ready"

# macOS 没有 coreutils；shasum 自带。指纹只用于判断依赖有没有变化，算法不必通用。
hash_stream() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum | cut -d' ' -f1
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | cut -d' ' -f1
    else
        cksum | awk '{print $1 "-" $2}'
    fi
}

# 用 python 版本 + 平台 + 依赖清单 + torch 规格算一个指纹，变了才重新装。
fingerprint() {
    "$VENV_PY" -c 'import sys; print(sys.version)' 2>/dev/null || echo unknown
    echo "platform=$UNAME_S-$UNAME_M"
    cat "$REQ"
    if [ -f "$CONSTRAINTS" ]; then cat "$CONSTRAINTS"; fi
    echo "torch-spec=$TORCH_SPEC"
    echo "torch-index=$TORCH_INDEX"
}

venv_ready() {
    [ -x "$VENV_PY" ] || return 1
    [ -f "$STAMP" ] || return 1
    [ "$(cat "$STAMP" 2>/dev/null)" = "$(fingerprint | hash_stream)" ] || return 1
    # 只查包是否存在（find_spec 不会真的 import），避免每次启动都花几秒加载 torch/transformers
    "$VENV_PY" -c 'import importlib.util as u, sys
mods = ("fastapi", "uvicorn", "torch", "transformers", "chromadb", "av", "PIL", "cryptography")
missing = [m for m in mods if u.find_spec(m) is None]
sys.exit(1 if missing else 0)' >/dev/null 2>&1
}

if venv_ready; then
    echo "$VENV_PY"
    exit 0
fi

if [ "${VBLIS_NO_VENV_INSTALL:-0}" = "1" ]; then
    die "虚拟环境 $VENV_DIR 不可用，且设置了 VBLIS_NO_VENV_INSTALL=1（先手动安装依赖）"
fi

# ── 找系统 python（3.11–3.14；torch 2.14.1 为这些版本都提供轮子） ────────────
# 3.14 在找不到旧版本时也能用；PATH 之外（pyenv、conda、/usr/local/bin）再找一轮。
PYTHON_CANDIDATES=(python3.14 python3.13 python3.12 python3.11 python3 python)
PYTHON_SEARCH_DIRS=(
    "$HOME/.local/bin" "$HOME/.pyenv/shims" "$HOME/miniconda3/bin" "$HOME/anaconda3/bin"
    /opt/conda/bin /opt/homebrew/bin /usr/local/bin /usr/bin /bin
)

python_ok() {
    "$1" -c 'import sys; raise SystemExit(0 if (3, 11) <= sys.version_info[:2] <= (3, 14) else 1)' 2>/dev/null
}

find_system_python() {
    if [ -n "${VBLIS_SYSTEM_PY:-}" ]; then
        command -v "$VBLIS_SYSTEM_PY" >/dev/null 2>&1 || [ -x "$VBLIS_SYSTEM_PY" ] \
            || die "VBLIS_SYSTEM_PY=$VBLIS_SYSTEM_PY 不存在"
        python_ok "$VBLIS_SYSTEM_PY" \
            || die "VBLIS_SYSTEM_PY=$VBLIS_SYSTEM_PY 不是 Python 3.11–3.14（$("$VBLIS_SYSTEM_PY" -V 2>&1)）"
        echo "$VBLIS_SYSTEM_PY"; return
    fi
    for candidate in "${PYTHON_CANDIDATES[@]}"; do
        path="$(type -P "$candidate" 2>/dev/null || true)"
        [ -n "$path" ] || continue
        if python_ok "$path"; then echo "$path"; return; fi
    done
    for dir in "${PYTHON_SEARCH_DIRS[@]}"; do
        for candidate in "${PYTHON_CANDIDATES[@]}"; do
            path="$dir/$candidate"
            [ -x "$path" ] || continue
            if python_ok "$path"; then echo "$path"; return; fi
        done
    done
    # uv 管理的解释器（装了 uv 但没把它加进 PATH 的情况）
    if command -v uv >/dev/null 2>&1; then
        for version in 3.14 3.13 3.12 3.11; do
            path="$(uv python find "$version" 2>/dev/null || true)"
            if [ -n "$path" ] && python_ok "$path"; then echo "$path"; return; fi
        done
    fi
    found=""
    for candidate in "${PYTHON_CANDIDATES[@]}"; do
        path="$(type -P "$candidate" 2>/dev/null || true)"
        [ -n "$path" ] && found="$found $candidate($("$path" -V 2>&1 | head -n1))"
    done
    if [ "$IS_MAC" = "1" ]; then
        die "找不到 Python 3.11–3.14。已检测到:${found:- 无}；
  用 VBLIS_SYSTEM_PY=/path/to/python3.12 指定解释器，或安装：brew install python@3.12"
    fi
    die "找不到 Python 3.11–3.14。已检测到:${found:- 无}；
  用 VBLIS_SYSTEM_PY=/path/to/python3.12 指定解释器，或安装 python3.12（Arch: sudo pacman -S python312；Ubuntu: sudo apt install python3.12-venv）"
}

# ── 创建 venv ───────────────────────────────────────────────────────────────
if [ ! -x "$VENV_PY" ]; then
    SYS_PY="$(find_system_python)"
    log "使用系统解释器 $("$SYS_PY" -V 2>&1)"
    log "创建虚拟环境 $VENV_DIR"
    mkdir -p "$(dirname "$VENV_DIR")"
    if ! "$SYS_PY" -m venv "$VENV_DIR" 2>/dev/null; then
        rm -rf "$VENV_DIR"
        "$SYS_PY" -m pip install --quiet --user virtualenv >/dev/null 2>&1 || true
        "$SYS_PY" -m virtualenv "$VENV_DIR" || die "创建虚拟环境失败（系统缺少 venv 模块？Debian/Ubuntu 装 python3-venv，macOS 重装 brew 的 python）"
    fi
fi

[ -x "$VENV_PY" ] || die "虚拟环境创建后仍找不到 $VENV_PY"
"$VENV_PY" -m pip --version >/dev/null 2>&1 || "$VENV_PY" -m ensurepip --upgrade >/dev/null 2>&1 || \
    die "虚拟环境里没有 pip，且 ensurepip 失败"

# ── 安装依赖 ────────────────────────────────────────────────────────────────
log "准备安装后端依赖（首次需要下载 PyTorch 等，约 3–5 GB，取决于网络）"
"$VENV_PY" -m pip install --upgrade pip >&2

# torch 先单独装：Linux 默认用 cu130 索引（与 requirements.txt / 上游发布一致），纯 CPU 机器也能跑；
# macOS 默认用 PyPI 轮子（含 Metal）。想强制 CPU 版或换 CUDA 版本时用
# VBLIS_TORCH_SPEC / VBLIS_TORCH_INDEX 覆盖。
if [ -n "$TORCH_INDEX" ]; then
    log "安装 ${TORCH_SPEC}（索引 ${TORCH_INDEX}）"
    "$VENV_PY" -m pip install --only-binary :all: --index-url "$TORCH_INDEX" "$TORCH_SPEC" >&2
else
    log "安装 ${TORCH_SPEC}（PyPI 默认轮子）"
    "$VENV_PY" -m pip install --only-binary :all: "$TORCH_SPEC" >&2
fi

log "安装 requirements.txt"
PIP_INSTALL=(-r "$REQ")
if [ -f "$CONSTRAINTS" ]; then PIP_INSTALL+=(-c "$CONSTRAINTS"); fi
"$VENV_PY" -m pip install --only-binary :all: "${PIP_INSTALL[@]}" >&2

fingerprint | hash_stream > "$STAMP"

if [ "$IS_MAC" = "1" ]; then
    if "$VENV_PY" -c 'import torch, sys; sys.exit(0 if torch.backends.mps.is_available() else 1)' 2>/dev/null; then
        log "检测到 Apple Metal（MPS）；GPU 加速可在页面「设置 → 硬件加速」里启用"
    else
        log "未检测到 Apple Metal（MPS），将以 CPU 推理"
    fi
elif command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1; then
    log "检测到 NVIDIA 驱动；GPU 加速可在页面「设置 → 硬件加速」里启用"
else
    log "未检测到 NVIDIA 驱动，将以 CPU 推理"
fi

echo "$VENV_PY"
