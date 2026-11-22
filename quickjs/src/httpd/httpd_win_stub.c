/* SPDX-FileCopyrightText: 2026 YCL <email@ycl.cool> */
/* SPDX-License-Identifier: MIT */

/* qjsa:httpd 的 Windows 占位实现（可选回退）。
 *
 * 默认 Windows 目标与 Linux 共用同一份实现：CivetWeb 原生支持 Windows，
 * pthread 互斥/条件变量由 MinGW-w64 自带的 winpthreads 提供。
 * 只有配置时显式传入 -DQJSA_HTTPD_WIN_STUB=ON 才会链接本文件：
 * 模块仍可 import，但 serve() 抛 InternalError，用于排查 Windows 兼容性。
 */
#include "qjs_http.h"

#define QH_WIN_UNSUPPORTED                                                   \
    "qjsa:httpd is running the stub build (QJSA_HTTPD_WIN_STUB=ON). "        \
    "Reconfigure without that option to enable the CivetWeb backend"

static JSValue qh_win_serve(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_ThrowInternalError(ctx, QH_WIN_UNSUPPORTED);
}

static JSValue qh_win_noop(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
    (void)this_val;
    (void)argc;
    (void)argv;
    (void)ctx;
    return JS_UNDEFINED;
}

static int qh_win_module_init(JSContext *ctx, JSModuleDef *m) {
    JS_SetModuleExport(ctx, m, "serve",
                       JS_NewCFunction(ctx, qh_win_serve, "serve", 2));
    JS_SetModuleExport(ctx, m, "stop",
                       JS_NewCFunction(ctx, qh_win_noop, "stop", 0));
    JS_SetModuleExport(ctx, m, "isRunning",
                       JS_NewCFunction(ctx, qh_win_noop, "isRunning", 0));
    JS_SetModuleExport(ctx, m, "version", JS_NewString(ctx, QJS_HTTP_VERSION));
    JS_SetModuleExport(ctx, m, "civetweb", JS_NewString(ctx, "unavailable"));
    return 0;
}

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_http
#endif

JSModuleDef *JS_INIT_MODULE(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, qh_win_module_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "serve");
    JS_AddModuleExport(ctx, m, "stop");
    JS_AddModuleExport(ctx, m, "isRunning");
    JS_AddModuleExport(ctx, m, "version");
    JS_AddModuleExport(ctx, m, "civetweb");
    return m;
}
