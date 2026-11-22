/* SPDX-FileCopyrightText: 2026 YCL <email@ycl.cool> */
/* SPDX-License-Identifier: MIT */

/*
 * qjsa:httpd -- CivetWeb as the HTTP backend for QuickJS-ng.
 *
 * Core: HTTP request -> JS handler -> HTTP response, plus the ES module
 * definition ("qjsa:httpd" when embedded, the import path when loaded as a
 * .so) used both by embedded hosts and by the .so build.
 *
 * Design notes
 * ------------
 * - QuickJS is single threaded.  Every HTTP request locks `server->js_lock`
 *   before touching the JSContext, so at most one handler runs at a time no
 *   matter how many CivetWeb worker threads exist.  Each worker calls
 *   JS_UpdateStackTop() so the engine sees the right stack bounds.
 * - The whole response (status line + headers + body) is assembled in one
 *   buffer and written with a single mg_write(): writing headers piecewise
 *   on a keep-alive connection gets stalled by Nagle + delayed ACK
 *   (measured: ~60k req/s -> 24 req/s).
 * - Nothing is printed to stdout/stderr.  Failures surface as JS exceptions
 *   or as a 500 response; the optional `onError(error)` hook sees handler
 *   exceptions before the response is flushed.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "civetweb.h"
#include "qjs_http.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

typedef struct qh_buf {
    char *data;
    size_t len;
    size_t cap;
} qh_buf;

static int qh_buf_reserve(qh_buf *b, size_t extra)
{
    size_t need, cap;
    char *p;

    if (extra > (size_t)-1 - b->len - 1)
        return -1;
    need = b->len + extra + 1;
    if (need <= b->cap)
        return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > ((size_t)-1) / 2)
            return -1;
        cap *= 2;
    }
    p = (char *)realloc(b->data, cap);
    if (!p)
        return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

static int qh_buf_add(qh_buf *b, const void *data, size_t len)
{
    if (len == 0)
        return 0;
    if (qh_buf_reserve(b, len) < 0)
        return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

static int qh_buf_adds(qh_buf *b, const char *s)
{
    return qh_buf_add(b, s, strlen(s));
}

static int qh_buf_printf(qh_buf *b, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if ((size_t)n < sizeof(tmp))
        return qh_buf_add(b, tmp, (size_t)n);
    {
        char *big = (char *)malloc((size_t)n + 1);
        int rc;
        if (!big)
            return -1;
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        rc = qh_buf_add(b, big, (size_t)n);
        free(big);
        return rc;
    }
}

static void qh_buf_free(qh_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static char *qh_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

static char *qh_strdupn(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = '\0';
    }
    return p;
}

static int qh_ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int qh_strcasecmp(const char *a, const char *b)
{
    for (;;) {
        int ca = qh_ascii_lower((unsigned char)*a++);
        int cb = qh_ascii_lower((unsigned char)*b++);
        if (ca != cb)
            return ca - cb;
        if (!ca)
            return 0;
    }
}

static const char *qh_strcasestr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!nlen)
        return hay;
    for (; *hay; hay++) {
        size_t i;
        for (i = 0; i < nlen; i++) {
            if (!hay[i] || qh_ascii_lower((unsigned char)hay[i]) !=
                               qh_ascii_lower((unsigned char)needle[i]))
                break;
        }
        if (i == nlen)
            return hay;
    }
    return NULL;
}

static const char *qh_status_text(int status)
{
    switch (status) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 422: return "Unprocessable Entity";
    case 426: return "Upgrade Required";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default: return "";
    }
}

static void qh_http_date(char *out, size_t outlen)
{
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    if (gmtime_s(&tm, &now) != 0) {
#else
    if (!gmtime_r(&now, &tm)) {
#endif
        out[0] = '\0';
        return;
    }
    strftime(out, outlen, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

/* ------------------------------------------------------------------ */
/* JS <-> C helpers                                                    */
/* ------------------------------------------------------------------ */

/* Copy a JS value to a NUL-terminated malloc()ed string. */
static char *qh_js_cstr(JSContext *ctx, JSValueConst v)
{
    const char *s;
    size_t len;
    char *out;

    s = JS_ToCStringLen(ctx, &len, v);
    if (!s)
        return NULL;
    out = qh_strdupn(s, len);
    JS_FreeCString(ctx, s);
    return out;
}

/*
 * Append a JS value to a byte buffer.  Accepts strings, ArrayBuffers and
 * typed arrays.  Returns 0 on success, -1 with a pending JS exception.
 */
static int qh_buf_add_js(JSContext *ctx, qh_buf *b, JSValueConst v)
{
    size_t len = 0;
    uint8_t *p;
    JSValue abuf;
    int rc = -1;

    if (JS_IsString(v)) {
        const char *s = JS_ToCStringLen(ctx, &len, v);
        if (!s)
            return -1;
        rc = qh_buf_add(b, s, len);
        JS_FreeCString(ctx, s);
        return rc;
    }

    if (JS_IsArrayBuffer(v)) {
        p = JS_GetArrayBuffer(ctx, &len, v);
        if (p)
            return qh_buf_add(b, p, len);
    } else {
        abuf = JS_GetTypedArrayBuffer(ctx, v, NULL, NULL, NULL);
        if (!JS_IsException(abuf)) {
            p = JS_GetArrayBuffer(ctx, &len, abuf);
            if (p)
                rc = qh_buf_add(b, p, len);
            JS_FreeValue(ctx, abuf);
            return rc;
        }
        JS_FreeValue(ctx, JS_GetException(ctx));
    }

    JS_ThrowTypeError(ctx,
                      "body must be a string, ArrayBuffer or typed array");
    return -1;
}

/* ------------------------------------------------------------------ */
/* response object                                                     */
/* ------------------------------------------------------------------ */

typedef struct qh_kv {
    char *name;
    char *value;
} qh_kv;

typedef struct qh_res {
    int status;
    bool ended;
    bool is_json;
    qh_kv *headers;
    int nheaders;
    int hcap;
    qh_buf body;
    /* streaming state (set once the headers have been written) */
    struct mg_connection *conn;
    int keep_alive;
    int sent_headers; /* status line + headers already on the wire */
    int streaming;    /* body is written as it is produced */
    int chunked;      /* streaming with Transfer-Encoding: chunked */
    int head_only;    /* HEAD / 204 / 304: headers only */
    int stream_bytes; /* total body bytes written while streaming */
} qh_res;

typedef struct qh_class_slot {
    JSRuntime *rt;
    JSClassID id;
} qh_class_slot;

static void qh_attach_res_proto(JSContext *ctx, JSClassID id);
static int qh_write_all(struct mg_connection *conn, const void *data, size_t len);
static int qh_header_block(qh_res *res, struct mg_connection *conn, int chunked,
                           int *send_body, qh_buf *out);
static int qh_stream_begin(qh_res *res);
static int qh_stream_write(qh_res *res, const void *data, size_t len);
static int qh_stream_end(qh_res *res);
static void qh_flush_response(qh_res *res, struct mg_connection *conn,
                              int keep_alive, int debug);
static const char *qh_mime_type(const char *path);

static qh_class_slot qh_class_slots[8];
static int qh_class_nslots;
static pthread_mutex_t qh_global_lock = PTHREAD_MUTEX_INITIALIZER;

static void qh_res_finalizer(JSRuntime *rt, JSValue val)
{
    qh_res *r = (qh_res *)JS_GetOpaque(val, JS_GetClassID(val));
    int i;
    (void)rt;

    if (!r)
        return;
    for (i = 0; i < r->nheaders; i++) {
        free(r->headers[i].name);
        free(r->headers[i].value);
    }
    free(r->headers);
    qh_buf_free(&r->body);
    free(r);
}

static JSClassDef qh_res_class = {
    "QJSHttpResponse",
    .finalizer = qh_res_finalizer,
};

static JSClassID qh_res_class_id(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    int i;
    JSClassID id = 0;

    pthread_mutex_lock(&qh_global_lock);
    for (i = 0; i < qh_class_nslots; i++) {
        if (qh_class_slots[i].rt == rt) {
            id = qh_class_slots[i].id;
            pthread_mutex_unlock(&qh_global_lock);
            return id;
        }
    }
    JS_NewClassID(rt, &id);
    if (JS_NewClass(rt, id, &qh_res_class) < 0) {
        pthread_mutex_unlock(&qh_global_lock);
        return 0;
    }
    qh_attach_res_proto(ctx, id);
    if (qh_class_nslots < (int)(sizeof(qh_class_slots) / sizeof(qh_class_slots[0])))
        qh_class_slots[qh_class_nslots++] = (qh_class_slot){ rt, id };
    pthread_mutex_unlock(&qh_global_lock);
    return id;
}

static qh_res *qh_res_data(JSContext *ctx, JSValueConst obj)
{
    JSClassID id = qh_res_class_id(ctx);
    qh_res *r;

    if (!id)
        return NULL;
    r = (qh_res *)JS_GetOpaque2(ctx, obj, id);
    if (!r) {
        JS_ThrowTypeError(ctx, "not a response object");
        return NULL;
    }
    return r;
}

static qh_res *qh_res_new(int status)
{
    qh_res *r = (qh_res *)calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->status = status;
    return r;
}

static void qh_res_free(qh_res *r)
{
    int i;
    if (!r)
        return;
    for (i = 0; i < r->nheaders; i++) {
        free(r->headers[i].name);
        free(r->headers[i].value);
    }
    free(r->headers);
    qh_buf_free(&r->body);
    free(r);
}

static qh_kv *qh_res_find_header(qh_res *r, const char *name)
{
    int i;
    for (i = 0; i < r->nheaders; i++) {
        if (qh_strcasecmp(r->headers[i].name, name) == 0)
            return &r->headers[i];
    }
    return NULL;
}

static int qh_res_set_header(qh_res *r, const char *name, const char *value)
{
    qh_kv *kv = qh_res_find_header(r, name);
    char *v = qh_strdup(value);

    if (!v)
        return -1;
    if (kv) {
        free(kv->value);
        kv->value = v;
        return 0;
    }
    if (r->nheaders == r->hcap) {
        int cap = r->hcap ? r->hcap * 2 : 8;
        qh_kv *p = (qh_kv *)realloc(r->headers, (size_t)cap * sizeof(*p));
        if (!p) {
            free(v);
            return -1;
        }
        r->headers = p;
        r->hcap = cap;
    }
    r->headers[r->nheaders].name = qh_strdup(name);
    if (!r->headers[r->nheaders].name) {
        free(v);
        return -1;
    }
    r->headers[r->nheaders].value = v;
    r->nheaders++;
    return 0;
}

static int qh_res_add_header(qh_res *r, const char *name, const char *value)
{
    char *n, *v;

    if (r->nheaders == r->hcap) {
        int cap = r->hcap ? r->hcap * 2 : 8;
        qh_kv *p = (qh_kv *)realloc(r->headers, (size_t)cap * sizeof(*p));
        if (!p)
            return -1;
        r->headers = p;
        r->hcap = cap;
    }
    n = qh_strdup(name);
    v = qh_strdup(value);
    if (!n || !v) {
        free(n);
        free(v);
        return -1;
    }
    r->headers[r->nheaders].name = n;
    r->headers[r->nheaders].value = v;
    r->nheaders++;
    return 0;
}

static int qh_res_remove_header(qh_res *r, const char *name)
{
    int i;
    for (i = 0; i < r->nheaders; i++) {
        if (qh_strcasecmp(r->headers[i].name, name) == 0) {
            free(r->headers[i].name);
            free(r->headers[i].value);
            memmove(&r->headers[i], &r->headers[i + 1],
                    (size_t)(r->nheaders - i - 1) * sizeof(r->headers[0]));
            r->nheaders--;
            return 1;
        }
    }
    return 0;
}

/* ---- JS methods on the response object ---- */

