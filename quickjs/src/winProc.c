/* SPDX-FileCopyrightText: 2026 YCL <email@ycl.cool> */
/* SPDX-License-Identifier: MIT */

#include "quickjs.h"

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_winproc
#endif

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>

static JSClassID js_winproc_class_id;

typedef struct
{
    HANDLE hProcess;
    JSValue then_cb;
    int exit_code;
    bool finished;
    bool has_object;
    bool thread_done;
    CRITICAL_SECTION lock;
    JSContext *ctx;
} WinProcData;

static JSValue js_winproc_then_job(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 2)
        return JS_UNDEFINED;

    JSValue cb = JS_DupValue(ctx, argv[0]);
    JSValue exit_code = JS_DupValue(ctx, argv[1]);
    JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 1, &exit_code);
    JS_FreeValue(ctx, cb);
    JS_FreeValue(ctx, exit_code);
    return ret;
}

static DWORD WINAPI js_winproc_wait_thread(LPVOID arg)
{
    WinProcData *pd = arg;
    DWORD wait = WaitForSingleObject(pd->hProcess, INFINITE);
    DWORD exit_code = 0;
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pd->hProcess, &exit_code);

    JSValue cb = JS_UNDEFINED;
    JSContext *ctx = pd->ctx;

    EnterCriticalSection(&pd->lock);
    pd->finished = true;
    pd->exit_code = (int)exit_code;
    pd->thread_done = true;
    if (!JS_IsUndefined(pd->then_cb))
        cb = JS_DupValue(ctx, pd->then_cb);
    bool has_object = pd->has_object;
    LeaveCriticalSection(&pd->lock);

    CloseHandle(pd->hProcess);

    if (has_object && !JS_IsUndefined(cb))
    {
        JSValue argv[2];
        argv[0] = cb;
        argv[1] = JS_NewInt32(ctx, pd->exit_code);
        JS_EnqueueJob(ctx, js_winproc_then_job, 2, argv);
        JS_FreeValue(ctx, cb);
        JS_FreeValue(ctx, argv[1]);
    }

    if (!has_object)
    {
        DeleteCriticalSection(&pd->lock);
        js_free(ctx, pd);
    }

    return 0;
}

static void js_winproc_finalizer(JSRuntime *rt, JSValueConst val)
{
    WinProcData *pd = JS_GetOpaque(val, js_winproc_class_id);
    if (!pd)
        return;

    EnterCriticalSection(&pd->lock);
    pd->has_object = false;
    bool thread_done = pd->thread_done;
    JS_FreeValue(pd->ctx, pd->then_cb);
    pd->then_cb = JS_UNDEFINED;
    LeaveCriticalSection(&pd->lock);

    if (thread_done)
    {
        DeleteCriticalSection(&pd->lock);
        js_free(pd->ctx, pd);
    }
}

static JSValue js_proc_then(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "function expected");

    WinProcData *pd = JS_GetOpaque2(ctx, this_val, js_winproc_class_id);
    if (!pd)
        return JS_EXCEPTION;

    JSValue cb = JS_DupValue(ctx, argv[0]);

    EnterCriticalSection(&pd->lock);
    if (!JS_IsUndefined(pd->then_cb))
        JS_FreeValue(ctx, pd->then_cb);
    pd->then_cb = cb;
    bool finished = pd->finished;
    int exit_code = pd->exit_code;
    LeaveCriticalSection(&pd->lock);

    if (finished)
    {
        JSValue argv2[2];
        argv2[0] = JS_DupValue(ctx, cb);
        argv2[1] = JS_NewInt32(ctx, exit_code);
        JS_EnqueueJob(ctx, js_winproc_then_job, 2, argv2);
        JS_FreeValue(ctx, argv2[0]);
        JS_FreeValue(ctx, argv2[1]);
    }

    return JS_DupValue(ctx, this_val);
}

