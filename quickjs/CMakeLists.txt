cmake_minimum_required(VERSION 3.15)

# =====================================================================
# 目标 C 库：glibc（默认）或 musl
#
#   cmake -B build                          # glibc（默认）
#   cmake -B build-musl -DQJSA_LIBC=musl    # musl
#
# 该参数统一决定 app-linux、qjsc/libqjs（ExternalProject）以及 .so 模块
# 使用哪套 libc，避免 glibc 与 musl 混链。
# =====================================================================
set(QJSA_LIBC "glibc" CACHE STRING "目标 C 库：glibc（默认）或 musl")
set_property(CACHE QJSA_LIBC PROPERTY STRINGS glibc musl)
if(NOT QJSA_LIBC MATCHES "^glibc$|^musl$")
    message(FATAL_ERROR "QJSA_LIBC 只能取 glibc 或 musl，当前为 '${QJSA_LIBC}'")
endif()

if(QJSA_LIBC STREQUAL "musl" AND NOT CMAKE_C_COMPILER)
    find_program(QJSA_MUSL_GCC musl-gcc)
    if(NOT QJSA_MUSL_GCC)
        message(FATAL_ERROR "QJSA_LIBC=musl，但找不到 musl-gcc，请先安装 musl 工具链")
    endif()
    set(CMAKE_C_COMPILER "${QJSA_MUSL_GCC}")
endif()

project(QjsApp LANGUAGES C)

# 也允许只传 -DCMAKE_C_COMPILER=musl-gcc 的调用方式
if(CMAKE_C_COMPILER MATCHES "musl" AND QJSA_LIBC STREQUAL "glibc")
    set(QJSA_LIBC "musl")
    message(STATUS "检测到 musl 编译器，按 musl 方式构建")
endif()
message(STATUS "目标 C 库: ${QJSA_LIBC} (编译器: ${CMAKE_C_COMPILER})")

#### 全局编译选项 ####

set(COMMON_CFLAGS  -Os -flto -ffunction-sections -fdata-sections)
set(COMMON_LDFLAGS -Wl,--gc-sections -s)

#### 构建输出目录 ####

set(BUILD_OUTPUT_DIR ${CMAKE_BINARY_DIR})
set(QUICKJS_BUILD_DIR ${BUILD_OUTPUT_DIR}/quickjs)
set(MODULES_OUTPUT_DIR ${BUILD_OUTPUT_DIR}/modules)

file(MAKE_DIRECTORY ${BUILD_OUTPUT_DIR}/libs)
file(MAKE_DIRECTORY ${QUICKJS_BUILD_DIR})

# 确保存在测试脚本 main.js, 若缺失则创建一个空文件供 qjsc 使用
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/main.js")
    file(WRITE "${CMAKE_SOURCE_DIR}/main.js" "")
    message(STATUS "Created empty main.js")
endif()

######### 模块开关 #########
#
# 每个模块一个独立小节。禁用某个模块只需：
#   1. 注释掉小节里的 `set(QJSA_XXX ON)` 这一行，或
#   2. 注释掉整个 `if(QJSA_XXX) ... endif()` 块。
#
# 禁用后，该模块的源文件、-M 模块映射、.so 目标和 Windows 目标
# 会一起消失；main.js 里再 import 它会在 qjsc 阶段报模块找不到。
#
# 默认全部开启。
#########

set(QJSA_HTTPD     ON)   # qjsa:httpd    —— CivetWeb 1.16（MIT，内置源码）
set(QJSA_WINPROC   ON)   # qjsa:winproc  —— Windows 子进程管理（非 Windows 为 stub）
set(QJSA_RESOURCES ON)   # qjsa:resources —— 编译期嵌入的文本资源

# 聚合变量
set(APP_MODULE_SRCS "")
set(APP_MODULE_DEFS "")
set(APP_MODULE_INCS "")
set(WIN_MODULE_SRCS "")
set(MODULE_MAP_ARGS "")
set(PRE_BUILD_SCRIPTS "")
set(GENERATED_LIBS_SRCS "")
set(SHARED_MODULES "")