static JSValue js_res_set_header(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    char *name, *value;

    if (!r || argc < 2)
        return JS_EXCEPTION;
    name = qh_js_cstr(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    value = qh_js_cstr(ctx, argv[1]);
    if (!value) {
        free(name);
        return JS_EXCEPTION;
    }
    if (qh_res_set_header(r, name, value) < 0)
        JS_ThrowInternalError(ctx, "out of memory");
    free(name);
    free(value);
    return JS_UNDEFINED;
}

static JSValue js_res_get_header(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    qh_kv *kv;
    char *name;
    JSValue ret;

    if (!r || argc < 1)
        return JS_EXCEPTION;
    name = qh_js_cstr(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    kv = qh_res_find_header(r, name);
    free(name);
    if (!kv)
        return JS_UNDEFINED;
    ret = JS_NewString(ctx, kv->value);
    return ret;
}

static JSValue js_res_remove_header(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    char *name;

    if (!r || argc < 1)
        return JS_EXCEPTION;
    name = qh_js_cstr(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    qh_res_remove_header(r, name);
    free(name);
    return JS_UNDEFINED;
}

static JSValue js_res_write(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    int i;

    if (!r)
        return JS_EXCEPTION;
    if (r->ended)
        return JS_ThrowTypeError(ctx, "write() after end()");
    for (i = 0; i < argc; i++) {
        if (r->streaming) {
            qh_buf tmp = { 0 };
            if (qh_buf_add_js(ctx, &tmp, argv[i]) < 0) {
                qh_buf_free(&tmp);
                return JS_EXCEPTION;
            }
            if (qh_stream_write(r, tmp.data, tmp.len) < 0) {
                qh_buf_free(&tmp);
                return JS_ThrowInternalError(ctx, "connection closed");
            }
            qh_buf_free(&tmp);
        } else if (qh_buf_add_js(ctx, &r->body, argv[i]) < 0) {
            return JS_EXCEPTION;
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_res_end(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    int i;

    if (!r)
        return JS_EXCEPTION;
    if (r->ended)
        return JS_UNDEFINED;
    for (i = 0; i < argc; i++) {
        if (r->streaming) {
            qh_buf tmp = { 0 };
            if (qh_buf_add_js(ctx, &tmp, argv[i]) < 0) {
                qh_buf_free(&tmp);
                return JS_EXCEPTION;
            }
            qh_stream_write(r, tmp.data, tmp.len);
            qh_buf_free(&tmp);
        } else if (qh_buf_add_js(ctx, &r->body, argv[i]) < 0) {
            return JS_EXCEPTION;
        }
    }
    if (r->streaming) {
        if (qh_stream_end(r) < 0)
            return JS_ThrowInternalError(ctx, "connection closed");
        return JS_UNDEFINED;
    }
    r->ended = true;
    return JS_UNDEFINED;
}

/* res.flush(): send status line + headers now, then stream chunk by chunk. */
static JSValue js_res_flush(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);

    (void)argc;
    (void)argv;
    if (!r)
        return JS_EXCEPTION;
    if (r->ended)
        return JS_ThrowTypeError(ctx, "flush() after end()");
    if (!r->conn)
        return JS_ThrowInternalError(ctx, "flush() outside a request");
    if (!r->streaming && qh_stream_begin(r) < 0)
        return JS_ThrowInternalError(ctx, "connection closed");
    if (r->body.len > 0) {
        if (qh_stream_write(r, r->body.data, r->body.len) < 0) {
            qh_buf_free(&r->body);
            return JS_ThrowInternalError(ctx, "connection closed");
        }
        qh_buf_free(&r->body);
    }
    return JS_UNDEFINED;
}

static JSValue js_res_get_headers_sent(JSContext *ctx, JSValueConst this_val)
{
    qh_res *r = qh_res_data(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewBool(ctx, r->sent_headers != 0);
}

static JSValue js_res_has_header(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    char *name;
    int has;

    if (!r)
        return JS_EXCEPTION;
    if (argc < 1)
        return JS_FALSE;
    name = qh_js_cstr(ctx, argv[0]);
    if (!name)
        return JS_EXCEPTION;
    has = qh_res_find_header(r, name) != NULL;
    free(name);
    return JS_NewBool(ctx, has);
}

/* res.sendFile(path, {root, download}) */
static int qh_parse_range(const char *hdr, long long size, long long *start,
                          long long *end)
{
    long long a, b;
    char *endp;

    if (!hdr || strncmp(hdr, "bytes=", 6) != 0)
        return 0;
    hdr += 6;
    while (*hdr == ' ')
        hdr++;
    if (*hdr == '-') { /* suffix range: last N bytes */
        b = strtoll(hdr + 1, &endp, 10);
        if (endp == hdr + 1 || b <= 0)
            return -1;
        if (b > size)
            b = size;
        *start = size - b;
        *end = size - 1;
        return size > 0 ? 1 : -1;
    }
    a = strtoll(hdr, &endp, 10);
    if (endp == hdr || *endp != '-')
        return -1;
    *start = a;
    if (endp[1] == '\0' || endp[1] == ',') {
        *end = size - 1;
    } else {
        b = strtoll(endp + 1, &endp, 10);
        if (endp == hdr || b < a)
            return -1;
        *end = b < size ? b : size - 1;
    }
    if (*start >= size || *start > *end)
        return -1;
    return 1;
}

static JSValue js_res_send_file(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    const struct mg_request_info *ri;
    JSValue opts = argc > 1 ? argv[1] : JS_UNDEFINED;
    char *path = NULL, *root = NULL, *dl = NULL, full[4096];
    struct stat st;
    FILE *fp = NULL;
    char etag[80], lastmod[80], rng[64], clbuf[64];
    const char *inm, *ims, *rh;
    long long start = 0, end = -1, total;
    int status = 200, rc = -1;
    qh_buf out = { 0 };
    int send_body = 0;
    int is_head;

    if (!r)
        return JS_EXCEPTION;
    if (r->streaming || r->sent_headers)
        return JS_ThrowTypeError(ctx, "sendFile() after headers were sent");
    if (!r->conn)
        return JS_ThrowInternalError(ctx, "sendFile() outside a request");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "sendFile(path)");
    path = qh_js_cstr(ctx, argv[0]);
    if (!path)
        return JS_EXCEPTION;
    if (JS_IsObject(opts)) {
        JSValue v = JS_GetPropertyStr(ctx, opts, "root");
        if (JS_IsString(v))
            root = qh_js_cstr(ctx, v);
        JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "download");
        if (JS_IsString(v))
            dl = qh_js_cstr(ctx, v);
        JS_FreeValue(ctx, v);
    }
    if (root && strstr(path, "..")) {
        JS_ThrowTypeError(ctx, "sendFile(): '..' is not allowed under root");
        goto out;
    }
    if (root)
        snprintf(full, sizeof(full), "%s%s%s", root,
                 path[0] == '/' ? "" : "/", path);
    else
        snprintf(full, sizeof(full), "%s", path);

    if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
        JS_ThrowTypeError(ctx, "sendFile(): cannot read %s", full);
        goto out;
    }
    fp = fopen(full, "rb");
    if (!fp) {
        JS_ThrowTypeError(ctx, "sendFile(): cannot open %s", full);
        goto out;
    }
    total = (long long)st.st_size;
    snprintf(etag, sizeof(etag), "\"%llx-%llx\"",
             (unsigned long long)st.st_size,
             (unsigned long long)st.st_mtime);
    {
        time_t mt = st.st_mtime;
        struct tm tmv;
#ifdef _WIN32
        if (gmtime_s(&tmv, &mt) != 0)
            tmv = (struct tm){0};
#else
        gmtime_r(&mt, &tmv);
#endif
        strftime(lastmod, sizeof(lastmod), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
    }

    inm = mg_get_header(r->conn, "If-None-Match");
    ims = mg_get_header(r->conn, "If-Modified-Since");
    if ((inm && strcmp(inm, etag) == 0) ||
        (ims && strcmp(ims, lastmod) == 0)) {
        qh_res_set_header(r, "ETag", etag);
        qh_res_set_header(r, "Last-Modified", lastmod);
        r->status = 304;
        r->ended = true;
        qh_flush_response(r, r->conn, r->keep_alive, 0);
        rc = 0;
        goto out;
    }

    rh = mg_get_header(r->conn, "Range");
    end = total - 1;
    if (rh && total > 0) {
        int pr = qh_parse_range(rh, total, &start, &end);
        if (pr > 0) {
            status = 206;
        } else if (pr < 0) {
            char cr[64];
            snprintf(cr, sizeof(cr), "bytes */%lld", total);
            qh_res_set_header(r, "Content-Range", cr);
            qh_res_set_header(r, "Content-Length", "0");
            r->status = 416;
            r->ended = true;
            qh_flush_response(r, r->conn, r->keep_alive, 0);
            rc = 0;
            goto out;
        }
    }
    if (total == 0) {
        start = 0;
        end = -1;
    }

    r->status = status;
    if (!qh_res_find_header(r, "Content-Type"))
        qh_res_set_header(r, "Content-Type", qh_mime_type(full));
    qh_res_set_header(r, "Accept-Ranges", "bytes");
    qh_res_set_header(r, "ETag", etag);
    qh_res_set_header(r, "Last-Modified", lastmod);
    if (dl) {
        char cd[512];
        snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", dl);
        qh_res_set_header(r, "Content-Disposition", cd);
    }
    snprintf(clbuf, sizeof(clbuf), "%lld",
             total > 0 ? (end - start + 1) : 0);
    qh_res_set_header(r, "Content-Length", clbuf);
    if (status == 206) {
        snprintf(rng, sizeof(rng), "bytes %lld-%lld/%lld", start, end, total);
        qh_res_set_header(r, "Content-Range", rng);
    }

    ri = mg_get_request_info(r->conn);
    is_head = ri->request_method && qh_strcasecmp(ri->request_method, "HEAD") == 0;
    if (qh_header_block(r, r->conn, 0, &send_body, &out) < 0)
        goto out;
    if (qh_write_all(r->conn, out.data, out.len) < 0)
        goto out;
    r->sent_headers = 1;
    if (send_body && !is_head && total > 0) {
        char buf[65536];
        long long remaining = end - start + 1;
        if (fseek(fp, (long)start, SEEK_SET) != 0)
            goto out;
        while (remaining > 0) {
            size_t want = remaining > (long long)sizeof(buf) ? sizeof(buf)
                                                             : (size_t)remaining;
            size_t got = fread(buf, 1, want, fp);
            if (got == 0)
                break;
            if (qh_write_all(r->conn, buf, got) < 0)
                break;
            remaining -= (long long)got;
        }
    }
    r->ended = true;
    rc = 0;
out:
    if (fp)
        fclose(fp);
    free(path);
    free(root);
    free(dl);
    qh_buf_free(&out);
    return rc == 0 ? JS_UNDEFINED : JS_EXCEPTION;
}

/* res.setCookie(name, value, options) */
static JSValue js_res_set_cookie(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    char *name = NULL, *value = NULL;
    qh_buf line = { 0 };
    JSValue opts = argc > 2 ? argv[2] : JS_UNDEFINED;

    if (!r)
        return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "setCookie(name, value, options?)");
    name = qh_js_cstr(ctx, argv[0]);
    value = qh_js_cstr(ctx, argv[1]);
    if (!name || !value)
        goto fail;
    if (qh_buf_adds(&line, name) < 0 || qh_buf_adds(&line, "=") < 0 ||
        qh_buf_adds(&line, value) < 0)
        goto fail;
    {
        char *path_owned = NULL, *domain = NULL, *same_site = NULL;
        const char *path = "/";
        int max_age = 0, http_only = 0, secure = 0;

        if (JS_IsObject(opts)) {
            JSValue v = JS_GetPropertyStr(ctx, opts, "path");
            if (JS_IsString(v)) {
                path_owned = qh_js_cstr(ctx, v);
                if (path_owned && path_owned[0])
                    path = path_owned;
            }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, opts, "domain");
            if (JS_IsString(v))
                domain = qh_js_cstr(ctx, v);
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, opts, "maxAge");
            if (JS_IsNumber(v)) {
                int32_t n = 0;
                JS_ToInt32(ctx, &n, v);
                max_age = (int)n;
            }
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, opts, "httpOnly");
            http_only = JS_ToBool(ctx, v) > 0;
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, opts, "secure");
            secure = JS_ToBool(ctx, v) > 0;
            JS_FreeValue(ctx, v);
            v = JS_GetPropertyStr(ctx, opts, "sameSite");
            if (JS_IsString(v))
                same_site = qh_js_cstr(ctx, v);
            JS_FreeValue(ctx, v);
        }
        if (path && *path) {
            if (qh_buf_adds(&line, "; Path=") < 0 || qh_buf_adds(&line, path) < 0)
                goto fail;
        }
        if (domain && *domain) {
            if (qh_buf_adds(&line, "; Domain=") < 0 || qh_buf_adds(&line, domain) < 0)
                goto fail;
        }
        if (max_age != 0) {
            char nb[32], eb[80];
            time_t now = time(NULL) + max_age;
            struct tm tmv;
            snprintf(nb, sizeof(nb), "%d", max_age);
            if (qh_buf_adds(&line, "; Max-Age=") < 0 || qh_buf_adds(&line, nb) < 0)
                goto fail;
#ifdef _WIN32
            if (gmtime_s(&tmv, &now) != 0)
                tmv = (struct tm){0};
#else
            gmtime_r(&now, &tmv);
#endif
            strftime(eb, sizeof(eb), "%a, %d %b %Y %H:%M:%S GMT", &tmv);
            if (qh_buf_adds(&line, "; Expires=") < 0 || qh_buf_adds(&line, eb) < 0)
                goto fail;
        }
        if (http_only && qh_buf_adds(&line, "; HttpOnly") < 0)
            goto fail;
        if (secure && qh_buf_adds(&line, "; Secure") < 0)
            goto fail;
        if (same_site && *same_site) {
            if (qh_buf_adds(&line, "; SameSite=") < 0 ||
                qh_buf_adds(&line, same_site) < 0)
                goto fail;
        }
        free(path_owned);
        free(domain);
        free(same_site);
    }
    if (qh_res_add_header(r, "Set-Cookie", line.data) < 0)
        goto fail;
    qh_buf_free(&line);
    free(name);
    free(value);
    return JS_UNDEFINED;
