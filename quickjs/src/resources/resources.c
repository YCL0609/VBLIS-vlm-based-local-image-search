/* SPDX-FileCopyrightText: 2026 YCL <email@ycl.cool> */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include "quickjs.h"

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_resources
#endif

/* @@RESOURCE_DATA@@ */

typedef struct
{
    const char *name;
    const unsigned char *data;
    uint32_t len;
} ResourceItem;

static const ResourceItem g_resources[] = {
    /* @@RESOURCE_TABLE@@ */
    {NULL, NULL, 0}};

static JSValue js_getSource(
    JSContext *ctx,
    JSValueConst this_val,
    int argc,
    JSValueConst *argv)
{
    const char *name;

    if (argc < 1)
        return JS_EXCEPTION;

    name = JS_ToCString(
        ctx,
        argv[0]);

    if (!name)
        return JS_EXCEPTION;
    for (int i = 0; g_resources[i].name; i++)
    {
        if (!strcmp(
                g_resources[i].name,
                name))
        {
            JS_FreeCString(
                ctx,
                name);

            return JS_NewStringLen(
                ctx,
                (const char *)g_resources[i].data,
                g_resources[i].len);
        }
    }

    JS_FreeCString(
        ctx,
        name);

    return JS_UNDEFINED;
}

static const JSCFunctionListEntry funcs[] = {
    // js内部函数 getSource(id) 导出
    JS_CFUNC_DEF(
        "getSource",
        1,
        js_getSource),
};

static int js_sources_init(
    JSContext *ctx,
    JSModuleDef *m)
{
    return JS_SetModuleExportList(
        ctx,
        m,
        funcs,
        sizeof(funcs) / sizeof(funcs[0]));
}

JSModuleDef *JS_INIT_MODULE(
    JSContext *ctx,
    const char *module_name)
{
    JSModuleDef *m = JS_NewCModule(
        ctx,
        module_name,
        js_sources_init);

    if (!m)
        return NULL;

    JS_AddModuleExportList(
        ctx,
        m,
        funcs,
        sizeof(funcs) / sizeof(funcs[0]));

    return m;
}