# 共享模块（.so）目标构造函数；各模块在下面按需调用
option(BUILD_SHARED_MODULES "构建可被 qjs 加载的 .so 原生模块" OFF)
if(BUILD_SHARED_MODULES)
    file(MAKE_DIRECTORY ${MODULES_OUTPUT_DIR})
    # 模块目标：JS_* 符号保持未定义，由宿主解释器在 dlopen 时提供
    function(add_shared_module name)
        add_library(${name} MODULE ${ARGN})
        target_include_directories(${name} PRIVATE ${APP_INCLUDE_DIRS})
        target_compile_definitions(${name} PRIVATE JS_SHARED_LIBRARY)
        target_compile_options(${name} PRIVATE -fPIC -O2)
        set_target_properties(${name} PROPERTIES
            PREFIX ""
            SUFFIX ".so"
            LIBRARY_OUTPUT_DIRECTORY ${MODULES_OUTPUT_DIR}
        )
    endfunction()
endif()

# ---- qjsa:httpd（CivetWeb 后端）----
if(QJSA_HTTPD)
    set(HTTPD_SRCS
        ${CMAKE_SOURCE_DIR}/src/httpd/httpd.c
        ${CMAKE_SOURCE_DIR}/src/httpd/httpd_module.c
        ${CMAKE_SOURCE_DIR}/src/httpd/vendor/civetweb/civetweb.c)
    set(HTTPD_DEFS
        NO_CGI NO_SSL NO_CACHING USE_WEBSOCKET
        # musl 的 pthread 默认线程栈只有 128 KiB，而 QuickJS 默认按 1 MiB
        # 栈预算做溢出检测；CivetWeb 工作线程要跑 JS 处理器，必须放大栈。
        # glibc 下无副作用，所以两套 libc 都保留。
        USE_STACK_SIZE=2097152)
    set(HTTPD_INCS
        ${CMAKE_SOURCE_DIR}/src/httpd
        ${CMAKE_SOURCE_DIR}/src/httpd/vendor/civetweb)
    # Windows 与 Linux 共用同一份实现：CivetWeb 原生支持 Windows，
    # pthread 互斥/条件变量由 MinGW-w64 自带的 winpthreads 提供。
    # 如需退回占位实现（serve 抛 InternalError，仅用于排查），
    # 配置时加 -DQJSA_HTTPD_WIN_STUB=ON。
    option(QJSA_HTTPD_WIN_STUB "Windows 目标使用 httpd_win_stub.c 占位实现" OFF)
    if(QJSA_HTTPD_WIN_STUB)
        set(HTTPD_WIN_SRCS ${CMAKE_SOURCE_DIR}/src/httpd/httpd_win_stub.c)
    else()
        set(HTTPD_WIN_SRCS ${HTTPD_SRCS})
    endif()

    list(APPEND APP_MODULE_SRCS ${HTTPD_SRCS})
    list(APPEND APP_MODULE_DEFS ${HTTPD_DEFS})
    list(APPEND APP_MODULE_INCS ${HTTPD_INCS})
    list(APPEND WIN_MODULE_SRCS ${HTTPD_WIN_SRCS})
    list(APPEND MODULE_MAP_ARGS -M qjsa:httpd,http)
    list(APPEND SHARED_MODULES http)
endif()

# ---- qjsa:winproc ----
if(QJSA_WINPROC)
    set(WINPROC_SRCS ${CMAKE_SOURCE_DIR}/src/winProc.c)
    list(APPEND APP_MODULE_SRCS ${WINPROC_SRCS})
    list(APPEND MODULE_MAP_ARGS -M qjsa:winproc,winproc)
    list(APPEND SHARED_MODULES winproc)
endif()

# ---- qjsa:resources ----
if(QJSA_RESOURCES)
    # 预构建脚本：生成 build/libs/resources.c
    list(APPEND PRE_BUILD_SCRIPTS src/resources/per-build.sh)
    list(APPEND GENERATED_LIBS_SRCS resources.c)
    list(APPEND MODULE_MAP_ARGS -M qjsa:resources,resources)
    list(APPEND SHARED_MODULES resources)
endif()

message(STATUS "启用模块: httpd=${QJSA_HTTPD} winproc=${QJSA_WINPROC} resources=${QJSA_RESOURCES}")

######### 宿主 qjsc / 静态 libqjs #########

include(ExternalProject)

# 用与 app-linux 相同的编译器构建，保证 libc 一致
ExternalProject_Add(quickjs-linux
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps/quickjs
    BINARY_DIR ${QUICKJS_BUILD_DIR}/build-linux
    INSTALL_COMMAND ""
    CMAKE_ARGS
        -DQJS_BUILD_CLI_STATIC=ON
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_C_FLAGS=-O2
        -DCMAKE_EXE_LINKER_FLAGS=-Wl,--gc-sections
    BUILD_BYPRODUCTS
        ${QUICKJS_BUILD_DIR}/build-linux/qjsc
        ${QUICKJS_BUILD_DIR}/build-linux/libqjs.a
        ${QUICKJS_BUILD_DIR}/build-linux/libqjs-libc.a
    BUILD_COMMAND $(MAKE) qjsc
)