fail:
    qh_buf_free(&line);
    free(name);
    free(value);
    return JS_EXCEPTION;
}

/* res.send(value) -- string, JSON-able object, or bytes */
static JSValue js_res_send(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);

    if (!r)
        return JS_EXCEPTION;
    if (r->ended || r->streaming)
        return js_res_end(ctx, this_val, argc, argv);
    if (r->ended)
        return JS_UNDEFINED;
    if (argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
        r->ended = true;
        return JS_UNDEFINED;
    }
    {
        JSValue v = argv[0];
        if (JS_IsString(v)) {
            if (!qh_res_find_header(r, "Content-Type"))
                qh_res_set_header(r, "Content-Type", "text/plain; charset=utf-8");
        } else if (JS_IsArrayBuffer(v) || JS_GetTypedArrayType(v) != 0) {
            if (!qh_res_find_header(r, "Content-Type"))
                qh_res_set_header(r, "Content-Type", "application/octet-stream");
        } else if (JS_IsObject(v)) {
            size_t len;
            JSValue s = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
            const char *c;
            int bad;
            if (JS_IsException(s))
                return JS_EXCEPTION;
            c = JS_ToCStringLen(ctx, &len, s);
            if (!c) {
                JS_FreeValue(ctx, s);
                return JS_EXCEPTION;
            }
            bad = qh_buf_add(&r->body, c, len);
            JS_FreeCString(ctx, c);
            JS_FreeValue(ctx, s);
            if (bad < 0)
                return JS_ThrowInternalError(ctx, "out of memory");
            if (!qh_res_find_header(r, "Content-Type"))
                qh_res_set_header(r, "Content-Type", "application/json; charset=utf-8");
            r->ended = true;
            return JS_UNDEFINED;
        }
        if (qh_buf_add_js(ctx, &r->body, v) < 0)
            return JS_EXCEPTION;
    }
    r->ended = true;
    return JS_UNDEFINED;
}

static JSValue js_res_type(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    char *value;

    if (!r || argc < 1)
        return JS_EXCEPTION;
    value = qh_js_cstr(ctx, argv[0]);
    if (!value)
        return JS_EXCEPTION;
    if (qh_res_set_header(r, "Content-Type", value) < 0)
        JS_ThrowInternalError(ctx, "out of memory");
    free(value);
    return JS_UNDEFINED;
}

static JSValue js_res_json(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    size_t len;
    JSValue s;
    int rc;

    if (!r)
        return JS_EXCEPTION;
    if (r->ended)
        return JS_ThrowTypeError(ctx, "json() after end()");
    s = JS_JSONStringify(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, JS_UNDEFINED,
                         JS_UNDEFINED);
    if (JS_IsException(s))
        return JS_EXCEPTION;
    if (!JS_IsString(s)) {
        JS_FreeValue(ctx, s);
        return JS_ThrowTypeError(ctx, "json: value is not serializable");
    }
    {
        const char *c = JS_ToCStringLen(ctx, &len, s);
        if (!c) {
            JS_FreeValue(ctx, s);
            return JS_EXCEPTION;
        }
        rc = qh_buf_add(&r->body, c, len);
        JS_FreeCString(ctx, c);
    }
    JS_FreeValue(ctx, s);
    if (rc < 0)
        return JS_ThrowInternalError(ctx, "out of memory");
    if (!qh_res_find_header(r, "Content-Type"))
        qh_res_set_header(r, "Content-Type", "application/json; charset=utf-8");
    r->ended = true;
    return JS_UNDEFINED;
}

static JSValue js_res_redirect(JSContext *ctx, JSValueConst this_val, int argc,
                               JSValueConst *argv)
{
    qh_res *r = qh_res_data(ctx, this_val);
    char *url;
    int32_t status = 302;

    if (!r || argc < 1)
        return JS_EXCEPTION;
    if (argc > 1 && JS_ToInt32(ctx, &status, argv[1]) < 0)
        return JS_EXCEPTION;
    if (status < 300 || status > 399) {
        return JS_ThrowRangeError(ctx, "redirect status must be 3xx");
    }
    url = qh_js_cstr(ctx, argv[0]);
    if (!url)
        return JS_EXCEPTION;
    r->status = status;
    if (qh_res_set_header(r, "Location", url) < 0)
        JS_ThrowInternalError(ctx, "out of memory");
    free(url);
    r->ended = true;
    return JS_UNDEFINED;
}

static JSValue js_res_get_status(JSContext *ctx, JSValueConst this_val)
{
    qh_res *r = qh_res_data(ctx, this_val);
    if (!r)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, r->status);
}

