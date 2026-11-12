# qjsa:resources

`qjsa:resources` 提供对编译时嵌入资源的访问，允许在运行时通过资源名获取文本内容。

## 说明

- 嵌入资源由 `per-build.sh` 脚本生成，可自行修改文件顶部的配置选项
- 嵌入资源列表由 `list.conf` 控制，命名规范为: `<资源内部名>=<资源相对路径>`

## `getSource(id)`

- `id`: `string`
- 返回: `string | undefined`

根据资源名称查找已嵌入的资源内容。

### 行为

- 如果指定资源存在，返回该资源的文本内容。
- 如果资源不存在，返回 `undefined`。
- `id` 必须是字符串，否则会抛出类型错误。

### 示例

```js
import { getSource } from 'qjsa:resources';

const text = getSource('list');
if (text !== undefined) {
  print(text);
} else {
  print('resource not found');
}
```

### 备注

- 资源表由构建过程生成并嵌入到 `qjsa:resources` 模块中。
- 当前实现通过 `JS_NewStringLen` 返回字符串，因此该 API 适用于文本资源。
- 如果需要二进制资源，需在调用端做额外处理或扩展模块。