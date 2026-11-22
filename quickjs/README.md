# QuickJS Addons

QuickJS 原生扩展模块仓库。

## 模块

| 模块 | 平台 | 说明 | 依赖 |
| --- | --- | --- | --- |
| `qjsa:httpd` | Linux / macOS / Windows | HTTP/1.1 服务器：CivetWeb 内核（线程池、keep-alive、静态文件/Range、流式响应、WebSocket） | 内置 CivetWeb 1.16（MIT） |
| `qjsa:winproc` | Windows | 子进程创建 / 终止（非 Windows 为 stub） | 无 |
| `qjsa:resources` | 全平台 | 读取编译时嵌入的文本资源 | 无 |

API 见 `docs/`。`qjsa:httpd` 的 CivetWeb 源码在 `src/httpd/vendor/civetweb/`，
不需要额外 submodule。

## 构建

```bash
git clone --recursive <repo> && cd <repo>

# 默认 glibc 静态构建；main.js 为入口脚本（缺失时会自动创建空文件）
cmake -B build && cmake --build build -j
./build/app-linux

# .so 模块：产出 build/modules/*.so 与宿主解释器 build/qjs-shared
cmake -B build -DBUILD_SHARED_MODULES=ON && cmake --build build -j
./build/qjs-shared -e 'import("./build/modules/http.so").then(m => print(m.version))'

# musl 静态构建：只换一个参数（.so 需改用 musl 动态宿主，见 docs/httpd.md）
cmake -B build-musl -DQJSA_LIBC=musl && cmake --build build-musl -j
./build-musl/app-linux
```

### 选择 C 库

`-DQJSA_LIBC=glibc`（默认）或 `-DQJSA_LIBC=musl`。该参数统一作用于
`app-linux`、`qjsc`/`libqjs`（ExternalProject）和 `.so` 模块，不会出现
glibc 与 musl 混链。只传 `-DCMAKE_C_COMPILER=musl-gcc` 时也会自动按 musl 构建。
musl 静态程序不支持 `dlopen`，需要 `.so` 时同时加 `-DBUILD_SHARED_MODULES=ON`。

### 裁剪模块

`CMakeLists.txt` 的「模块开关」区每个模块一行：

```cmake
set(QJSA_HTTPD     ON)
set(QJSA_WINPROC   ON)
set(QJSA_RESOURCES ON)
```

注释掉某一行（或对应的 `if(QJSA_XXX) ... endif()` 整块）即可完全禁用该模块：
源文件、`-M qjsa:xxx` 映射、`.so` 目标和 Windows 目标都会一起移除。