static JSValue js_res_set_status(JSContext *ctx, JSValueConst this_val,
                                 JSValueConst val)
{
    qh_res *r = qh_res_data(ctx, this_val);
    int32_t status;

    if (!r)
        return JS_EXCEPTION;
    if (JS_ToInt32(ctx, &status, val) < 0)
        return JS_EXCEPTION;
    if (status < 100 || status > 999)
        return JS_ThrowRangeError(ctx, "invalid HTTP status %d", status);
    r->status = status;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qh_res_proto_funcs[] = {
    JS_CFUNC_DEF("setHeader", 2, js_res_set_header),
    JS_CFUNC_DEF("getHeader", 1, js_res_get_header),
    JS_CFUNC_DEF("removeHeader", 1, js_res_remove_header),
    JS_CFUNC_DEF("write", 1, js_res_write),
    JS_CFUNC_DEF("end", 0, js_res_end),
    JS_CFUNC_DEF("send", 1, js_res_send),
    JS_CFUNC_DEF("type", 1, js_res_type),
    JS_CFUNC_DEF("json", 1, js_res_json),
    JS_CFUNC_DEF("redirect", 1, js_res_redirect),
    JS_CFUNC_DEF("flush", 0, js_res_flush),
    JS_CFUNC_DEF("sendFile", 2, js_res_send_file),
    JS_CFUNC_DEF("setCookie", 3, js_res_set_cookie),
    JS_CFUNC_DEF("hasHeader", 1, js_res_has_header),
    JS_CGETSET_DEF("status", js_res_get_status, js_res_set_status),
    JS_CGETSET_DEF("headersSent", js_res_get_headers_sent, NULL),
};

static void qh_attach_res_proto(JSContext *ctx, JSClassID id)
{
    JSValue proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return;
    JS_SetPropertyFunctionList(ctx, proto, qh_res_proto_funcs,
                               (int)(sizeof(qh_res_proto_funcs) /
                                     sizeof(qh_res_proto_funcs[0])));
    JS_SetClassProto(ctx, id, proto);
}

/* ------------------------------------------------------------------ */
/* request object                                                      */
/* ------------------------------------------------------------------ */

static void qh_lower(char *s)
{
    for (; *s; s++)
        *s = (char)qh_ascii_lower((unsigned char)*s);
}
/* ------------------------------------------------------------------ */
/* url decoding, query strings, cookies, MIME types                    */
/* ------------------------------------------------------------------ */

static int qh_hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Decode %XX and '+' (form encoding).  Returns a fresh NUL-terminated
 * string; the byte length is stored in *outlen so NUL bytes survive. */
static char *qh_url_decode(const char *s, size_t len, size_t *outlen)
{
    char *out = (char *)malloc(len + 1);
    size_t i, o = 0;

    if (!out)
        return NULL;
    for (i = 0; i < len; i++) {
        int c = (unsigned char)s[i];
        if (c == '+') {
            out[o++] = ' ';
        } else if (c == '%' && i + 2 < len && qh_hexval((unsigned char)s[i + 1]) >= 0 &&
                   qh_hexval((unsigned char)s[i + 2]) >= 0) {
            out[o++] = (char)((qh_hexval((unsigned char)s[i + 1]) << 4) |
                              qh_hexval((unsigned char)s[i + 2]));
            i += 2;
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    if (outlen)
        *outlen = o;
    return out;
}

/* Set obj[key] = val; repeated keys become arrays.  Consumes `val`. */
static void qh_param_add(JSContext *ctx, JSValue obj, const char *key,
                         JSValue val)
{
    JSValue old = JS_GetPropertyStr(ctx, obj, key);

    if (JS_IsException(old)) {
        JS_FreeValue(ctx, old);
        JS_FreeValue(ctx, val);
        return;
    }
    if (JS_IsUndefined(old)) {
        JS_SetPropertyStr(ctx, obj, key, val);
        JS_FreeValue(ctx, old);
        return;
    }
    if (JS_IsArray(old)) {
        JSValue lenv = JS_GetPropertyStr(ctx, old, "length");
        uint32_t n = 0;
        JS_ToUint32(ctx, &n, lenv);
        JS_FreeValue(ctx, lenv);
        JS_SetPropertyUint32(ctx, old, n, val);
        JS_FreeValue(ctx, old);
        return;
    }
    {
        JSValue arr = JS_NewArray(ctx);
        if (JS_IsException(arr)) {
            JS_FreeValue(ctx, old);
            JS_FreeValue(ctx, val);
            return;
        }
        JS_SetPropertyUint32(ctx, arr, 0, old);
        JS_SetPropertyUint32(ctx, arr, 1, val);
        JS_SetPropertyStr(ctx, obj, key, arr);
    }
}

/* application/x-www-form-urlencoded -> JS object */
static void qh_parse_urlencoded(JSContext *ctx, JSValue obj, const char *s,
                                size_t len)
{
    size_t i = 0;

    while (i <= len) {
        size_t start = i, eq = (size_t)-1;
        size_t klen, vlen;
        char *key, *value;

        while (i < len && s[i] != '&') {
            if (s[i] == '=' && eq == (size_t)-1)
                eq = i;
            i++;
        }
        if (eq == (size_t)-1)
            eq = i;
        key = qh_url_decode(s + start, eq - start, &klen);
        value = qh_url_decode(eq < i ? s + eq + 1 : s + i,
                              eq < i ? i - eq - 1 : 0, &vlen);
        if (key && key[0])
            qh_param_add(ctx, obj, key,
                         JS_NewStringLen(ctx, value ? value : "", vlen));
        free(key);
        free(value);
        i++; /* skip '&' */
    }
}

/* Cookie header -> JS object (first occurrence of a name wins). */
static void qh_parse_cookies(JSContext *ctx, JSValue cookies, const char *s)
{
    while (s && *s) {
        const char *p = s;
        const char *name, *value;
        char *n, *v;

        while (*s && *s != ';')
            s++;
        while (p < s && (*p == ' ' || *p == '\t'))
            p++;
        name = p;
        while (p < s && *p != '=')
            p++;
        if (p < s) {
            value = p + 1;
            n = qh_strdupn(name, (size_t)(p - name));
            v = qh_strdupn(value, (size_t)(s - value));
            if (n && v && n[0]) {
                JSValue old = JS_GetPropertyStr(ctx, cookies, n);
                if (JS_IsUndefined(old))
                    JS_SetPropertyStr(ctx, cookies, n, JS_NewString(ctx, v));
                JS_FreeValue(ctx, old);
            }
            free(n);
            free(v);
        }
        if (*s == ';')
            s++;
    }
}

static const char *qh_mime_type(const char *path)
{
    static const struct {
        const char *ext;
        const char *type;
    } types[] = {
        { "html", "text/html; charset=utf-8" },
        { "htm", "text/html; charset=utf-8" },
        { "css", "text/css; charset=utf-8" },
        { "js", "text/javascript; charset=utf-8" },
        { "mjs", "text/javascript; charset=utf-8" },
        { "json", "application/json; charset=utf-8" },
        { "txt", "text/plain; charset=utf-8" },
        { "md", "text/markdown; charset=utf-8" },
        { "csv", "text/csv; charset=utf-8" },
        { "xml", "application/xml; charset=utf-8" },
        { "svg", "image/svg+xml" },
        { "png", "image/png" },
        { "jpg", "image/jpeg" },
        { "jpeg", "image/jpeg" },
        { "gif", "image/gif" },
        { "webp", "image/webp" },
        { "ico", "image/x-icon" },
        { "woff", "font/woff" },
        { "woff2", "font/woff2" },
        { "ttf", "font/ttf" },
        { "otf", "font/otf" },
        { "wasm", "application/wasm" },
        { "pdf", "application/pdf" },
        { "zip", "application/zip" },
        { "gz", "application/gzip" },
        { "mp4", "video/mp4" },
        { "webm", "video/webm" },
        { "mp3", "audio/mpeg" },
        { "wav", "audio/wav" },
    };
    const char *dot = strrchr(path, '.');
    size_t i;

    if (dot) {
        for (i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
            if (qh_strcasecmp(dot + 1, types[i].ext) == 0)
                return types[i].type;
        }
    }
    return "application/octet-stream";
}

static const uint8_t *qh_memmem(const uint8_t *hay, size_t hlen,
                                const void *needle, size_t nlen)
{
    const uint8_t *n = (const uint8_t *)needle;

    if (nlen == 0 || hlen < nlen)
        return NULL;
    while (hlen >= nlen) {
        const uint8_t *p = (const uint8_t *)memchr(hay, n[0], hlen - nlen + 1);
        if (!p)
            return NULL;
        if (memcmp(p, n, nlen) == 0)
            return p;
        hlen -= (size_t)(p - hay) + 1;
        hay = p + 1;
    }
    return NULL;
}

/* Extract a quoted parameter (name="value") from a header line. */
static char *qh_header_param(const char *line, const char *param)
{
    const char *p = qh_strcasestr(line, param);
    const char *start, *end;
    char *out;

    if (!p)
        return NULL;
    p += strlen(param);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '=')
        return NULL;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return qh_strdup(p);
    start = ++p;
    while (*p && *p != '"')
        p++;
    end = p;
    out = qh_strdupn(start, (size_t)(end - start));
    return out;
}

/* multipart/form-data -> JS object */
static JSValue qh_parse_multipart(JSContext *ctx, const uint8_t *data,
                                  size_t len, const char *boundary)
{
    JSValue obj = JS_NewObject(ctx);
    char *mark;
    size_t blen = strlen(boundary), mlen;
    const uint8_t *p;

    if (JS_IsException(obj))
        return obj;
    if (blen == 0 || blen > 200) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowTypeError(ctx, "invalid multipart boundary");
    }
    mark = (char *)malloc(blen + 5);
    if (!mark) {
        JS_FreeValue(ctx, obj);
        return JS_ThrowInternalError(ctx, "out of memory");
    }
    snprintf(mark, blen + 5, "--%s", boundary);
    mlen = blen + 2;

    p = qh_memmem(data, len, mark, mlen);
    while (p) {
        const uint8_t *cur = p + mlen;
        const uint8_t *hend, *dstart, *next;
        const uint8_t *eol = (const uint8_t *)"\r\n\r\n";
        char *headers, *line, *name = NULL, *filename = NULL, *ctype = NULL;
        size_t hlen, dlen;
        size_t rest;

        if ((size_t)(cur - data) + 2 > len)
            break;
        if (cur[0] == '-' && cur[1] == '-')
            break; /* closing boundary */
        if (cur[0] == '\r' && cur[1] == '\n')
            cur += 2;
        rest = len - (size_t)(cur - data);
        if (rest < 4)
            break;
        hend = qh_memmem(cur, rest, eol, 4);
        if (!hend)
            break;
        hlen = (size_t)(hend - cur);
        headers = (char *)malloc(hlen + 1);
        if (!headers)
            break;
        memcpy(headers, cur, hlen);
        headers[hlen] = '\0';
        {
            char *save = NULL;
            for (line = strtok_r(headers, "\r\n", &save); line;
                 line = strtok_r(NULL, "\r\n", &save)) {
                if (qh_strcasestr(line, "Content-Disposition:")) {
                    name = qh_header_param(line, "name");
                    filename = qh_header_param(line, "filename");
                } else if (qh_strcasestr(line, "Content-Type:")) {
                    const char *v = strchr(line, ':');
                    if (v) {
                        v++;
                        while (*v == ' ' || *v == '\t')
                            v++;
                        ctype = qh_strdup(v);
                    }
                }
            }
        }
        free(headers);
        dstart = hend + 4;
        {
            char *close_mark = (char *)malloc(mlen + 3);
            size_t cmlen = mlen + 2;
            if (!close_mark) {
                free(name);
                free(filename);
                free(ctype);
                break;
            }
            snprintf(close_mark, mlen + 3, "\r\n%s", mark);
            next = qh_memmem(dstart, len - (size_t)(dstart - data),
                             close_mark, cmlen);
            free(close_mark);
        }
        dlen = next ? (size_t)(next - dstart)
                    : len - (size_t)(dstart - data);

        if (name && name[0]) {
            if (filename) {
                JSValue file = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, file, "filename", JS_NewString(ctx, filename));
                JS_SetPropertyStr(ctx, file, "contentType",
                                  JS_NewString(ctx, ctype ? ctype : "application/octet-stream"));
                JS_SetPropertyStr(ctx, file, "size", JS_NewInt64(ctx, (int64_t)dlen));
                JS_SetPropertyStr(ctx, file, "data",
                                  JS_NewUint8ArrayCopy(ctx, dstart, dlen));
                qh_param_add(ctx, obj, name, file);
            } else {
                qh_param_add(ctx, obj, name, JS_NewStringLen(ctx, (const char *)dstart, dlen));
            }
        }
        free(name);
        free(filename);
        free(ctype);
        if (!next)
            break;
        p = next + 2; /* skip CRLF, point at "--boundary" */
    }
    free(mark);
    return obj;
}


static JSValue js_req_header(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    JSValue headers, hv;
    char *name;

    if (argc < 1)
        return JS_UNDEFINED;
    headers = JS_GetPropertyStr(ctx, this_val, "headers");
    if (JS_IsException(headers))
        return headers;
    name = qh_js_cstr(ctx, argv[0]);
    if (!name) {
        JS_FreeValue(ctx, headers);
        return JS_EXCEPTION;
    }
    qh_lower(name);
    hv = JS_GetPropertyStr(ctx, headers, name);
    free(name);
    JS_FreeValue(ctx, headers);
    return hv;
}

static int qh_strncasecmp(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = qh_ascii_lower((unsigned char)a[i]);
        int cb = qh_ascii_lower((unsigned char)b[i]);
        if (ca != cb || ca == 0)
            return ca - cb;
    }
    return 0;
}

/* req.json() */
static JSValue js_req_json(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    JSValue body = JS_GetPropertyStr(ctx, this_val, "body");
    const char *s;
    size_t len;
    JSValue v;

    (void)argc;
    (void)argv;
    if (JS_IsException(body))
        return body;
    s = JS_ToCStringLen(ctx, &len, body);
    JS_FreeValue(ctx, body);
    if (!s)
        return JS_EXCEPTION;
    v = JS_ParseJSON(ctx, s, len, "<request body>");
    JS_FreeCString(ctx, s);
    return v;
}

/* req.form() -- application/x-www-form-urlencoded and multipart/form-data */
static JSValue js_req_form(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    JSValue ctv = JS_GetPropertyStr(ctx, this_val, "contentType");
    char *ctype;
    JSValue out;

    (void)argc;
    (void)argv;
    if (JS_IsException(ctv))
        return ctv;
    ctype = qh_js_cstr(ctx, ctv);
    JS_FreeValue(ctx, ctv);
    if (!ctype)
        return JS_EXCEPTION;

    if (qh_strncasecmp(ctype, "application/x-www-form-urlencoded",
                       strlen("application/x-www-form-urlencoded")) == 0) {
        JSValue body = JS_GetPropertyStr(ctx, this_val, "body");
        const char *s;
        size_t len;
        out = JS_NewObject(ctx);
        if (!JS_IsException(out) && !JS_IsException(body)) {
            s = JS_ToCStringLen(ctx, &len, body);
            if (s) {
                qh_parse_urlencoded(ctx, out, s, len);
                JS_FreeCString(ctx, s);
            }
        }
        JS_FreeValue(ctx, body);
        free(ctype);
        return out;
    }
    if (qh_strncasecmp(ctype, "multipart/form-data", strlen("multipart/form-data")) == 0) {
        char *b = (char *)qh_strcasestr(ctype, "boundary=");
        JSValue bytes = JS_GetPropertyStr(ctx, this_val, "bodyBytes");
        size_t len = 0;
        uint8_t *data;

        free(ctype);
        if (!b) {
            JS_FreeValue(ctx, bytes);
            return JS_ThrowTypeError(ctx, "multipart body without boundary");
        }
        b += strlen("boundary=");
        if (*b == '"')
            b++;
        {
            char *e = b;
            while (*e && *e != '"' && *e != ';')
                e++;
            *e = '\0';
        }
        data = JS_GetUint8Array(ctx, &len, bytes);
        if (!data) {
            JS_FreeValue(ctx, bytes);
            return JS_EXCEPTION;
        }
        out = qh_parse_multipart(ctx, data, len, b);
        JS_FreeValue(ctx, bytes);
        return out;
    }
    free(ctype);
    return JS_ThrowTypeError(ctx,
                             "unsupported content type for form(): %s",
                             "");
}

/* req.cookie(name) */
static JSValue js_req_cookie(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    JSValue cookies;

    if (argc < 1)
        return JS_UNDEFINED;
    cookies = JS_GetPropertyStr(ctx, this_val, "cookies");
    if (JS_IsException(cookies))
        return cookies;
    {
        char *name = qh_js_cstr(ctx, argv[0]);
        JSValue v;
        if (!name) {
            JS_FreeValue(ctx, cookies);
            return JS_EXCEPTION;
        }
        v = JS_GetPropertyStr(ctx, cookies, name);
        free(name);
        JS_FreeValue(ctx, cookies);
        return v;
    }
}

