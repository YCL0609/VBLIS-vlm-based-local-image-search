/* SPDX-FileCopyrightText: 2026 YCL <email@ycl.cool> */
/* SPDX-License-Identifier: MIT */

/* qjsa:httpd module entry points.
 *
 * The module is compiled in two shapes:
 *   - into the interpreter: qjsc -M qjsa:httpd,http expects
 *     js_init_module_http();
 *   - as a loadable .so: QuickJS dlopen()s the file and looks up
 *     js_init_module().
 * Both symbols funnel into the same C module.
 */
#include "qjs_http.h"

#ifdef JS_SHARED_LIBRARY
JSModuleDef *js_init_module(JSContext *ctx, const char *module_name)
{
    return qjs_http_init_module(ctx, module_name);
}
#else
JSModuleDef *js_init_module_http(JSContext *ctx, const char *module_name)
{
    return qjs_http_init_module(ctx, module_name);
}
#endif
