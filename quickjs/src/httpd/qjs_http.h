/*
 * qjsa:httpd -- CivetWeb as an HTTP backend for QuickJS-ng.
 *
 * Public C API. The same code base is compiled in two shapes:
 *
 *   1. embedded in the interpreter  (js_init_module_http / qjs_http_register)
 *      import * as httpd from 'qjsa:httpd';
 *
 *   2. loadable .so module         (js_init_module, see httpd_module.c)
 *      import * as httpd from './httpd.so';
 *
 * The JavaScript API is identical in both modes.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef QJS_HTTP_H
#define QJS_HTTP_H

#include <stddef.h>

#include "quickjs.h"

#if defined(_WIN32)
#  define QJS_HTTP_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#  define QJS_HTTP_API __attribute__((visibility("default")))
#else
#  define QJS_HTTP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QJS_HTTP_VERSION "0.2.0"

typedef struct qjs_http_options {
    /*
     * "8080", or "127.0.0.1:8080", or "8080,8443s" (CivetWeb syntax).
     * NULL means "8080".
     */
    const char *port;

    /* Optional bind address, e.g. "127.0.0.1". NULL binds everything. */
    const char *binding;

    /* CivetWeb worker threads. 0 -> 8. */
    int threads;

    /* Listen backlog, 0 -> CivetWeb default. */
    int backlog;

    /* 1 (default) keeps connections alive; 0 closes after each response. */
    int keep_alive;

    /* 1 (default) disables Nagle; mandatory for keep-alive throughput. */
    int tcp_nodelay;

    /* Idle/request timeout in ms, 0 -> CivetWeb default. */
    int request_timeout_ms;

    /* Install SIGINT/SIGTERM handlers so Ctrl-C stops the server cleanly. */
    int grab_signals;

    /* Put the JS error text into a 500 response body (development aid). */
    int debug;

    /* Maximum request body size in bytes. 0 -> 10 MiB. */
    size_t max_body_size;

    /*
     * Optional extra CivetWeb options, as a NULL terminated array of
     * alternating "key", "value" strings. Example:
     *     {"document_root", "/srv/www", NULL}
     */
    const char *const *extra_civetweb_options;
} qjs_http_options;

/* Fill *opts with the defaults (port 8080, 8 threads, ...). */
QJS_HTTP_API void qjs_http_options_init(qjs_http_options *opts);

/*
 * Run one HTTP server. BLOCKING: returns only after qjs_http_stop_all()
 * (or a signal, when opts->grab_signals is set) stopped it, or when the
 * server could not be started.
 *
 * handler  : JS function (req) or (req, res)
 * on_error : optional JS function (error) called when the handler throws,
 *            may be JS_UNDEFINED.
 *
 * Returns 0 on success, -1 on failure; on failure `err` receives a
 * NUL-terminated message (may be "" if err is NULL / errlen is 0).
 *
 * QuickJS is single-threaded: the JS handler always runs under the
 * internal server lock, so no other thread may touch this JSContext while
 * the server is running.
 */
QJS_HTTP_API int qjs_http_run(JSContext *ctx,
                              const qjs_http_options *opts,
                              JSValueConst handler,
                              JSValueConst on_error,
                              char *err,
                              size_t errlen);

/* Stop every server currently running in this process. Thread-safe. */
QJS_HTTP_API void qjs_http_stop_all(void);

/* Number of servers currently accepting requests. */
QJS_HTTP_API int qjs_http_is_running(void);

/*
 * Create the ES module `module_name` (call it once per JSContext before
 * evaluating code that imports it). Returns NULL and throws on failure.
 */
QJS_HTTP_API JSModuleDef *qjs_http_init_module(JSContext *ctx,
                                               const char *module_name);

/* qjs_http_init_module(ctx, "qjsa:httpd") -- the usual embedded setup. */
QJS_HTTP_API int qjs_http_register(JSContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* QJS_HTTP_H */