/* req.is(type) */
static JSValue js_req_is(JSContext *ctx, JSValueConst this_val, int argc,
                         JSValueConst *argv)
{
    JSValue ctv;
    char *ctype, *want;
    int match = 0;

    if (argc < 1)
        return JS_FALSE;
    ctv = JS_GetPropertyStr(ctx, this_val, "contentType");
    if (JS_IsException(ctv))
        return ctv;
    ctype = qh_js_cstr(ctx, ctv);
    JS_FreeValue(ctx, ctv);
    want = qh_js_cstr(ctx, argv[0]);
    if (!ctype || !want) {
        free(ctype);
        free(want);
        return JS_EXCEPTION;
    }
    if (qh_strcasecmp(want, "json") == 0)
        want = (free(want), qh_strdup("application/json"));
    else if (qh_strcasecmp(want, "form") == 0 || qh_strcasecmp(want, "urlencoded") == 0)
        want = (free(want), qh_strdup("application/x-www-form-urlencoded"));
    else if (qh_strcasecmp(want, "multipart") == 0)
        want = (free(want), qh_strdup("multipart/form-data"));
    else if (qh_strcasecmp(want, "html") == 0)
        want = (free(want), qh_strdup("text/html"));
    if (want && want[0])
        match = qh_strncasecmp(ctype, want, strlen(want)) == 0;
    free(ctype);
    free(want);
    return JS_NewBool(ctx, match);
}

static JSValue qh_build_request(JSContext *ctx, struct mg_connection *conn,
                                const qh_buf *body)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    JSValue obj, headers, fn;
    const char *uri = ri->request_uri ? ri->request_uri : "/";
    const char *path = ri->local_uri ? ri->local_uri : "/";
    const char *query = ri->query_string ? ri->query_string : "";
    int i;

    obj = JS_NewObject(ctx);
    if (JS_IsException(obj))
        return obj;
    headers = JS_NewObject(ctx);
    if (JS_IsException(headers)) {
        JS_FreeValue(ctx, obj);
        return headers;
    }

    for (i = 0; i < ri->num_headers && i < MG_MAX_HEADERS; i++) {
        char *name = qh_strdup(ri->http_headers[i].name ? ri->http_headers[i].name : "");
        JSValue val;
        if (!name)
            continue;
        qh_lower(name);
        val = JS_NewString(ctx, ri->http_headers[i].value ? ri->http_headers[i].value : "");
        JS_SetPropertyStr(ctx, headers, name, val);
        free(name);
    }

    JS_SetPropertyStr(ctx, obj, "method",
                      JS_NewString(ctx, ri->request_method ? ri->request_method : "GET"));
    JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, uri));
    JS_SetPropertyStr(ctx, obj, "path", JS_NewString(ctx, path));
    JS_SetPropertyStr(ctx, obj, "query", JS_NewString(ctx, query));
    JS_SetPropertyStr(ctx, obj, "httpVersion",
                      JS_NewString(ctx, ri->http_version ? ri->http_version : "1.1"));
    JS_SetPropertyStr(ctx, obj, "remoteAddr", JS_NewString(ctx, ri->remote_addr));
    JS_SetPropertyStr(ctx, obj, "headers", headers);

    JS_SetPropertyStr(ctx, obj, "body", JS_NewStringLen(ctx, body->data ? body->data : "", body->len));
    JS_SetPropertyStr(ctx, obj, "bodyBytes",
                      JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(body->data ? body->data : ""),
                                           body->len));

    /* query string parsed into an object (repeated keys -> arrays) */
    {
        JSValue qp = JS_NewObject(ctx);
        if (!JS_IsException(qp)) {
            if (query[0])
                qh_parse_urlencoded(ctx, qp, query, strlen(query));
            JS_SetPropertyStr(ctx, obj, "queryParams", qp);
        }
    }

    /* cookies */
    {
        JSValue cookies = JS_NewObject(ctx);
        if (!JS_IsException(cookies)) {
            for (i = 0; i < ri->num_headers && i < MG_MAX_HEADERS; i++) {
                if (ri->http_headers[i].name &&
                    qh_strcasecmp(ri->http_headers[i].name, "Cookie") == 0) {
                    qh_parse_cookies(ctx, cookies, ri->http_headers[i].value);
                }
            }
            JS_SetPropertyStr(ctx, obj, "cookies", cookies);
        }
    }

    {
        const char *ct = mg_get_header(conn, "Content-Type");
        JS_SetPropertyStr(ctx, obj, "contentType", JS_NewString(ctx, ct ? ct : ""));
        JS_SetPropertyStr(ctx, obj, "contentLength", JS_NewInt64(ctx, ri->content_length));
    }

    fn = JS_NewCFunction(ctx, js_req_header, "header", 1);
    JS_SetPropertyStr(ctx, obj, "header", fn);
    fn = JS_NewCFunction(ctx, js_req_json, "json", 0);
    JS_SetPropertyStr(ctx, obj, "json", fn);
    fn = JS_NewCFunction(ctx, js_req_form, "form", 0);
    JS_SetPropertyStr(ctx, obj, "form", fn);
    fn = JS_NewCFunction(ctx, js_req_cookie, "cookie", 1);
    JS_SetPropertyStr(ctx, obj, "cookie", fn);
    fn = JS_NewCFunction(ctx, js_req_is, "is", 1);
    JS_SetPropertyStr(ctx, obj, "is", fn);
    return obj;
}

/* ------------------------------------------------------------------ */
/* response serialisation                                              */
/* ------------------------------------------------------------------ */

static int qh_line(qh_buf *out, const char *name, const char *value)
{
    if (qh_buf_adds(out, name) < 0)
        return -1;
    if (qh_buf_adds(out, ": ") < 0)
        return -1;
    if (qh_buf_adds(out, value) < 0)
        return -1;
    return qh_buf_adds(out, "\r\n");
}

static int qh_has_body(int status)
{
    return status >= 200 && status != 204 && status != 304;
}

static int qh_write_all(struct mg_connection *conn, const void *data, size_t len)
{
    const char *p = (const char *)data;

    while (len > 0) {
        int n = mg_write(conn, p, len);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Build status line + headers.  `chunked` selects Transfer-Encoding
 * instead of Content-Length.  *send_body tells whether a body follows. */
static int qh_header_block(qh_res *res, struct mg_connection *conn, int chunked,
                           int *send_body, qh_buf *out)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *hv = ri->http_version ? ri->http_version : "1.1";
    int is_head = ri->request_method && qh_strcasecmp(ri->request_method, "HEAD") == 0;
    int body_ok = qh_has_body(res->status);
    int client_close = 0;
    const char *ch = mg_get_header(conn, "Connection");
    char date[64];
    int i;

    *send_body = body_ok && !is_head;
    if (ch && qh_strcasestr(ch, "close"))
        client_close = 1;

    {
        const char *reason = qh_status_text(res->status);
        int rc = reason[0]
                     ? qh_buf_printf(out, "HTTP/%s %d %s\r\n", hv,
                                     res->status, reason)
                     : qh_buf_printf(out, "HTTP/%s %d\r\n", hv, res->status);
        if (rc < 0)
            return -1;
    }

    for (i = 0; i < res->nheaders; i++) {
        if (qh_line(out, res->headers[i].name, res->headers[i].value) < 0)
            return -1;
    }

    if (!qh_res_find_header(res, "Server")) {
        if (qh_line(out, "Server", "qjs-http/" QJS_HTTP_VERSION " (civetweb/" CIVETWEB_VERSION ")") < 0)
            return -1;
    }
    qh_http_date(date, sizeof(date));
    if (date[0] && !qh_res_find_header(res, "Date")) {
        if (qh_line(out, "Date", date) < 0)
            return -1;
    }
    if (!qh_res_find_header(res, "Content-Type") && *send_body && res->body.len > 0) {
        if (qh_line(out, "Content-Type", "text/plain; charset=utf-8") < 0)
            return -1;
    }
    if (chunked) {
        if (!qh_res_find_header(res, "Transfer-Encoding") &&
            qh_line(out, "Transfer-Encoding", "chunked") < 0)
            return -1;
    } else if (!qh_res_find_header(res, "Content-Length") && body_ok) {
        if (qh_buf_printf(out, "Content-Length: %zu\r\n", res->body.len) < 0)
            return -1;
    }
    if (!qh_res_find_header(res, "Connection")) {
        int close = !res->keep_alive || client_close ||
                    (qh_strcasecmp(hv, "1.0") == 0 &&
                     !(ch && qh_strcasestr(ch, "keep-alive")));
        if (close && qh_line(out, "Connection", "close") < 0)
            return -1;
    }
    return qh_buf_adds(out, "\r\n");
}

static void qh_flush_response(qh_res *res, struct mg_connection *conn,
                              int keep_alive, int debug)
{
    qh_buf out = { 0 };
    int send_body = 0;

    (void)debug;
    if (res->sent_headers) {
        qh_buf_free(&out);
        return;
    }
    res->conn = conn;
    res->keep_alive = keep_alive;
    if (qh_header_block(res, conn, 0, &send_body, &out) < 0)
        goto out;
    if (send_body && res->body.len > 0 &&
        qh_buf_add(&out, res->body.data, res->body.len) < 0)
        goto out;
    qh_write_all(conn, out.data, out.len);
    res->sent_headers = 1;
    res->stream_bytes = (int)res->body.len;
out:
    qh_buf_free(&out);
}

/* Start streaming: headers go out now, body bytes follow as they come.
 * Uses chunked encoding unless the caller set Content-Length. */
static int qh_stream_begin(qh_res *res)
{
    qh_buf out = { 0 };
    int send_body = 0;

    if (res->sent_headers) {
        res->streaming = 1;
        return 0;
    }
    if (!res->conn)
        return -1;
    res->chunked = qh_res_find_header(res, "Content-Length") == NULL;
    if (qh_header_block(res, res->conn, res->chunked, &send_body, &out) < 0) {
        qh_buf_free(&out);
        return -1;
    }
    if (qh_write_all(res->conn, out.data, out.len) < 0) {
        qh_buf_free(&out);
        return -1;
    }
    qh_buf_free(&out);
    res->sent_headers = 1;
    res->streaming = 1;
    res->head_only = !send_body;
    return 0;
}

static int qh_stream_write(qh_res *res, const void *data, size_t len)
{
    if (!res->streaming && qh_stream_begin(res) < 0)
        return -1;
    if (len == 0 || res->head_only || !qh_has_body(res->status))
        return 0;
    if (res->chunked) {
        char hdr[40];
        int n = snprintf(hdr, sizeof(hdr), "%zx\r\n", len);
        if (qh_write_all(res->conn, hdr, (size_t)n) < 0)
            return -1;
        if (qh_write_all(res->conn, data, len) < 0)
            return -1;
        if (qh_write_all(res->conn, "\r\n", 2) < 0)
            return -1;
    } else if (qh_write_all(res->conn, data, len) < 0) {
        return -1;
    }
    res->stream_bytes += (int)len;
    return 0;
}

static int qh_stream_end(qh_res *res)
{
    if (!res->streaming)
        return 0;
    if (res->chunked && !res->head_only && qh_has_body(res->status)) {
        if (qh_write_all(res->conn, "0\r\n\r\n", 5) < 0)
            return -1;
    }
    res->ended = true;
    return 0;
}

static void qh_send_simple(struct mg_connection *conn, int status,
                           const char *text, int keep_alive)
{
    qh_res *r = qh_res_new(status);
    if (!r)
        return;
    if (text && *text)
        qh_buf_adds(&r->body, text);
    qh_flush_response(r, conn, keep_alive, 0);
    qh_res_free(r);
}

/* ------------------------------------------------------------------ */
/* server                                                              */
/* ------------------------------------------------------------------ */

enum {
    QH_BODY_OK = 0,
    QH_BODY_TOO_LARGE = 1,
    QH_BODY_ERROR = -1,
};

struct qh_server;

typedef struct qh_ws_route {
    struct qh_ws_route *next;
    struct qh_server *s;
    char *uri;
    JSValue handler;
} qh_ws_route;

typedef struct qh_ws_conn {
    struct qh_ws_conn *next;
    struct mg_connection *conn;
    JSValue obj;
} qh_ws_conn;

typedef struct qh_server {
    struct qh_server *next;
    JSContext *ctx;
    JSValue handler;
    JSValue on_error;
    char *port;
    char *binding;
    const char *const *extra;
    int threads;
    int backlog;
    int keep_alive;
    int tcp_nodelay;
    int request_timeout_ms;
    int debug;
    int grab_signals;
    size_t max_body;
    struct mg_context *mg;
    qh_ws_route *ws_routes;
    qh_ws_conn *ws_conns;
    pthread_mutex_t js_lock;
    pthread_mutex_t wait_lock;
    pthread_cond_t cond;
    int stopping;
} qh_server;

static qh_server *g_servers;
static pthread_mutex_t g_servers_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t g_signal_stop;

static void qh_signal_handler(int sig)
{
    (void)sig;
    g_signal_stop = 1;
}

static void qh_install_signals(void)
{
    static int installed;

    if (installed)
        return;
    installed = 1;
#ifdef _WIN32
    signal(SIGINT, qh_signal_handler);
    signal(SIGTERM, qh_signal_handler);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = qh_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
#endif
}

static int qh_read_body(struct mg_connection *conn,
                        const struct mg_request_info *ri, qh_buf *out,
                        size_t max)
{
    long long cl = ri->content_length;
    size_t target;
    char chunk[8192];

    if (cl == 0)
        return QH_BODY_OK;
    if (cl > 0) {
        if ((unsigned long long)cl > (unsigned long long)max)
            return QH_BODY_TOO_LARGE;
        target = (size_t)cl;
    } else {
        target = max; /* chunked or unspecified */
    }
    while (out->len < target) {
        size_t want = target - out->len;
        int n;
        if (want > sizeof(chunk))
            want = sizeof(chunk);
        n = mg_read(conn, chunk, want);
        if (n < 0)
            return QH_BODY_ERROR;
        if (n == 0)
            break;
        if (qh_buf_add(out, chunk, (size_t)n) < 0)
            return QH_BODY_ERROR;
    }
    if (cl < 0 && out->len >= max)
        return QH_BODY_TOO_LARGE;
    return QH_BODY_OK;
}

static JSValue qh_new_response_object(JSContext *ctx, qh_res *res)
{
    JSClassID id = qh_res_class_id(ctx);
    JSValue obj;

    if (!id)
        return JS_EXCEPTION;
    obj = JS_NewObjectClass(ctx, id);
    if (JS_IsException(obj))
        return obj;
    JS_SetOpaque(obj, res);
    return obj;
}

/* Turn the pending JS exception into a 500 response (and onError call). */
static void qh_fail_request(qh_server *s, qh_res *res)
{
    JSContext *ctx = s->ctx;
    JSValue exc = JS_GetException(ctx);
    char *msg = qh_js_cstr(ctx, exc);

    if (JS_IsFunction(ctx, s->on_error)) {
        JSValue r = JS_Call(ctx, s->on_error, JS_UNDEFINED, 1, &exc);
        if (JS_IsException(r))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, r);
    }

    qh_buf_free(&res->body);
    res->status = 500;
    qh_res_remove_header(res, "Content-Type");
    qh_res_remove_header(res, "Content-Length");
    qh_res_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    qh_buf_adds(&res->body,
                (s->debug && msg && *msg) ? msg : "Internal Server Error");
    res->ended = true;
    free(msg);
    JS_FreeValue(ctx, exc);
}

