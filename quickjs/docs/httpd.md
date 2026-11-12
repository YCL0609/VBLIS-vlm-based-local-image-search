# qjsa:httpd

`qjsa:httpd` 是一个可嵌入的 HTTP/1.1 服务器，内核是
[CivetWeb 1.16](https://github.com/civetweb/civetweb)（MIT，已内置在
`src/httpd/vendor/civetweb/`）。CivetWeb 负责 TCP、HTTP 解析、keep-alive、
chunked、超时和线程池；请求最终被转换成 JavaScript 回调，响应由 JS 侧构造。

```js
import * as httpd from 'qjsa:httpd';

httpd.serve(8080, (req, res) => {
  res.setHeader('X-Powered-By', 'qjsa:httpd');
  res.json({ path: req.path, query: req.query, method: req.method });
});
```

## 说明

- **线程池 + 单线程 JS**：CivetWeb 用多个工作线程并发收请求，但每个处理器在进入
  JS 前都要先拿一把互斥锁，所以同一时刻只有一个 JS 回调在跑；处理器里可以安全地
  共享全局状态。不要在处理器里做长时间阻塞操作（会卡住所有连接）。
- **阻塞式 `serve()`**：`serve()` 自己就是事件循环，启动后一直处理请求，直到 `httpd.stop()` 被调用
  或收到 SIGINT/SIGTERM。返回值是 `undefined`。
- **静默库**：不会向 stdout/stderr 打印任何东西；启动失败、端口占用等都以异常形式
  从 `serve()` 抛出。
- **一次写出**：整个响应（状态行 + 头 + 正文）先拼进一个缓冲区，再一次性交给
  CivetWeb。在 keep-alive 连接上分多次写会触发 Nagle + 延迟 ACK，吞吐会掉到个位数
  req/s。需要流式输出时用 `res.flush()`（切换为 chunked）。
- **静态文件**：由 `res.sendFile()` 提供，带 ETag / Last-Modified / 304 / Range 206 /
  416 / HEAD，并拒绝 `..` 路径穿越；没有隐式的文档根目录，需要在处理器里显式调用。
- **WebSocket**：`serve()` 的 `websocket` 选项支持升级与双向消息。

## `serve(options, handler)`

- `options`: `number | object` — 端口号，或配置对象
- `handler`: `(req, res) => void` — 请求处理器
- 返回: `undefined`
- 抛出: `TypeError` / `RangeError` / `InternalError`

```js
httpd.serve(8080, handler);
httpd.serve({ port: 8080, binding: '127.0.0.1', threads: 8 }, handler);
httpd.serve({ port: 8080, handler, websocket: { '/ws': { ... } } });
```

### 参数

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `port` | `number \| string` | `8080` | `1–65535`；也接受 `"8080"` 或 `"127.0.0.1:8080"` |
| `binding` | `string` | 所有接口 | 仅当 `port` 是纯数字时拼成 `binding:port` |
| `threads` | `number` | `8` | CivetWeb 工作线程数 |
| `backlog` | `number` | CivetWeb 默认 | listen backlog |
| `keepAlive` | `boolean` | `true` | 开启 HTTP keep-alive（CivetWeb 默认关闭） |
| `tcpNoDelay` | `boolean` | `true` | 关闭 Nagle，避免延迟 ACK 卡顿 |
| `requestTimeout` | `number` | `30000` | 读取请求的超时（毫秒） |
| `maxBodySize` | `number` | `10 MiB` | 超过返回 413；chunked/无长度时读到上限 |
| `grabSignals` | `boolean` | `true` | 捕获 SIGINT/SIGTERM，收到后优雅停止 |
| `debug` | `boolean` | `false` | 500 响应体里附上错误文本 |
| `onError` | `function` | 无 | `(err) => {}`，处理器抛异常时先调用它 |
| `extra` | `string[]` | 无 | 原样追加给 CivetWeb 的配置项（`[key, value, ...]`） |
| `websocket` | `object` | 无 | `{ "/path": { open, message, close } }` |
| `handler` | `function` | 无 | 也可以放进 `options` 里 |

## 请求对象 `req`

| 成员 | 类型 | 说明 |
| --- | --- | --- |
| `method` | `string` | `GET` / `POST` / … |
| `url` | `string` | 原始请求目标，如 `/a?b=1` |
| `path` | `string` | 路径（CivetWeb 已去掉查询串） |
| `query` | `string` | 原始查询串（不含 `?`） |
| `queryParams` | `object` | 解析后的查询参数，重复键为数组 |
| `cookies` | `object` | 解析后的 Cookie，同名首个生效 |
| `headers` | `object` | 全部头，键小写 |
| `header(name)` | `function` | 取单个头（大小写不敏感） |
| `httpVersion` | `string` | `HTTP/1.1` |
| `remoteAddr` | `string` | 客户端地址 |
| `contentType` | `string` | `Content-Type` 头，无则 `""` |
| `contentLength` | `number` | `Content-Length`，未知（chunked）为 `-1` |
| `body` | `string` | 请求体（UTF-8 解码） |
| `bodyBytes` | `Uint8Array` | 原始字节（二进制上传用这个） |
| `json()` | `function` | `JSON.parse(req.body)`，坏 JSON 抛异常 |
| `form()` | `function` | 解析 urlencoded 或 `multipart/form-data` |
| `cookie(name)` | `function` | 取单个 Cookie |
| `is(type)` | `function` | 判断 Content-Type：`json` / `form` / `urlencoded` / `multipart` / 任意前缀 |

`form()` 的返回值：普通字段为字符串（重复名变数组），文件字段为
`{ filename, contentType, size, data }`（`data` 是 `Uint8Array`）。

## 响应对象 `res`

| 成员 | 说明 |
| --- | --- |
| `status` | 状态码属性，默认 `200` |
| `setHeader(n, v)` / `getHeader(n)` / `removeHeader(n)` | 大小写不敏感，可重复设置 |
| `write(chunk)` | 追加响应体（string / ArrayBuffer / TypedArray） |
| `end(chunk?)` | 结束响应；`chunk` 可选追加 |
| `send(body?)` | 按类型发送：string / 字节 / 对象（对象→JSON） |
| `type(t)` | 设置 Content-Type（文本类型自动补 `; charset=utf-8`） |
| `json(value)` | 序列化并发送 `application/json; charset=utf-8` |
| `redirect(url, status = 302)` | 设置 Location 并结束 |
| `flush()` | 立即发出头和已缓冲内容，切换为 chunked 流式 |
| `sendFile(path, opts?)` | 发送文件（ETag/304/Range 206/416/HEAD） |
| `setCookie(name, value, opts?)` | 追加 `Set-Cookie` |
| `hasHeader(n)` / `headersSent` | 头是否设置 / 是否已发出 |

处理器返回后若响应尚未结束，库会自动 `end()` 一个空响应。

### 静态文件

```js
res.sendFile('logo.png', { root: 'public' });            // public/logo.png
res.sendFile(req.path.slice('/static/'.length), { root: 'public' });
```

`opts`：`root`（目录前缀）、`download`（加 `Content-Disposition: attachment`）。
路径里含 `..` 会被拒绝；文件不存在等错误以异常抛出，便于处理器自己转成 404。

### 流式响应

```js
res.type('text/plain');
res.write('one\n');
res.flush();          // 头 + 已缓冲内容立即发出，之后是 chunked
res.write('two\n');
res.end('three\n');
```

### WebSocket

```js
httpd.serve({
  port: 8080,
  handler(req, res) { res.end('http here\n'); },
  websocket: {
    '/ws': {
      open(ws) { ws.send('welcome'); },
      message(ws, data) {                 // string 或 Uint8Array
        if (typeof data === 'string') ws.send('echo:' + data);
        else ws.send(data, true);         // 第二个参数 true = 二进制帧
      },
      close(ws) {},
    },
  },
});
```

`ws` 对象：`send(data[, binary])`、`close(code = 1000, reason?)`、`path`、`url`、
`remoteAddr`、`headers`。回调里抛异常会关闭该连接；`ws.close()` 在 `message`
返回后关闭连接。升级请求（`Upgrade: websocket`）由 CivetWeb 直接接管，不会进入
普通 `handler`。

## 模块级函数

| 导出 | 说明 |
| --- | --- |
| `serve(options?, handler)` | 启动并阻塞运行服务器 |
| `stop()` | 停止本进程中所有服务器（可在处理器内调用，不会死锁） |
| `isRunning()` | 是否有服务器在运行（`0` / `1`） |
| `version` | 模块版本，当前 `"0.2.0"` |
| `civetweb` | 内置 CivetWeb 版本，`"1.16"` |

## 示例

```js
import * as httpd from 'qjsa:httpd';

print('listening on http://127.0.0.1:8080');

httpd.serve({ port: 8080, binding: '127.0.0.1' }, (req, res) => {
  if (req.path === '/health') {
    res.type('text/plain');
    return res.end('ok\n');
  }
  if (req.path === '/api/echo' && req.method === 'POST') {
    return res.json({ method: req.method, body: req.json() });
  }
  if (req.path.startsWith('/static/')) {
    try { res.sendFile(req.path.slice('/static/'.length), { root: 'public' }); }
    catch (e) { res.status = 404; res.end('not found\n'); }
    return;
  }
  res.status = 404;
  res.end('not found\n');
});

print(`listening on http://127.0.0.1:${8080}`);
```

`serve()` 返回后（收到 SIGINT 或 `httpd.stop()`）套接字已经关闭。

### musl / 静态构建
musl 静态可执行文件不支持 `dlopen`，所以静态 `app-linux`
  不能 `import` `build/modules/http.so`。要在 musl 下使用 `.so`，构建时同时传
  `-DQJSA_LIBC=musl -DBUILD_SHARED_MODULES=ON`，得到 musl 动态宿主
  `build/qjs-shared`；纯静态部署请用嵌入式映射（`-M qjsa:httpd,http`）。