static JSValue js_exec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int i;
    size_t len = 0;
    char *cmdline = NULL;
    char *p;

    if (argc < 1 || (!JS_IsArray(argv[0]) && !JS_IsString(argv[0])))
        return JS_ThrowTypeError(ctx, "array or string expected");

    if (JS_IsString(argv[0]))
    {
        const char *s = JS_ToCString(ctx, argv[0]);
        if (!s)
            return JS_EXCEPTION;
        len = strlen(s);
        cmdline = js_malloc(ctx, len + 1);
        if (!cmdline)
        {
            JS_FreeCString(ctx, s);
            return JS_EXCEPTION;
        }
        memcpy(cmdline, s, len + 1);
        JS_FreeCString(ctx, s);
    }
    else
    {
        int64_t count = 0;
        JS_GetLength(ctx, argv[0], &count);
        for (i = 0; i < (int)count; i++)
        {
            JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
            const char *s = JS_ToCString(ctx, v);
            if (!s)
            {
                JS_FreeValue(ctx, v);
                js_free(ctx, cmdline);
                return JS_EXCEPTION;
            }
            len += strlen(s) + 3;
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
        }

        cmdline = js_malloc(ctx, len + 1);
        if (!cmdline)
            return JS_EXCEPTION;

        p = cmdline;
        *p = 0;

        for (i = 0; i < (int)count; i++)
        {
            JSValue v = JS_GetPropertyUint32(ctx, argv[0], i);
            const char *s = JS_ToCString(ctx, v);
            p += sprintf(p, "\"%s\"", s);
            if (i + 1 < (int)count)
                *p++ = ' ';
            *p = 0;
            JS_FreeCString(ctx, s);
            JS_FreeValue(ctx, v);
        }
    }

    wchar_t *wcmdline = NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
    if (wlen == 0)
    {
        js_free(ctx, cmdline);
        return JS_ThrowInternalError(ctx, "MultiByteToWideChar failed");
    }

    wcmdline = js_malloc(ctx, (size_t)wlen * sizeof(wchar_t));
    if (!wcmdline)
    {
        js_free(ctx, cmdline);
        return JS_EXCEPTION;
    }

    if (MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, (LPWSTR)wcmdline, wlen) == 0)
    {
        js_free(ctx, cmdline);
        js_free(ctx, wcmdline);
        return JS_ThrowInternalError(ctx, "MultiByteToWideChar failed");
    }

    HANDLE hOutput = INVALID_HANDLE_VALUE;
    HANDLE hInput = INVALID_HANDLE_VALUE;
    wchar_t *woutfile = NULL;

    if (argc >= 2)
    {
        if (!JS_IsString(argv[1]))
        {
            js_free(ctx, cmdline);
            js_free(ctx, wcmdline);
            return JS_ThrowTypeError(ctx, "string expected");
        }

        const char *outPath = JS_ToCString(ctx, argv[1]);
        if (!outPath)
        {
            js_free(ctx, cmdline);
            js_free(ctx, wcmdline);
            return JS_EXCEPTION;
        }

        int woutlen = MultiByteToWideChar(CP_UTF8, 0, outPath, -1, NULL, 0);
        if (woutlen == 0)
        {
            JS_FreeCString(ctx, outPath);
            js_free(ctx, cmdline);
            js_free(ctx, wcmdline);
            return JS_ThrowInternalError(ctx, "MultiByteToWideChar failed");
        }

        woutfile = js_malloc(ctx, (size_t)woutlen * sizeof(wchar_t));
        if (!woutfile)
        {
            JS_FreeCString(ctx, outPath);
            js_free(ctx, cmdline);
            js_free(ctx, wcmdline);
            return JS_EXCEPTION;
        }

        if (MultiByteToWideChar(CP_UTF8, 0, outPath, -1, (LPWSTR)woutfile, woutlen) == 0)
        {
            JS_FreeCString(ctx, outPath);
            js_free(ctx, cmdline);
            js_free(ctx, wcmdline);
            js_free(ctx, woutfile);
            return JS_ThrowInternalError(ctx, "MultiByteToWideChar failed");
        }

        JS_FreeCString(ctx, outPath);

        SECURITY_ATTRIBUTES sa;
        ZeroMemory(&sa, sizeof(sa));
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        sa.lpSecurityDescriptor = NULL;

        hOutput = CreateFileW(woutfile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOutput == INVALID_HANDLE_VALUE)
        {
            js_free(ctx, cmdline);
            js_free(ctx, wcmdline);
            js_free(ctx, woutfile);
            return JS_ThrowInternalError(ctx, "CreateFileW failed (%lu)", GetLastError());
        }

        hInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hInput == INVALID_HANDLE_VALUE)
        {
            CloseHandle(hOutput);
            js_free(ctx, cmdline);
            js_free(ctx, wcmdline);
            js_free(ctx, woutfile);
            return JS_ThrowInternalError(ctx, "CreateFileW failed (%lu)", GetLastError());
        }
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (hOutput != INVALID_HANDLE_VALUE)
    {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput = hInput;
        si.hStdOutput = hOutput;
        si.hStdError = hOutput;
    }

    BOOL ok = CreateProcessW(NULL, (LPWSTR)wcmdline, NULL, NULL, hOutput != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    js_free(ctx, cmdline);
    js_free(ctx, wcmdline);
    if (woutfile)
    {
        js_free(ctx, woutfile);
        woutfile = NULL;
    }

    if (!ok)
    {
        if (hOutput != INVALID_HANDLE_VALUE)
            CloseHandle(hOutput);
        if (hInput != INVALID_HANDLE_VALUE)
            CloseHandle(hInput);
        return JS_ThrowInternalError(ctx, "CreateProcess failed (%lu)", GetLastError());
    }

    WinProcData *pd = js_mallocz(ctx, sizeof(*pd));
    if (!pd)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (hOutput != INVALID_HANDLE_VALUE)
            CloseHandle(hOutput);
        if (hInput != INVALID_HANDLE_VALUE)
            CloseHandle(hInput);
        return JS_EXCEPTION;
    }

    pd->hProcess = pi.hProcess;
    pd->then_cb = JS_UNDEFINED;
    pd->exit_code = 0;
    pd->finished = false;
    pd->has_object = true;
    pd->thread_done = false;
    pd->ctx = ctx;
    InitializeCriticalSection(&pd->lock);

    JSValue proc = JS_NewObjectClass(ctx, js_winproc_class_id);
    if (JS_IsException(proc))
    {
        DeleteCriticalSection(&pd->lock);
        js_free(ctx, pd);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (hOutput != INVALID_HANDLE_VALUE)
            CloseHandle(hOutput);
        if (hInput != INVALID_HANDLE_VALUE)
            CloseHandle(hInput);
        return proc;
    }

    JS_SetOpaque(proc, pd);
    JS_SetPropertyStr(ctx, proc, "pid", JS_NewInt64(ctx, pi.dwProcessId));

    HANDLE hThread = CreateThread(NULL, 0, js_winproc_wait_thread, pd, 0, NULL);
    if (hThread == NULL)
    {
        JS_FreeValue(ctx, proc);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (hOutput != INVALID_HANDLE_VALUE)
            CloseHandle(hOutput);
        if (hInput != INVALID_HANDLE_VALUE)
            CloseHandle(hInput);
        return JS_ThrowInternalError(ctx, "CreateThread failed (%lu)", GetLastError());
    }

    CloseHandle(hThread);
    CloseHandle(pi.hThread);
    if (hOutput != INVALID_HANDLE_VALUE)
        CloseHandle(hOutput);
    if (hInput != INVALID_HANDLE_VALUE)
        CloseHandle(hInput);

    return proc;
}