static int qh_serve_one(qh_server *s, struct mg_connection *conn)
{
    const struct mg_request_info *ri = mg_get_request_info(conn);
    qh_res *res;
    qh_buf body = { 0 };
    JSValue req = JS_UNDEFINED, robj = JS_UNDEFINED, ret = JS_UNDEFINED;
    int rc;

    res = qh_res_new(200);
    if (!res) {
        qh_send_simple(conn, 500, "Internal Server Error", s->keep_alive);
        return 1;
    }

    rc = qh_read_body(conn, ri, &body, s->max_body);
    if (rc == QH_BODY_TOO_LARGE) {
        qh_buf_free(&body);
        qh_res_free(res);
        qh_send_simple(conn, 413, "Payload Too Large", s->keep_alive);
        return 413;
    }
    if (rc == QH_BODY_ERROR) {
        qh_buf_free(&body);
        qh_res_free(res);
        qh_send_simple(conn, 400, "Bad Request", s->keep_alive);
        return 400;
    }

    pthread_mutex_lock(&s->js_lock);
    JS_UpdateStackTop(JS_GetRuntime(s->ctx));

    req = qh_build_request(s->ctx, conn, &body);
    qh_buf_free(&body);
    robj = JS_IsException(req) ? JS_EXCEPTION : qh_new_response_object(s->ctx, res);
    if (!JS_IsException(robj) && !JS_IsUndefined(robj)) {
        res->conn = conn;
        res->keep_alive = s->keep_alive;
    }

    if (!JS_IsException(req) && !JS_IsException(robj)) {
        JSValue args[2];
        args[0] = req;
        args[1] = robj;
        ret = JS_Call(s->ctx, s->handler, JS_UNDEFINED, 2, args);
        if (JS_IsException(ret)) {
            qh_fail_request(s, res);
        } else if (!res->ended) {
            if (!JS_IsUndefined(ret) && res->body.len == 0) {
                if (qh_buf_add_js(s->ctx, &res->body, ret) < 0)
                    qh_fail_request(s, res);
            }
            res->ended = true;
        }
    } else {
        qh_fail_request(s, res);
    }

    if (res->status < 1 || res->status > 999)
        res->status = 500;
    qh_flush_response(res, conn, s->keep_alive, s->debug);
    rc = res->status;

    /* Detach the C struct before the JS object's finalizer can see it. */
    if (!JS_IsException(robj) && !JS_IsUndefined(robj))
        JS_SetOpaque(robj, NULL);
    qh_res_free(res);
    JS_FreeValue(s->ctx, req);
    JS_FreeValue(s->ctx, robj);
    JS_FreeValue(s->ctx, ret);
    pthread_mutex_unlock(&s->js_lock);
    return rc;
}

/* ------------------------------------------------------------------ */
/* WebSocket bridge                                                    */
/* ------------------------------------------------------------------ */

typedef struct qh_ws {
    struct mg_connection *conn;
    qh_server *s;
    int closed;
} qh_ws;

typedef struct qh_ws_class_slot {
    JSRuntime *rt;
    JSClassID id;
} qh_ws_class_slot;

static void qh_attach_ws_proto(JSContext *ctx, JSClassID id);
static qh_ws_class_slot qh_ws_slots[8];
static int qh_ws_nslots;

static void qh_ws_finalizer(JSRuntime *rt, JSValue val)
{
    qh_ws *w = (qh_ws *)JS_GetOpaque(val, JS_GetClassID(val));
    (void)rt;
    free(w);
}

static JSClassDef qh_ws_class = {
    "QJSHttpWebSocket",
    .finalizer = qh_ws_finalizer,
};

static JSClassID qh_ws_class_id(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JSClassID id = 0;
    int i;

    pthread_mutex_lock(&qh_global_lock);
    for (i = 0; i < qh_ws_nslots; i++) {
        if (qh_ws_slots[i].rt == rt) {
            id = qh_ws_slots[i].id;
            pthread_mutex_unlock(&qh_global_lock);
            return id;
        }
    }
    JS_NewClassID(rt, &id);
    if (JS_NewClass(rt, id, &qh_ws_class) < 0) {
        pthread_mutex_unlock(&qh_global_lock);
        return 0;
    }
    qh_attach_ws_proto(ctx, id);
    if (qh_ws_nslots < (int)(sizeof(qh_ws_slots) / sizeof(qh_ws_slots[0])))
        qh_ws_slots[qh_ws_nslots++] = (qh_ws_class_slot){ rt, id };
    pthread_mutex_unlock(&qh_global_lock);
    return id;
}

static qh_ws *qh_ws_handle(JSContext *ctx, JSValueConst obj)
{
    JSClassID id = qh_ws_class_id(ctx);
    if (!id)
        return NULL;
    return (qh_ws *)JS_GetOpaque2(ctx, obj, id);
}

static JSValue js_ws_send(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv)
{
    qh_ws *w = qh_ws_handle(ctx, this_val);
    qh_buf b = { 0 };
    int binary, rc;

    if (!w)
        return JS_EXCEPTION;
    if (w->closed)
        return JS_FALSE;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "send(data[, binary])");
    binary = JS_IsString(argv[0]) ? 0 : 1;
    if (argc > 1)
        binary = JS_ToBool(ctx, argv[1]) > 0;
    if (qh_buf_add_js(ctx, &b, argv[0]) < 0)
        return JS_EXCEPTION;
    rc = mg_websocket_write(w->conn,
                            binary ? MG_WEBSOCKET_OPCODE_BINARY
                                   : MG_WEBSOCKET_OPCODE_TEXT,
                            b.data ? b.data : "", b.len);
    qh_buf_free(&b);
    if (rc <= 0) {
        w->closed = 1;
        return JS_FALSE;
    }
    return JS_TRUE;
}

static JSValue js_ws_close(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv)
{
    qh_ws *w = qh_ws_handle(ctx, this_val);
    uint8_t frame[125];
    size_t n = 2;
    int32_t code = 1000;

    if (!w)
        return JS_EXCEPTION;
    if (w->closed)
        return JS_UNDEFINED;
    if (argc > 0 && !JS_IsUndefined(argv[0]) &&
        JS_ToInt32(ctx, &code, argv[0]) < 0)
        return JS_EXCEPTION;
    frame[0] = (uint8_t)((code >> 8) & 0xff);
    frame[1] = (uint8_t)(code & 0xff);
    if (argc > 1 && JS_IsString(argv[1])) {
        size_t len = 0;
        const char *reason = JS_ToCStringLen(ctx, &len, argv[1]);
        if (reason) {
            if (len > 123)
                len = 123;
            memcpy(frame + 2, reason, len);
            n += len;
            JS_FreeCString(ctx, reason);
        }
    }
    mg_websocket_write(w->conn, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE,
                       (char *)frame, n);
    w->closed = 1;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry qh_ws_funcs[] = {
    JS_CFUNC_DEF("send", 1, js_ws_send),
    JS_CFUNC_DEF("close", 0, js_ws_close),
};

static void qh_attach_ws_proto(JSContext *ctx, JSClassID id)
{
    JSValue proto = JS_NewObject(ctx);
    if (JS_IsException(proto))
        return;
    JS_SetPropertyFunctionList(ctx, proto, qh_ws_funcs,
                               (int)(sizeof(qh_ws_funcs) / sizeof(qh_ws_funcs[0])));
    JS_SetClassProto(ctx, id, proto);
}

static JSValue qh_new_ws_object(JSContext *ctx, qh_server *s,
                                struct mg_connection *conn)
{
    JSClassID id = qh_ws_class_id(ctx);
    const struct mg_request_info *ri = mg_get_request_info(conn);
    qh_ws *w;
    JSValue obj, headers;
    int i;

    if (!id)
        return JS_EXCEPTION;
    w = (qh_ws *)calloc(1, sizeof(*w));
    if (!w)
        return JS_ThrowInternalError(ctx, "out of memory");
    w->conn = conn;
    w->s = s;
    obj = JS_NewObjectClass(ctx, id);
    if (JS_IsException(obj)) {
        free(w);
        return obj;
    }
    JS_SetOpaque(obj, w);
    headers = JS_NewObject(ctx);
    if (!JS_IsException(headers)) {
        for (i = 0; i < ri->num_headers && i < MG_MAX_HEADERS; i++) {
            char *name = qh_strdup(ri->http_headers[i].name ? ri->http_headers[i].name : "");
            if (!name)
                continue;
            qh_lower(name);
            JS_SetPropertyStr(ctx, headers, name,
                              JS_NewString(ctx, ri->http_headers[i].value ? ri->http_headers[i].value : ""));
            free(name);
        }
        JS_SetPropertyStr(ctx, obj, "headers", headers);
    }
    JS_SetPropertyStr(ctx, obj, "path",
                      JS_NewString(ctx, ri->local_uri ? ri->local_uri : "/"));
    JS_SetPropertyStr(ctx, obj, "url",
                      JS_NewString(ctx, ri->request_uri ? ri->request_uri : "/"));
    JS_SetPropertyStr(ctx, obj, "remoteAddr", JS_NewString(ctx, ri->remote_addr));
    return obj;
}

static qh_ws_conn *qh_ws_find(qh_server *s, const struct mg_connection *conn)
{
    qh_ws_conn *wc;
    for (wc = s->ws_conns; wc; wc = wc->next) {
        if (wc->conn == conn)
            return wc;
    }
    return NULL;
}

/* Call handler[name](ws_obj, ...) and swallow exceptions. */
static void qh_ws_invoke(qh_server *s, qh_ws_route *route, qh_ws_conn *wc,
                         const char *name)
{
    JSContext *ctx = s->ctx;
    JSValue fn = JS_GetPropertyStr(ctx, route->handler, name);

    if (JS_IsFunction(ctx, fn)) {
        JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &wc->obj);
        if (JS_IsException(ret))
            JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, ret);
    }
    JS_FreeValue(ctx, fn);
}

