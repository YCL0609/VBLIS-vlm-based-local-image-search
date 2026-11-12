# qjsa:winproc

`qjsa:winproc` 提供 Windows 平台的进程创建和进程终止能力。模块导出 `exec()` 和 `kill()`，并返回一个可通过 `then()` 回调获取退出码的进程对象。

## 说明

- 启动进程时，进程窗口默认隐藏（`CREATE_NO_WINDOW`）。
- 如果未指定 `outputPath`，子进程的输出不会写入当前终端。
- `winproc.exec()` 和 `winproc.kill()` 的核心实现依赖 Windows API，因此仅在 Windows 平台上提供实际行为。

## `winproc.exec(command, outputPath?)`

- `command`: `string | string[]`
- `outputPath`: `string`（可选）
- 返回: `object`

在 Windows 上启动一个新进程并返回进程对象。

### 参数

- 如果 `command` 是字符串，则直接作为命令行传递给 `CreateProcessW`。
- 如果 `command` 是字符串数组，则模块会将数组元素按顺序组合为带引号的命令行参数，类似：
  `"arg0" "arg1" ...`。
- 如果提供 `outputPath`，则进程的标准输出和标准错误会被重定向到指定文件。

### 返回值

返回一个进程对象，包含：

- `pid`: 进程 ID
- `then(callback)`: 注册进程退出回调，回调参数为退出码

### 示例

```js
import * as winproc from 'qjsa:winproc';

const proc = winproc.exec(['cmd', '/c', 'echo hello'], 'out.txt');
print('pid:', proc.pid);
proc.then(code => {
  print('process exited with code', code);
});
```

### 平台限制

- 在非 Windows 平台上，`winproc.exec()` 会抛出内部错误：
  `Process execution not supported on this platform`。

## `winproc.kill(pid)`

- `pid`: `number`
- 返回: `boolean`

尝试终止给定的进程。

### 返回值

- `true` - 成功终止进程
- `false` - 无法打开或终止目标进程

### 示例

```js
import * as winproc from 'qjsa:winproc';

const ok = winproc.kill(12345);
print('killed:', ok);
```

### 平台限制

- 在非 Windows 平台上，`winproc.kill()` 会抛出内部错误：
  `Process killing not supported on this platform`。

## 进程对象方法 `proc.then(callback)`

- `callback`: `function`
- 返回: 进程对象自身

当子进程结束后，回调会以退出码为参数被调度执行。若进程已结束，则回调会立即安排为一个任务执行。

### 示例

```js
const proc = winproc.exec('ping 127.0.0.1 -n 1');
proc.then(code => {
  print('exit code', code);
});
```