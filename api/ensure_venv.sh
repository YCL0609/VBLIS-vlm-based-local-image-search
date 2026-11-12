#!/usr/bin/env bash
# 确保 Python 后端有一个可用的虚拟环境，并把解释器路径打印到 stdout（最后一行）。
#
# 这里只处理「环境」：查找/创建 venv、安装 requirements.txt。真正的启动逻辑在
# run.sh（打包产物）或 run.py（源码树）里，避免职责混在一起。
#
# 目录选择：
#   api/.venv 已经存在（源码树开发）        → 直接用它，不做任何安装
#   VBLIS_VENV_DIR 有设置                   → 用它
#   否则                                    → $VBLIS_DATA_DIR/venv（默认 ~/.local/share/vblis/venv）
#
# 可用环境变量：
#   VBLIS_VENV_DIR          指定 venv 目录
#   VBLIS_SYSTEM_PY         指定用来创建 venv 的系统解释器（默认自动找 python3.x）
#   VBLIS_TORCH_SPEC        torch 的 pip 规格（默认 "torch"，即 PyPI 的 CUDA 版，CPU 机器同样可用）
#   VBLIS_TORCH_INDEX       torch 的额外索引地址（如 https://download.pytorch.org/whl/cu130）
#   VBLIS_NO_VENV_INSTALL=1 禁止自动安装；环境不可用时直接报错退出
set -euo pipefail

API_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA_DIR="${VBLIS_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/vblis}"
REQ="$API_DIR/requirements.txt"
TORCH_SPEC="${VBLIS_TORCH_SPEC:-torch}"

log() { echo "[vblis] $*" >&2; }
die() { echo "[vblis] 错误：$*" >&2; exit 1; }

[ -f "$REQ" ] || die "找不到依赖清单 $REQ"

# 源码树开发：已经有 api/.venv 就直接用，不在用户数据目录里再造一份。
if [ -z "${VBLIS_VENV_DIR:-}" ] && [ -x "$API_DIR/.venv/bin/python" ]; then
    echo "$API_DIR/.venv/bin/python"
    exit 0
fi

VENV_DIR="${VBLIS_VENV_DIR:-$DATA_DIR/venv}"
VENV_PY="$VENV_DIR/bin/python"
STAMP="$VENV_DIR/.vblis-ready"

# 用 python 版本 + 依赖清单 + torch 规格算一个指纹，变了才重新装。
fingerprint() {
    "$VENV_PY" -c 'import sys; print(sys.version)' 2>/dev/null || echo unknown
    cat "$REQ"
    echo "torch-spec=$TORCH_SPEC"
    echo "torch-index=${VBLIS_TORCH_INDEX:-pypi}"
}

venv_ready() {
    [ -x "$VENV_PY" ] || return 1
    [ -f "$STAMP" ] || return 1
    [ "$(cat "$STAMP" 2>/dev/null)" = "$(fingerprint | sha256sum | cut -d' ' -f1)" ] || return 1
    # 只查包是否存在（find_spec 不会真的 import），避免每次启动都花几秒加载 torch/transformers
    "$VENV_PY" -c 'import importlib.util as u, sys
mods = ("fastapi", "uvicorn", "torch", "transformers", "chromadb", "av", "PIL")
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

# ── 找系统 python（>= 3.10，代码里用了 X | None 写法） ───────────────────────
find_system_python() {
    if [ -n "${VBLIS_SYSTEM_PY:-}" ]; then
        command -v "$VBLIS_SYSTEM_PY" >/dev/null 2>&1 || die "VBLIS_SYSTEM_PY=$VBLIS_SYSTEM_PY 不存在"
        echo "$VBLIS_SYSTEM_PY"; return
    fi
    for candidate in python3.14 python3.13 python3.12 python3.11 python3.10 python3; do
        if command -v "$candidate" >/dev/null 2>&1 \
           && "$candidate" -c 'import sys; raise SystemExit(0 if sys.version_info >= (3, 10) else 1)' 2>/dev/null; then
            echo "$candidate"; return
        fi
    done
    die "找不到 Python >= 3.10（设置 VBLIS_SYSTEM_PY 指向可用的解释器）"
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
        "$SYS_PY" -m virtualenv "$VENV_DIR" || die "创建虚拟环境失败（系统缺少 venv 模块？装 python3-venv 后重试）"
    fi
fi

[ -x "$VENV_PY" ] || die "虚拟环境创建后仍找不到 $VENV_PY"
"$VENV_PY" -m pip --version >/dev/null 2>&1 || "$VENV_PY" -m ensurepip --upgrade >/dev/null 2>&1 || \
    die "虚拟环境里没有 pip，且 ensurepip 失败"

# ── 安装依赖 ────────────────────────────────────────────────────────────────
log "准备安装后端依赖（首次需要下载 PyTorch 等，约 3–5 GB，取决于网络）"
"$VENV_PY" -m pip install --upgrade pip >&2

# torch 先单独装：PyPI 的 Linux 轮子自带 CUDA，NVIDIA 机器直接可用，纯 CPU 机器也能跑；
# 想强制 CPU 版或换 CUDA 版本时用 VBLIS_TORCH_SPEC / VBLIS_TORCH_INDEX 覆盖。
if [ -n "${VBLIS_TORCH_INDEX:-}" ]; then
    log "安装 $TORCH_SPEC（索引 $VBLIS_TORCH_INDEX）"
    "$VENV_PY" -m pip install --only-binary :all: --index-url "$VBLIS_TORCH_INDEX" "$TORCH_SPEC" >&2
else
    log "安装 $TORCH_SPEC（PyPI 默认轮子）"
    "$VENV_PY" -m pip install --only-binary :all: "$TORCH_SPEC" >&2
fi

log "安装 requirements.txt"
"$VENV_PY" -m pip install --only-binary :all: -r "$REQ" >&2

fingerprint | sha256sum | cut -d' ' -f1 > "$STAMP"

if command -v nvidia-smi >/dev/null 2>&1 && nvidia-smi -L >/dev/null 2>&1; then
    log "检测到 NVIDIA 驱动；GPU 加速可在页面「设置 → 硬件加速」里启用"
else
    log "未检测到 NVIDIA 驱动，将以 CPU 推理"
fi

echo "$VENV_PY"