static int qh_ws_connect(const struct mg_connection *conn, void *cbdata)
{
    qh_ws_route *route = (qh_ws_route *)cbdata;
    qh_server *s = route->s;
    qh_ws_conn *wc = (qh_ws_conn *)calloc(1, sizeof(*wc));

    if (!wc)
        return 1;
    pthread_mutex_lock(&s->js_lock);
    JS_UpdateStackTop(JS_GetRuntime(s->ctx));
    wc->conn = (struct mg_connection *)conn;
    wc->obj = qh_new_ws_object(s->ctx, s, wc->conn);
    if (JS_IsException(wc->obj)) {
        JS_FreeValue(s->ctx, JS_GetException(s->ctx));
        pthread_mutex_unlock(&s->js_lock);
        free(wc);
        return 1;
    }
    wc->next = s->ws_conns;
    s->ws_conns = wc;
    pthread_mutex_unlock(&s->js_lock);
    return 0;
}

static void qh_ws_ready(struct mg_connection *conn, void *cbdata)
{
    qh_ws_route *route = (qh_ws_route *)cbdata;
    qh_server *s = route->s;
    qh_ws_conn *wc;

    pthread_mutex_lock(&s->js_lock);
    JS_UpdateStackTop(JS_GetRuntime(s->ctx));
    wc = qh_ws_find(s, conn);
    if (wc)
        qh_ws_invoke(s, route, wc, "open");
    pthread_mutex_unlock(&s->js_lock);
}

static int qh_ws_on_data(struct mg_connection *conn, int bits, char *data,
                         size_t len, void *cbdata)
{
    qh_ws_route *route = (qh_ws_route *)cbdata;
    qh_server *s = route->s;
    qh_ws_conn *wc;
    JSContext *ctx = s->ctx;
    int keep = 1;

    pthread_mutex_lock(&s->js_lock);
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    wc = qh_ws_find(s, conn);
    if (!wc) {
        pthread_mutex_unlock(&s->js_lock);
        return 0;
    }
    {
        JSValue msg = ((bits & 0xF) == MG_WEBSOCKET_OPCODE_BINARY)
                          ? JS_NewUint8ArrayCopy(ctx, (const uint8_t *)(data ? data : ""), len)
                          : JS_NewStringLen(ctx, data ? data : "", len);
        JSValue fn = JS_GetPropertyStr(ctx, route->handler, "message");
        if (JS_IsFunction(ctx, fn)) {
            JSValue args[3];
            JSValue ret;
            args[0] = wc->obj;
            args[1] = msg;
            args[2] = JS_NewBool(ctx, (bits & 0xF) == MG_WEBSOCKET_OPCODE_BINARY);
            ret = JS_Call(ctx, fn, JS_UNDEFINED, 3, args);
            JS_FreeValue(ctx, args[2]);
            if (JS_IsException(ret)) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                keep = 0;
            }
            JS_FreeValue(ctx, ret);
        }
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, msg);
    }
    {
        qh_ws *w = qh_ws_handle(ctx, wc->obj);
        if (w && w->closed)
            keep = 0;
    }
    pthread_mutex_unlock(&s->js_lock);
    return keep;
}

static void qh_ws_on_close(const struct mg_connection *conn, void *cbdata)
{
    qh_ws_route *route = (qh_ws_route *)cbdata;
    qh_server *s = route->s;
    qh_ws_conn **pp;

    pthread_mutex_lock(&s->js_lock);
    JS_UpdateStackTop(JS_GetRuntime(s->ctx));
    pp = &s->ws_conns;
    while (*pp && (*pp)->conn != conn)
        pp = &(*pp)->next;
    if (*pp) {
        qh_ws_conn *wc = *pp;
        *pp = wc->next;
        qh_ws_invoke(s, route, wc, "close");
        JS_FreeValue(s->ctx, wc->obj);
        free(wc);
    }
    pthread_mutex_unlock(&s->js_lock);
}

static void qh_ws_cleanup(qh_server *s)
{
    qh_ws_route *r;
    qh_ws_conn *wc;

    if (!s->ctx)
        return;
    for (r = s->ws_routes; r;) {
        qh_ws_route *next = r->next;
        JS_FreeValue(s->ctx, r->handler);
        free(r->uri);
        free(r);
        r = next;
    }
    s->ws_routes = NULL;
    for (wc = s->ws_conns; wc;) {
        qh_ws_conn *next = wc->next;
        JS_FreeValue(s->ctx, wc->obj);
        free(wc);
        wc = next;
    }
    s->ws_conns = NULL;
}

/* options.websocket = { "/path": { open, message, close } } */
static void qh_register_ws_routes(qh_server *s, JSValueConst wsmap)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0, i;
    JSContext *ctx = s->ctx;

    if (!s->mg || !JS_IsObject(wsmap))
        return;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, wsmap,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    for (i = 0; i < n; i++) {
        const char *uri = JS_AtomToCString(ctx, tab[i].atom);
        JSValue h = JS_GetProperty(ctx, wsmap, tab[i].atom);

        if (uri && JS_IsObject(h)) {
            qh_ws_route *r = (qh_ws_route *)calloc(1, sizeof(*r));
            if (r) {
                r->s = s;
                r->uri = qh_strdup(uri);
                r->handler = h;
                r->next = s->ws_routes;
                s->ws_routes = r;
                mg_set_websocket_handler(s->mg, r->uri, qh_ws_connect, qh_ws_ready,
                                         qh_ws_on_data, qh_ws_on_close, r);
            } else {
                JS_FreeValue(ctx, h);
            }
        } else {
            JS_FreeValue(ctx, h);
        }
        if (uri)
            JS_FreeCString(ctx, uri);
    }
    JS_FreePropertyEnum(ctx, tab, n);
}

static int qh_is_ws_upgrade(struct mg_connection *conn)
{
    const char *up = mg_get_header(conn, "Upgrade");
    return up && qh_strcasestr(up, "websocket") != NULL;
}

static int qh_begin_request(struct mg_connection *conn)
{
    qh_server *s = (qh_server *)mg_get_user_data(mg_get_context(conn));
    if (qh_is_ws_upgrade(conn))
        return 0; /* CivetWeb runs the websocket handler registered for the path */
    if (!s) {
        qh_send_simple(conn, 500, "Internal Server Error", 0);
        return 500;
    }
    return qh_serve_one(s, conn);
}

static int qh_log_silent(const struct mg_connection *conn, const char *message)
{
    (void)conn;
    (void)message;
    return 1; /* suppress civetweb's own logging */
}

/* ------------------------------------------------------------------ */
/* C API                                                               */
/* ------------------------------------------------------------------ */

void qjs_http_options_init(qjs_http_options *opts)
{
    if (!opts)
        return;
    memset(opts, 0, sizeof(*opts));
    opts->port = "8080";
    opts->threads = 8;
    opts->keep_alive = 1;
    opts->tcp_nodelay = 1;
    opts->request_timeout_ms = 30000;
    opts->grab_signals = 1;
    opts->debug = 0;
    opts->max_body_size = 10u * 1024u * 1024u;
}

static int qh_run_server(JSContext *ctx, const qjs_http_options *opts,
                         JSValueConst handler, JSValueConst on_error,
                         JSValueConst wsmap, char *err, size_t errlen)
{
    qh_server *s;
    struct mg_callbacks cb;
    const char *civopts[64];
    char nums[8][48];
    char portbuf[256];
    int nopts = 0, nnums = 0;
    const char *port;

    if (err && errlen)
        err[0] = '\0';
    if (!ctx || !JS_IsFunction(ctx, handler)) {
        if (err && errlen)
            snprintf(err, errlen, "handler must be a function");
        return -1;
    }

    s = (qh_server *)calloc(1, sizeof(*s));
    if (!s) {
        if (err && errlen)
            snprintf(err, errlen, "out of memory");
        return -1;
    }

    port = (opts && opts->port && *opts->port) ? opts->port : "8080";
    if (opts && opts->binding && *opts->binding && !strchr(port, ':') &&
        !strchr(port, ',')) {
        snprintf(portbuf, sizeof(portbuf), "%s:%s", opts->binding, port);
        port = portbuf;
    }

    s->ctx = ctx;
    s->handler = JS_DupValue(ctx, handler);
    s->on_error = JS_IsUndefined(on_error) ? JS_UNDEFINED : JS_DupValue(ctx, on_error);
    s->port = qh_strdup(port);
    s->threads = (opts && opts->threads > 0) ? opts->threads : 8;
    s->backlog = opts ? opts->backlog : 0;
    s->keep_alive = opts ? (opts->keep_alive != 0) : 1;
    s->tcp_nodelay = opts ? (opts->tcp_nodelay != 0) : 1;
    s->request_timeout_ms = (opts && opts->request_timeout_ms > 0)
                                ? opts->request_timeout_ms
                                : 30000;
    s->debug = opts ? (opts->debug != 0) : 0;
    s->grab_signals = opts ? (opts->grab_signals != 0) : 1;
    s->max_body = (opts && opts->max_body_size > 0)
                      ? opts->max_body_size
                      : 10u * 1024u * 1024u;
    s->extra = opts ? opts->extra_civetweb_options : NULL;

    pthread_mutex_init(&s->js_lock, NULL);
    pthread_mutex_init(&s->wait_lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->stopping = 0;

    civopts[nopts++] = "listening_ports";
    civopts[nopts++] = s->port;
    snprintf(nums[nnums], sizeof(nums[0]), "%d", s->threads);
    civopts[nopts++] = "num_threads";
    civopts[nopts++] = nums[nnums++];
    if (s->backlog > 0) {
        snprintf(nums[nnums], sizeof(nums[0]), "%d", s->backlog);
        civopts[nopts++] = "listen_backlog";
        civopts[nopts++] = nums[nnums++];
    }
    civopts[nopts++] = "enable_keep_alive";
    civopts[nopts++] = s->keep_alive ? "yes" : "no";
    civopts[nopts++] = "tcp_nodelay";
    civopts[nopts++] = s->tcp_nodelay ? "1" : "0";
    snprintf(nums[nnums], sizeof(nums[0]), "%d", s->request_timeout_ms);
    civopts[nopts++] = "request_timeout_ms";
    civopts[nopts++] = nums[nnums++];
    if (s->keep_alive) {
        snprintf(nums[nnums], sizeof(nums[0]), "%d", s->request_timeout_ms);
        civopts[nopts++] = "keep_alive_timeout_ms";
        civopts[nopts++] = nums[nnums++];
    }
    if (s->extra) {
        const char *const *p = s->extra;
        while (p[0] && p[1] && nopts < 62) {
            civopts[nopts++] = p[0];
            civopts[nopts++] = p[1];
            p += 2;
        }
    }
    civopts[nopts] = NULL;

    memset(&cb, 0, sizeof(cb));
    cb.begin_request = qh_begin_request;
    cb.log_message = qh_log_silent;
    cb.log_access = qh_log_silent;

    s->mg = mg_start(&cb, s, civopts);
    if (!s->mg) {
        if (err && errlen)
            snprintf(err, errlen, "could not listen on %s", s->port);
        JS_FreeValue(ctx, s->handler);
        JS_FreeValue(ctx, s->on_error);
        free(s->port);
        pthread_mutex_destroy(&s->js_lock);
        pthread_mutex_destroy(&s->wait_lock);
        pthread_cond_destroy(&s->cond);
        free(s);
        return -1;
    }

    qh_register_ws_routes(s, wsmap);

    if (s->grab_signals)
        qh_install_signals();
    g_signal_stop = 0;

    pthread_mutex_lock(&g_servers_lock);
    s->next = g_servers;
    g_servers = s;
    pthread_mutex_unlock(&g_servers_lock);

    pthread_mutex_lock(&s->wait_lock);
    while (!s->stopping && !g_signal_stop) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 200 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&s->cond, &s->wait_lock, &ts);
    }
    pthread_mutex_unlock(&s->wait_lock);

    mg_stop(s->mg);
    s->mg = NULL;
    /* All worker threads are gone; publish this (the serving) thread's stack
       top again before the caller runs any more JS. */
    JS_UpdateStackTop(JS_GetRuntime(ctx));
    qh_ws_cleanup(s);

    pthread_mutex_lock(&g_servers_lock);
    {
        qh_server **pp = &g_servers;
        while (*pp && *pp != s)
            pp = &(*pp)->next;
        if (*pp)
            *pp = s->next;
    }
    pthread_mutex_unlock(&g_servers_lock);

    JS_FreeValue(ctx, s->handler);
    JS_FreeValue(ctx, s->on_error);
    free(s->port);
    pthread_mutex_destroy(&s->js_lock);
    pthread_mutex_destroy(&s->wait_lock);
    pthread_cond_destroy(&s->cond);
    free(s);
    return 0;
}