# 生成并修补 main.c
add_custom_command(
    OUTPUT ${BUILD_OUTPUT_DIR}/main.c
    COMMAND ${QUICKJS_BUILD_DIR}/build-linux/qjsc ${MODULE_MAP_ARGS} -e -o ${BUILD_OUTPUT_DIR}/main.c ${CMAKE_SOURCE_DIR}/main.js
    COMMAND ${CMAKE_COMMAND} -DIN_FILE=${BUILD_OUTPUT_DIR}/main.c -DOUT_FILE=${BUILD_OUTPUT_DIR}/main.c -P ${CMAKE_BINARY_DIR}/patch_main.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/main.js ${QUICKJS_BUILD_DIR}/build-linux/qjsc
    COMMENT "Generating and patching main.c (main entry point)"
)

# 补丁脚本 (使用 CMake 脚本在生成后修改 main.c)
#      - 在文件开头插入 #include <windows.h>
#      - 在 main 函数体内添加 SetConsoleCP/SetConsoleOutputCP 调用
file(WRITE ${CMAKE_BINARY_DIR}/patch_main.cmake
"file(READ \"\${IN_FILE}\" CONTENT)
set(PREFIX \"#ifdef _WIN32\\n#include <windows.h>\\n#endif\\n\")
string(PREPEND CONTENT \"\${PREFIX}\")
string(REGEX REPLACE 
    \"int main\\\\(int argc, char \\\\*\\\\*argv\\\\) *\\\\{\"
    \"int main(int argc, char **argv)\\n#ifdef _WIN32\\n    SetConsoleCP(CP_UTF8);\\n    SetConsoleOutputCP(CP_UTF8);\\n#endif\\n{\"
    CONTENT \"\${CONTENT}\")
file(WRITE \"\${OUT_FILE}\" \"\${CONTENT}\")
")

add_custom_target(generate_test_c DEPENDS ${BUILD_OUTPUT_DIR}/main.c)
add_dependencies(generate_test_c quickjs-linux)

#### 预构建脚本 ####

string(REPLACE ";" " " PRE_BUILD_SCRIPT_LIST "${PRE_BUILD_SCRIPTS}")
file(WRITE ${CMAKE_BINARY_DIR}/run_pre_build.sh
"#!/bin/sh\nset -e\nfor script in ${PRE_BUILD_SCRIPT_LIST}; do\n  sh \"${CMAKE_SOURCE_DIR}/$script\"\ndone\n")

if(PRE_BUILD_SCRIPTS)
    add_custom_target(pre_build ALL
        COMMAND /bin/sh ${CMAKE_BINARY_DIR}/run_pre_build.sh
        COMMENT "Executing pre-build scripts"
    )
else()
    add_custom_target(pre_build ALL
        COMMENT "No pre-build scripts"
    )
endif()

######### 应用源文件 ########

set(GENERATED_LIBS_PATHS "")
foreach(_gen ${GENERATED_LIBS_SRCS})
    list(APPEND GENERATED_LIBS_PATHS ${BUILD_OUTPUT_DIR}/libs/${_gen})
endforeach()

set(APP_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/deps/quickjs"
    ${APP_MODULE_INCS}
)

# 原生可执行文件
add_executable(app-linux
    ${APP_MODULE_SRCS}
    ${BUILD_OUTPUT_DIR}/main.c
    ${GENERATED_LIBS_PATHS}
)
target_include_directories(app-linux PRIVATE ${APP_INCLUDE_DIRS})
target_link_libraries(app-linux
    ${QUICKJS_BUILD_DIR}/build-linux/libqjs.a
    ${QUICKJS_BUILD_DIR}/build-linux/libqjs-libc.a
    -static
    -lm
    -lpthread
)
target_compile_options(app-linux PRIVATE ${COMMON_CFLAGS})
target_compile_definitions(app-linux PRIVATE ${APP_MODULE_DEFS})
target_link_options(app-linux PRIVATE ${COMMON_LDFLAGS})
set_target_properties(app-linux PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${BUILD_OUTPUT_DIR}
    OUTPUT_NAME app-linux
)
add_dependencies(app-linux quickjs-linux generate_test_c pre_build)

set_source_files_properties(${BUILD_OUTPUT_DIR}/main.c PROPERTIES GENERATED TRUE)
foreach(_gen ${GENERATED_LIBS_PATHS})
    set_source_files_properties(${_gen} PROPERTIES GENERATED TRUE)
endforeach()

######### Windows 目标 #########

# 构建 Windows 版 QuickJS 静态库
ExternalProject_Add(quickjs-win
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps/quickjs
    BINARY_DIR ${QUICKJS_BUILD_DIR}/build-win
    INSTALL_COMMAND ""
    CMAKE_ARGS
        -DCMAKE_SYSTEM_NAME=Windows
        -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc
        -DQJS_BUILD_CLI_STATIC=ON
        -DCMAKE_BUILD_TYPE=Release
    BUILD_BYPRODUCTS
        ${QUICKJS_BUILD_DIR}/build-win/libqjs.a
        ${QUICKJS_BUILD_DIR}/build-win/libqjs-libc.a
    BUILD_COMMAND $(MAKE) qjs-libc
)

set(WIN_INCLUDE_ARGS "")
foreach(dir ${APP_INCLUDE_DIRS})
    list(APPEND WIN_INCLUDE_ARGS "-I${dir}")
endforeach()

set(WIN_DEF_ARGS "")
foreach(def ${APP_MODULE_DEFS})
    list(APPEND WIN_DEF_ARGS "-D${def}")
endforeach()

set(WIN_APP_SRCS
    ${WIN_MODULE_SRCS}
    ${BUILD_OUTPUT_DIR}/main.c
    ${GENERATED_LIBS_PATHS}
)

# 直接调用 MinGW 编译器生成 app-win.exe
add_custom_target(app-win ALL
    COMMAND x86_64-w64-mingw32-gcc ${COMMON_CFLAGS} ${WIN_DEF_ARGS} ${WIN_INCLUDE_ARGS}
        ${WIN_APP_SRCS}
        ${QUICKJS_BUILD_DIR}/build-win/libqjs.a
        ${QUICKJS_BUILD_DIR}/build-win/libqjs-libc.a
        -o ${BUILD_OUTPUT_DIR}/app-win.exe
        ${COMMON_LDFLAGS} -static -lws2_32 -lwinpthread -lm
    DEPENDS quickjs-win generate_test_c pre_build
    COMMENT "Compiling Windows executable (app-win.exe)"
)
add_dependencies(app-win quickjs-win)

######### 共享模块（.so） #########
#
# 加载 .so 的宿主解释器必须是用共享版 libqjs 构建的 qjs
# （默认静态构建会隐藏 JS_* 符号，dlopen 时报 undefined symbol）。
# 注意：.so 与宿主必须使用同一套 libc；musl 静态程序不支持 dlopen。

if(BUILD_SHARED_MODULES)
    # 共享版 libqjs + 可用于 dlopen 模块的宿主解释器
    ExternalProject_Add(quickjs-shared
        SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps/quickjs
        BINARY_DIR ${QUICKJS_BUILD_DIR}/build-shared
        INSTALL_COMMAND ""
        CMAKE_ARGS
            -DBUILD_SHARED_LIBS=ON
            -DQJS_BUILD_CLI=ON
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        BUILD_BYPRODUCTS
            ${QUICKJS_BUILD_DIR}/build-shared/qjs
    )

    add_custom_target(qjs-shared ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${QUICKJS_BUILD_DIR}/build-shared/qjs
            ${BUILD_OUTPUT_DIR}/qjs-shared
        DEPENDS quickjs-shared
        COMMENT "Copying shared-libqjs interpreter to build/qjs-shared"
    )

    # qjsa:httpd
    if(QJSA_HTTPD)
        add_shared_module(http ${HTTPD_SRCS})
        target_compile_definitions(http PRIVATE ${HTTPD_DEFS})
        target_link_libraries(http PRIVATE pthread)
    endif()

    # qjsa:winproc
    if(QJSA_WINPROC)
        add_shared_module(winproc ${WINPROC_SRCS})
    endif()

    # qjsa:resources（生成的源文件）
    if(QJSA_RESOURCES)
        add_shared_module(resources ${BUILD_OUTPUT_DIR}/libs/resources.c)
        add_dependencies(resources pre_build)
    endif()
endif()
