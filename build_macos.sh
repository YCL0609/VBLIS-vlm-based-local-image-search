#!/usr/bin/env bash
# VBLIS macOS 构建脚本
#
# 默认产物：dist/vblis-macos-<arch>/（源码分发，不带 PyInstaller）
#   vblis               QuickJS 外壳（内嵌 vsite/dist，即前端资源）
#   api/                Python 后端源码（*.py + routers/ + place_data/ + requirements.txt + constraints.txt）
#   run.sh              启动脚本：检测 venv，缺失时用系统 python 创建并安装依赖
#   README.md、LICENSE
#
# 用法：
#   ./build_macos.sh                 # 完整构建：前端 + 外壳 + 后端代码 + 组装 + 冒烟测试
#   ./build_macos.sh --binary        # 额外用 PyInstaller 把后端打成独立可执行文件
#   ./build_macos.sh --skip-backend  # 只构建前端和外壳（产物不含 api/）
#   ./build_macos.sh --skip-frontend --skip-shell   # 只重新组装
#   ./build_macos.sh --no-smoke      # 跳过冒烟测试
#
# 依赖：Xcode Command Line Tools（clang、make）、cmake、node/npm；
#       --binary 时还需要 api/.venv 与 pyinstaller。
#
# 说明：macOS 的二进制由链接器自动做 ad-hoc 签名，这里刻意不做 strip——
# 修改已签名的 Mach-O 会让 arm64 上的可执行文件被内核直接杀掉（Killed: 9）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

[ "$(uname -s)" = "Darwin" ] || {
    echo "此脚本只用于 macOS；Linux 请用 ./build_linux.sh" >&2
    exit 1
}

ARCH="$(uname -m)"
APP_NAME="vblis"
OUT_DIR="$ROOT/dist/vblis-macos-$ARCH"
API_DIR="$ROOT/api"
VSITE_DIR="$ROOT/vsite"
QUICKJS_DIR="$ROOT/quickjs"
VENV_PY="$API_DIR/.venv/bin/python"
DATA_HOME="$HOME/Library/Application Support/VBLIS"

SKIP_FRONTEND=0
SKIP_SHELL=0
SKIP_BACKEND=0
WITH_BINARY=0
DO_CLEAN=1
DO_SMOKE=1
SMOKE_PORT="${VBLIS_SMOKE_PORT:-8123}"

usage() {
    sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-frontend) SKIP_FRONTEND=1 ;;
        --skip-shell)    SKIP_SHELL=1 ;;
        --skip-backend)  SKIP_BACKEND=1 ;;
        --binary)        WITH_BINARY=1 ;;
        --no-clean)      DO_CLEAN=0 ;;
        --no-smoke)      DO_SMOKE=0 ;;
        -o|--output)     OUT_DIR="$2"; shift ;;
        -h|--help)       usage ;;
        *) echo "未知参数: $1（--help 查看用法）" >&2; exit 2 ;;
    esac
    shift
done