int qjs_http_run(JSContext *ctx, const qjs_http_options *opts,
                 JSValueConst handler, JSValueConst on_error, char *err,
                 size_t errlen)
{
    return qh_run_server(ctx, opts, handler, on_error, JS_UNDEFINED, err, errlen);
}

void qjs_http_stop_all(void)
{
    qh_server *s;

    pthread_mutex_lock(&g_servers_lock);
    for (s = g_servers; s; s = s->next) {
        pthread_mutex_lock(&s->wait_lock);
        s->stopping = 1;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->wait_lock);
    }
    pthread_mutex_unlock(&g_servers_lock);
}

int qjs_http_is_running(void)
{
    qh_server *s;
    int n = 0;

    pthread_mutex_lock(&g_servers_lock);
    for (s = g_servers; s; s = s->next)
        n++;
    pthread_mutex_unlock(&g_servers_lock);
    return n;
}

/* ------------------------------------------------------------------ */
/* JS options parsing                                                  */
/* ------------------------------------------------------------------ */

typedef struct qh_owned_opts {
    char *port;
    char *binding;
} qh_owned_opts;

static void qh_owned_opts_free(qh_owned_opts *o)
{
    free(o->port);
    free(o->binding);
    o->port = o->binding = NULL;
}

static int qh_take_string(JSContext *ctx, JSValueConst v, char **out)
{
    if (JS_IsNumber(v)) {
        double d;
        char buf[48];
        if (JS_ToFloat64(ctx, &d, v) < 0)
            return -1;
        if (!(d >= 1 && d <= 65535)) {
            JS_ThrowRangeError(ctx, "invalid port %g (expected 1..65535)", d);
            return -1;
        }
        snprintf(buf, sizeof(buf), "%d", (int)d);
        *out = qh_strdup(buf);
    } else if (JS_IsString(v)) {
        *out = qh_js_cstr(ctx, v);
    } else {
        JS_ThrowTypeError(ctx, "expected a port number or string");
        return -1;
    }
    return *out ? 0 : -1;
}

static int qh_get_int_prop(JSContext *ctx, JSValueConst obj, const char *name,
                           int dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int32_t x = dflt;

    if (JS_IsNumber(v) && JS_ToInt32(ctx, &x, v) < 0)
        x = dflt;
    JS_FreeValue(ctx, v);
    return x;
}

static size_t qh_get_size_prop(JSContext *ctx, JSValueConst obj,
                               const char *name, size_t dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    double d;

    if (JS_IsNumber(v) && JS_ToFloat64(ctx, &d, v) == 0 && d > 0)
        dflt = (size_t)d;
    JS_FreeValue(ctx, v);
    return dflt;
}

static int qh_get_bool_prop(JSContext *ctx, JSValueConst obj, const char *name,
                            int dflt)
{
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    int b = dflt;

    if (!JS_IsUndefined(v) && !JS_IsNull(v))
        b = JS_ToBool(ctx, v) ? 1 : 0;
    JS_FreeValue(ctx, v);
    return b;
}

static int qh_opts_from_js(JSContext *ctx, JSValueConst v, qjs_http_options *o,
                           qh_owned_opts *owned, JSValue *on_error)
{
    JSValue pv;

    memset(owned, 0, sizeof(*owned));
    *on_error = JS_UNDEFINED;
    qjs_http_options_init(o);

    if (JS_IsNumber(v) || JS_IsString(v)) {
        if (qh_take_string(ctx, v, &owned->port) < 0)
            return -1;
        o->port = owned->port;
        return 0;
    }
    if (!JS_IsObject(v)) {
        JS_ThrowTypeError(ctx, "expected serve(port, handler) or serve(options, handler)");
        return -1;
    }

    pv = JS_GetPropertyStr(ctx, v, "port");
    if (JS_IsUndefined(pv) || JS_IsNull(pv)) {
        JS_FreeValue(ctx, pv);
        JS_ThrowTypeError(ctx, "options.port is required");
        return -1;
    }
    if (qh_take_string(ctx, pv, &owned->port) < 0) {
        JS_FreeValue(ctx, pv);
        return -1;
    }
    JS_FreeValue(ctx, pv);
    o->port = owned->port;

    {
        JSValue bv = JS_GetPropertyStr(ctx, v, "binding");
        if (JS_IsString(bv)) {
            owned->binding = qh_js_cstr(ctx, bv);
            o->binding = owned->binding;
        }
        JS_FreeValue(ctx, bv);
    }

    o->threads = qh_get_int_prop(ctx, v, "threads", o->threads);
    o->backlog = qh_get_int_prop(ctx, v, "backlog", o->backlog);
    o->keep_alive = qh_get_bool_prop(ctx, v, "keepAlive", o->keep_alive);
    o->tcp_nodelay = qh_get_bool_prop(ctx, v, "tcpNoDelay", o->tcp_nodelay);
    o->request_timeout_ms = qh_get_int_prop(ctx, v, "requestTimeout",
                                            o->request_timeout_ms);
    o->grab_signals = qh_get_bool_prop(ctx, v, "grabSignals", o->grab_signals);
    o->debug = qh_get_bool_prop(ctx, v, "debug", o->debug);
    o->max_body_size = qh_get_size_prop(ctx, v, "maxBodySize", o->max_body_size);

    {
        JSValue ev = JS_GetPropertyStr(ctx, v, "onError");
        if (JS_IsFunction(ctx, ev))
            *on_error = JS_DupValue(ctx, ev);
        JS_FreeValue(ctx, ev);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ES module                                                           */
/* ------------------------------------------------------------------ */

static JSValue js_serve(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv)
{
    qjs_http_options o;
    qh_owned_opts owned;
    JSValue handler, on_error = JS_UNDEFINED;
    char err[256];
    int rc;
    int owned_handler = 0;

    (void)this_val;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "serve(port, handler) requires a port");
    if (argc >= 2 && JS_IsFunction(ctx, argv[1]))
        handler = argv[1];
    else if (JS_IsObject(argv[0])) {
        JSValue hv = JS_GetPropertyStr(ctx, argv[0], "handler");
        handler = hv;
        owned_handler = 1;
        if (!JS_IsFunction(ctx, handler)) {
            JS_FreeValue(ctx, hv);
            return JS_ThrowTypeError(ctx, "options.handler must be a function");
        }
    } else {
        return JS_ThrowTypeError(ctx, "handler must be a function");
    }

    if (qh_opts_from_js(ctx, argv[0], &o, &owned, &on_error) < 0) {
        if (owned_handler)
            JS_FreeValue(ctx, handler);
        qh_owned_opts_free(&owned);
        JS_FreeValue(ctx, on_error);
        return JS_EXCEPTION;
    }

    {
        JSValue wsmap = JS_UNDEFINED;
        if (JS_IsObject(argv[0]))
            wsmap = JS_GetPropertyStr(ctx, argv[0], "websocket");
        rc = qh_run_server(ctx, &o, handler, on_error, wsmap, err, sizeof(err));
        JS_FreeValue(ctx, wsmap);
    }
    qh_owned_opts_free(&owned);
    JS_FreeValue(ctx, on_error);
    if (owned_handler)
        JS_FreeValue(ctx, handler);
    if (rc < 0)
        return JS_ThrowInternalError(ctx, "%s", err[0] ? err : "could not start server");
    return JS_UNDEFINED;
}

static JSValue js_stop(JSContext *ctx, JSValueConst this_val, int argc,
                       JSValueConst *argv)
{
    (void)ctx;
    (void)this_val;
    (void)argc;
    (void)argv;
    qjs_http_stop_all();
    return JS_UNDEFINED;
}

static JSValue js_is_running(JSContext *ctx, JSValueConst this_val, int argc,
                             JSValueConst *argv)
{
    (void)this_val;
    (void)argc;
    (void)argv;
    return JS_NewInt32(ctx, qjs_http_is_running());
}

static const JSCFunctionListEntry qh_module_funcs[] = {
    JS_CFUNC_DEF("serve", 2, js_serve),
    JS_CFUNC_DEF("stop", 0, js_stop),
    JS_CFUNC_DEF("isRunning", 0, js_is_running),
    JS_PROP_STRING_DEF("version", QJS_HTTP_VERSION, 0),
    JS_PROP_STRING_DEF("civetweb", CIVETWEB_VERSION, 0),
};

#define QH_MODULE_FUNC_COUNT \
    ((int)(sizeof(qh_module_funcs) / sizeof(qh_module_funcs[0])))

static int qh_module_init(JSContext *ctx, JSModuleDef *m)
{
    return JS_SetModuleExportList(ctx, m, qh_module_funcs,
                                  QH_MODULE_FUNC_COUNT);
}

JSModuleDef *qjs_http_init_module(JSContext *ctx, const char *module_name)
{
    JSModuleDef *m;

    if (!module_name || !*module_name)
        module_name = "qjsa:httpd";
    m = JS_NewCModule(ctx, module_name, qh_module_init);
    if (!m)
        return NULL;
    if (JS_AddModuleExportList(ctx, m, qh_module_funcs,
                               QH_MODULE_FUNC_COUNT) < 0) {
        return NULL;
    }
    return m;
}

int qjs_http_register(JSContext *ctx)
{
    return qjs_http_init_module(ctx, "qjsa:httpd") ? 0 : -1;
}