static JSValue js_kill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    int64_t pid;
    if (JS_ToInt64(ctx, &pid, argv[0]))
        return JS_EXCEPTION;

    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h)
        return JS_NewBool(ctx, 0);

    BOOL ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return JS_NewBool(ctx, ok);
}

static const JSCFunctionListEntry js_winproc_proto_funcs[] = {
    JS_CFUNC_DEF("then", 1, js_proc_then),
};

static const JSCFunctionListEntry js_winproc_funcs[] = {
    JS_CFUNC_DEF("exec", 1, js_exec),
    JS_CFUNC_DEF("kill", 1, js_kill),
};

static int js_winproc_init(JSContext *ctx, JSModuleDef *m)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassDef class_def = {"WinProc", js_winproc_finalizer, NULL, NULL, NULL};

    if (JS_NewClassID(rt, &js_winproc_class_id) < 0)
        return -1;
    if (JS_NewClass(rt, js_winproc_class_id, &class_def) < 0)
        return -1;

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_winproc_proto_funcs, sizeof(js_winproc_proto_funcs) / sizeof(JSCFunctionListEntry));
    JS_SetClassProto(ctx, js_winproc_class_id, proto);

    return JS_SetModuleExportList(ctx, m, js_winproc_funcs, sizeof(js_winproc_funcs) / sizeof(JSCFunctionListEntry));
}

#else /* 非 Windows 平台的 Stubs 实现 */

static JSValue js_exec(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return JS_ThrowInternalError(ctx, "Process execution not supported on this platform");
}

static JSValue js_kill(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    return JS_ThrowInternalError(ctx, "Process killing not supported on this platform");
}

static const JSCFunctionListEntry js_winproc_funcs[] = {
    JS_CFUNC_DEF("exec", 1, js_exec),
    JS_CFUNC_DEF("kill", 1, js_kill),
};

static int js_winproc_init(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, js_winproc_funcs, sizeof(js_winproc_funcs) / sizeof(JSCFunctionListEntry));
}

#endif

JSModuleDef *JS_INIT_MODULE(JSContext *ctx, const char *module_name)
{
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_winproc_init);
    if (!m)
        return NULL;

    JS_AddModuleExportList(ctx, m, js_winproc_funcs, sizeof(js_winproc_funcs) / sizeof(JSCFunctionListEntry));
    return m;
}