# ── 输出辅助 ────────────────────────────────────────────────────────────────
if [ -t 1 ]; then
    C_CYAN=$'\033[36m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_RESET=$'\033[0m'
else
    C_CYAN=''; C_GREEN=''; C_YELLOW=''; C_RED=''; C_RESET=''
fi
step()  { echo; echo "${C_CYAN}=== $* ===${C_RESET}"; }
info()  { echo "  $*"; }
ok()    { echo "  ${C_GREEN}✓${C_RESET} $*"; }
warn()  { echo "  ${C_YELLOW}!${C_RESET} $*"; }
die()   { echo "${C_RED}✗ $*${C_RESET}" >&2; exit 1; }

need() {
    command -v "$1" >/dev/null 2>&1 || die "缺少 $1，请先安装（$2）"
}

# ── 0. 环境检查 ─────────────────────────────────────────────────────────────
step "0. 检查构建环境"

if [ "$ARCH" = "arm64" ]; then
    info "架构: Apple Silicon (arm64)"
elif [ "$ARCH" = "x86_64" ]; then
    info "架构: Intel (x86_64)"
else
    die "不支持的架构 $ARCH"
fi

xcode-select -p >/dev/null 2>&1 \
    || die "缺少 Xcode Command Line Tools，先执行：xcode-select --install"
need clang "xcode-select --install（随 Command Line Tools 安装）"
need make "xcode-select --install（随 Command Line Tools 安装）"
need cmake "brew install cmake"
[ "$SKIP_FRONTEND" -eq 1 ] || { need node "brew install node"; need npm "随 node 安装"; }
if [ "$WITH_BINARY" -eq 1 ]; then
    [ -x "$VENV_PY" ] || die "--binary 需要 $VENV_PY（先创建后端虚拟环境并安装依赖）"
    info "后端解释器: $($VENV_PY -V 2>&1)"
fi
info "仓库: $ROOT"
info "产物: $OUT_DIR"
info "打包方式: $([ "$WITH_BINARY" -eq 1 ] && echo 'PyInstaller 可执行文件' || echo 'Python 源码 + run.sh 自动建 venv')"

# ── 1. 清理 ─────────────────────────────────────────────────────────────────
if [ "$DO_CLEAN" -eq 1 ]; then
    step "1. 清理旧产物"
    # 只清掉本次会重新生成的东西，跳过阶段时保留已有产物
    CLEAN_DIRS=("$OUT_DIR")
    [ "$SKIP_FRONTEND" -eq 1 ] || CLEAN_DIRS+=("$VSITE_DIR/dist")
    [ "$SKIP_SHELL" -eq 1 ]    || CLEAN_DIRS+=("$QUICKJS_DIR/build-macos")
    if [ "$WITH_BINARY" -eq 1 ] && [ "$SKIP_BACKEND" -eq 0 ]; then
        CLEAN_DIRS+=("$API_DIR/build" "$API_DIR/dist")
    fi
    for d in "${CLEAN_DIRS[@]}"; do
        if [ -e "$d" ]; then rm -rf "$d"; info "已删除 $d"; fi
    done
    ok "清理完成"
else
    step "1. 清理旧产物（--no-clean，跳过）"
fi

# ── 2. 前端（vsite → vsite/dist） ───────────────────────────────────────────
if [ "$SKIP_FRONTEND" -eq 1 ]; then
    step "2. 构建前端（跳过）"
else
    step "2. 构建前端 vsite"
    if [ ! -d "$VSITE_DIR/node_modules" ]; then
        info "安装前端依赖…"
        ( cd "$VSITE_DIR" && { [ -f package-lock.json ] && npm ci || npm install; } )
    fi
    ( cd "$VSITE_DIR" && npm run build )
    for f in index.html index.js index.css vite.svg; do
        [ -f "$VSITE_DIR/dist/$f" ] || die "前端产物缺少 dist/$f"
    done
    ok "前端产物: $(du -sh "$VSITE_DIR/dist" | cut -f1)"
fi

# 外壳把 vsite/dist 编进二进制，缺失时资源会是空的
[ -f "$VSITE_DIR/dist/index.html" ] || die "缺少 $VSITE_DIR/dist/index.html（先构建前端，或去掉 --skip-frontend）"

# ── 3. 外壳（quickjs/build-macos/app-macos） ────────────────────────────────
if [ "$SKIP_SHELL" -eq 1 ]; then
    step "3. 构建 QuickJS 外壳（跳过）"
    [ -x "$QUICKJS_DIR/build-macos/app-macos" ] || die "没有现成的 build-macos/app-macos，不能跳过外壳构建"
else
    step "3. 构建 QuickJS 外壳（系统 clang，动态链接 libSystem）"
    ( cd "$QUICKJS_DIR" && ./build.sh )
    [ -x "$QUICKJS_DIR/build-macos/app-macos" ] || die "外壳构建失败：没有 app-macos"
    ok "外壳产物: $(du -h "$QUICKJS_DIR/build-macos/app-macos" | cut -f1)"
fi

# ── 4. 后端 ─────────────────────────────────────────────────────────────────
if [ "$SKIP_BACKEND" -eq 1 ]; then
    step "4. 准备 Python 后端（跳过）"
    warn "产物目录不含后端，运行时外壳会回退到源码树的 api/"
elif [ "$WITH_BINARY" -eq 1 ]; then
    step "4. 打包 Python 后端（PyInstaller）"
    if ! "$VENV_PY" -m PyInstaller --version >/dev/null 2>&1; then
        info "安装 PyInstaller…"
        "$VENV_PY" -m pip install --quiet --upgrade pyinstaller
    fi
    info "PyInstaller $("$VENV_PY" -m PyInstaller --version 2>&1 | tail -1)"
    info "这一步会收集 torch / transformers / chromadb 等依赖，通常需要几分钟…"
    # 网络共享由后端自己提供页面；冻结版从 RESOURCE_DIR/web 读取，先放进 api/ 让 spec 收集
    rm -rf "$API_DIR/web"
    cp -a "$VSITE_DIR/dist" "$API_DIR/web"
    trap 'rm -rf "$API_DIR/web"' EXIT
    ( cd "$API_DIR" && "$VENV_PY" -m PyInstaller api.spec \
        --noconfirm --clean \
        --log-level WARN \
        --distpath dist --workpath build ) \
        || die "PyInstaller 打包失败（见上方日志；常见原因：依赖缺失、磁盘不足、架构不匹配）"
    [ -x "$API_DIR/dist/api/api" ] || die "PyInstaller 未生成 dist/api/api"
    ok "后端产物: $(du -sh "$API_DIR/dist/api" | cut -f1)"
else
    step "4. 收集 Python 后端源码"
    [ -f "$API_DIR/place_data/places.npz" ] || die "缺少 $API_DIR/place_data/places.npz"
    rm -rf "$OUT_DIR/api"
    mkdir -p "$OUT_DIR/api"
    cp -a "$API_DIR"/*.py "$OUT_DIR/api/"
    cp -a "$API_DIR/routers" "$OUT_DIR/api/"
    cp -a "$API_DIR/place_data" "$OUT_DIR/api/"
    cp -a "$API_DIR/requirements.txt" "$API_DIR/constraints.txt" "$API_DIR/ensure_venv.sh" "$OUT_DIR/api/"
    chmod +x "$OUT_DIR/api/ensure_venv.sh"
    # 网络共享需要一份可由后端直接读取的页面（外壳内嵌的那份只服务本机）
    rm -rf "$OUT_DIR/web"
    cp -a "$VSITE_DIR/dist" "$OUT_DIR/web"
    ok "后端源码: $(find "$OUT_DIR/api" -name '*.py' | wc -l) 个 .py，$(du -sh "$OUT_DIR/api" | cut -f1)"
fi

# ── 5. 组装 ─────────────────────────────────────────────────────────────────
step "5. 组装产物目录"
mkdir -p "$OUT_DIR"

install -m 0755 "$QUICKJS_DIR/build-macos/app-macos" "$OUT_DIR/$APP_NAME"
# 不要 strip：会破坏链接器生成的 ad-hoc 签名，arm64 上直接无法运行
codesign --verify --quiet "$OUT_DIR/$APP_NAME" 2>/dev/null \
    && ok "外壳: $OUT_DIR/$APP_NAME（ad-hoc 签名有效）" \
    || {
        warn "外壳未带有效签名，补一个 ad-hoc 签名"
        codesign --force --sign - "$OUT_DIR/$APP_NAME" 2>/dev/null || true
        ok "外壳: $OUT_DIR/$APP_NAME"
    }

if [ "$WITH_BINARY" -eq 1 ] && [ "$SKIP_BACKEND" -eq 0 ]; then
    rm -rf "$OUT_DIR/api"
    mkdir -p "$OUT_DIR/api"
    cp -a "$API_DIR/dist/api/." "$OUT_DIR/api/"
    chmod +x "$OUT_DIR/api/api"
    codesign --force --sign - "$OUT_DIR/api/api" 2>/dev/null || true
    ok "后端: $OUT_DIR/api/api"
    cat > "$OUT_DIR/run.sh" <<'EOF'
#!/usr/bin/env bash
# 启动 VBLIS：外壳负责静态服务（127.0.0.1:11120）并拉起同目录的 api/api 后端。
cd "$(dirname "${BASH_SOURCE[0]}")"
exec ./vblis "$@"
EOF
elif [ "$SKIP_BACKEND" -eq 0 ]; then
    cat > "$OUT_DIR/run.sh" <<'EOF'
#!/usr/bin/env bash
# 启动 VBLIS。
#
# 首次运行会自动准备 Python 环境：用系统 python 在用户数据目录创建 venv，
# 安装 api/requirements.txt（含 PyTorch；macOS 轮子自带 Metal/MPS），然后启动。
# 之后每次启动直接复用，只有 requirements 变化时才重新安装。
#
#   ./run.sh                      启动应用（外壳 + 后端）
#   ./run.sh --backend            只启动后端（外壳会用它拉起后端）
#   ./run.sh --backend --port 8011   给后端换端口（其余参数见 api/main.py --help）
#
# 环境变量：
#   VBLIS_DATA_DIR      数据/模型目录（默认 ~/Library/Application Support/VBLIS）
#   VBLIS_VENV_DIR      指定 venv 目录（默认 $VBLIS_DATA_DIR/venv）
#   VBLIS_SYSTEM_PY     指定创建 venv 的系统解释器
#   VBLIS_TORCH_SPEC    torch 规格（默认 torch>=2.14.1）
#   VBLIS_TORCH_INDEX   torch 索引地址（macOS 默认空＝PyPI；可指向自建镜像）
#   VBLIS_NO_VENV_INSTALL=1  禁止自动安装依赖
#   PYTORCH_ENABLE_MPS_FALLBACK=1（默认已开启）不支持的算子回退到 CPU
set -euo pipefail
APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$APP_DIR"

MODE=gui
if [ "${1:-}" = "--backend" ]; then MODE=backend; shift; fi

PY="${VBLIS_PYTHON:-}"
if [ -z "$PY" ]; then
  PY="$(bash "$APP_DIR/api/ensure_venv.sh" | tail -n 1)"
fi
[ -x "$PY" ] || { echo "[vblis] 没有可用的 Python 解释器（$PY）" >&2; exit 1; }
export VBLIS_PYTHON="$PY"

if [ "$MODE" = "backend" ]; then
  export VBLIS_DATA_DIR="${VBLIS_DATA_DIR:-$HOME/Library/Application Support/VBLIS}"
  export VBLIS_WEB_DIR="${VBLIS_WEB_DIR:-$APP_DIR/web}"
  if [ -f "$APP_DIR/api/run.py" ]; then
    exec "$PY" "$APP_DIR/api/run.py" "$@"
  fi
  exec "$PY" "$APP_DIR/api/main.py" "$@"
fi

exec "$APP_DIR/vblis" "$@"
EOF
fi
[ -f "$OUT_DIR/run.sh" ] && chmod +x "$OUT_DIR/run.sh"

for f in README.md LICENSE; do
    [ -f "$ROOT/$f" ] && cp -a "$ROOT/$f" "$OUT_DIR/$f"
done

ok "组装完成: $(du -sh "$OUT_DIR" | cut -f1)"

# ── 6. 冒烟测试 ─────────────────────────────────────────────────────────────
if [ "$DO_SMOKE" -eq 1 ]; then
    step "6. 冒烟测试"
    if ! command -v curl >/dev/null 2>&1; then
        warn "没有 curl，跳过冒烟测试"
    else
        # BSD mktemp 要求模板以 X 结尾（-t 不接受带后缀的模板）
        LOG="$(mktemp "${TMPDIR:-/tmp}/vblis-smoke.XXXXXX")"
        SMOKE_ENV=(VBLIS_API_PORT="$SMOKE_PORT" VBLIS_NO_BROWSER=1 VBLIS_ALLOW_MULTI=1)
        TEST_BACKEND=1
        if [ "$SKIP_BACKEND" -eq 1 ]; then
            TEST_BACKEND=0
        elif [ "$WITH_BINARY" -eq 0 ]; then
            # 源码包的后端要现建 venv / 装依赖，冒烟测试只复用已经准备好的环境。
            SMOKE_VENV="${VBLIS_SMOKE_VENV:-$DATA_HOME/venv}"
            if [ -x "$SMOKE_VENV/bin/python" ]; then
                SMOKE_ENV+=(VBLIS_VENV_DIR="$SMOKE_VENV")
                info "复用后端环境: $SMOKE_VENV"
            elif [ -x "$VENV_PY" ]; then
                # 不设置 VBLIS_VENV_DIR，ensure_venv.sh 会优先复用源码树的 api/.venv
                info "复用后端环境: $API_DIR/.venv"
            else
                warn "找不到可复用的 venv（$SMOKE_VENV），本次只测外壳；"
                warn "首次安装依赖可执行: $OUT_DIR/run.sh --backend"
                TEST_BACKEND=0
            fi
        fi
        [ "$TEST_BACKEND" -eq 1 ] || SMOKE_ENV+=(VBLIS_NO_BACKEND=1)

        info "启动 $APP_NAME（API 端口 $SMOKE_PORT，日志 $LOG）…"
        env "${SMOKE_ENV[@]}" "$OUT_DIR/$APP_NAME" >"$LOG" 2>&1 &
        SHELL_PID=$!
        SMOKE_OK=0
        TRIES=0
        while [ "$TRIES" -lt 90 ]; do
            if curl -fsS -o /dev/null "http://127.0.0.1:11120/" 2>/dev/null \
               && { [ "$TEST_BACKEND" -eq 0 ] || curl -fsS -o /dev/null "http://127.0.0.1:$SMOKE_PORT/models" 2>/dev/null; }; then
                SMOKE_OK=1
                break
            fi
            TRIES=$((TRIES + 1))
            sleep 1
        done
        kill "$SHELL_PID" 2>/dev/null || true
        wait "$SHELL_PID" 2>/dev/null || true
        sleep 1
        if [ "$SMOKE_OK" -eq 1 ]; then
            if [ "$TEST_BACKEND" -eq 0 ]; then
                ok "外壳响应正常（前端 11120；本次未验证后端）"
            else
                ok "外壳与后端均响应正常（前端 11120，后端 $SMOKE_PORT）"
            fi
        else
            warn "冒烟测试未通过，最后 20 行日志："
            tail -20 "$LOG" | sed 's/^/    /'
            exit 1
        fi
    fi
else
    step "6. 冒烟测试（--no-smoke，跳过）"
fi

# ── 完成 ────────────────────────────────────────────────────────────────────
step "构建完成"
echo "  产物目录: $OUT_DIR"
echo "  启动方式: $OUT_DIR/run.sh"
echo "  前端:     http://127.0.0.1:11120/"
echo "  后端:     http://127.0.0.1:${VBLIS_API_PORT:-8000}/models"
echo
echo "  后端数据默认放在 ~/Library/Application Support/VBLIS，"
echo "  首次运行 run.sh 会在那里创建 venv 并安装依赖；可用 VBLIS_DATA_DIR 覆盖。"
