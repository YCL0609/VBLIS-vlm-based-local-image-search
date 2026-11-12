import * as httpd from 'qjsa:httpd';
import { getSource } from 'qjsa:resources';
import * as os from 'qjs:os';
import * as std from 'qjs:std';

const resources = {
    html: getSource('html'),
    css: getSource('css'),
    js: getSource('js'),
    svg: getSource('svg'),
    error404: "<html><head><title>404 Not Found</title></head><body><center><h1>404 Not Found</h1></center><hr></body></html>"
}

const PORT = Number(std.getenv('VBLIS_SHELL_PORT')) || 11120;
const API_HOST = '127.0.0.1';
const API_PORT = std.getenv('VBLIS_API_PORT') || '8000';

/** 拼接本地路径 */
const joinPath = (...parts) => parts
    .filter(part => part !== undefined && part !== null && part !== '')
    .join('/')
    .replace(/\/+/g, '/');

const dirName = (path) => path.replace(/\/[^/]*$/, '');

/**
 * stat 一个路径；quickjs-ng 返回 `[stat, errno]`，老版本直接返回对象或 null，这里两种都吃。
 * @returns {object|null} 存在时返回 stat 对象
 */
function statOf(path) {
    const res = os.stat(path);
    if (!res) return null;
    if (!Array.isArray(res)) return res;
    const [st, err] = res;
    return err ? null : st;
}

/**
 * 找到要拉起的 Python 后端。
 * 可用 VBLIS_BACKEND 直接指定可执行文件，VBLIS_REPO_DIR 改源码树根，VBLIS_PYTHON 指定解释器。
 * @returns {{exe: string, args: string[], cwd: string, label: string}|null}
 */
function findBackend() {
    const exePath = os.exePath();
    const exe = typeof exePath === 'string' ? exePath : '';
    const appDir = dirName(exe);
    const override = std.getenv('VBLIS_BACKEND');
    if (override) return { exe: override, args: [], cwd: appDir, label: 'VBLIS_BACKEND' };

    // 源码分发：api/ 里是 .py，由同目录 run.sh 负责准备 venv 并启动后端
    const runSh = joinPath(appDir, 'run.sh');
    const codeMain = joinPath(appDir, 'api', 'main.py');
    if (statOf(runSh) && statOf(codeMain)) {
        return { exe: runSh, args: ['--backend'], cwd: appDir, label: 'code package' };
    }

    // PyInstaller 版 api 可执行文件就在外壳旁边
    const packaged = joinPath(appDir, 'api', 'api');
    if (statOf(packaged)) return { exe: packaged, args: [], cwd: dirName(packaged), label: 'packaged api' };

    // 源码树 <repo>/quickjs/build-musl/app-linux → <repo>/api
    const repo = std.getenv('VBLIS_REPO_DIR') || joinPath(appDir, '..', '..');
    const apiDir = joinPath(repo, 'api');
    const frozen = joinPath(apiDir, 'dist', 'api', 'api');
    if (statOf(frozen)) return { exe: frozen, args: [], cwd: dirName(frozen), label: 'dist api' };

    const python = std.getenv('VBLIS_PYTHON') || joinPath(apiDir, '.venv', 'bin', 'python');
    const script = joinPath(apiDir, 'main.py');
    if (statOf(python) && statOf(script)) {
        return { exe: python, args: [script], cwd: apiDir, label: 'venv python' };
    }
    if (statOf(script)) return { exe: 'python3', args: [script], cwd: apiDir, label: 'system python3' };
    if (std.getenv('VBLIS_DEBUG') === '1') {
        print('[quickjs][debug] 未找到后端：exe=' + exe + ' apiDir=' + apiDir);
    }
    return null;
}

// 本进程退出时内核会关掉它，后端的 parent_watch.py 读到 EOF 后自行退出，不会留下孤儿进程。
const keptFds = [];

/** 逐级 mkdir -p 已存在不算错 */
function mkdirp(path) {
    const parts = path.split('/');
    let current = path.startsWith('/') ? '' : '.';
    for (const part of parts) {
        if (!part || part === '.') continue;
        current += '/' + part;
        const ret = os.mkdir(current, 0o755);
        if (ret !== 0 && ret !== -17 /* EEXIST */) return ret;
    }
    return 0;
}

/**
 * 打包版默认把数据放在 `$XDG_DATA_HOME/vblis`（默认 ~/.local/share/vblis）：
 * PyInstaller 目录里的 `_internal` 会随升级整个替换，不能在那里存模型和索引。
 * 源码树仍用 api/ 下的现有数据。
 */
function packagedDataDir() {
    const base = std.getenv('XDG_DATA_HOME') || joinPath(std.getenv('HOME') || '', '.local', 'share');
    return joinPath(base, 'vblis');
}

/** 拉起后端；失败只打印，不阻止前端外壳启动（用户可能已经手动跑着一个后端） */
function startBackend() {
    if (std.getenv('VBLIS_NO_BACKEND') === '1') {
        print('[quickjs] VBLIS_NO_BACKEND=1，不自动拉起后端');
        return;
    }
    if (os.platform === 'win32') {
        // qjs:os 的 exec/pipe 在 Windows 上不可用；Windows 继续用 winproc，或由用户手动启动
        print('[quickjs] Windows 下不自动拉起后端，请先运行 api\\api.exe');
        return;
    }

    const backend = findBackend();
    if (!backend) {
        print('[quickjs] 未找到后端，请先运行 python api/main.py，或设置 VBLIS_BACKEND');
        return;
    }

    const logPath = std.getenv('VBLIS_BACKEND_LOG') || '/tmp/vblis-backend.log';
    let logFd = -1;
    try {
        logFd = os.open(logPath, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644);
    } catch (err) {
        print('[quickjs] 无法写后端日志 ' + logPath + ': ' + err);
    }
    if (logFd < 0) logFd = -1;

    const fds = os.pipe();
    if (!fds) {
        print('[quickjs] os.pipe() 失败，跳过自动拉起后端');
        return;
    }
    const [readFd, writeFd] = fds;
    keptFds.push(writeFd);

    const env = {
        ...std.getenviron(),
        VBLIS_EXIT_WITH_PARENT: '1',
        VBLIS_API_PORT: API_PORT,
    };

    // 分发包不能把模型/索引写进安装目录（升级/重装时会被整个替换），默认放到用户数据目录。
    if ((backend.label === 'packaged api' || backend.label === 'code package') && !env.VBLIS_DATA_DIR) {
        const dataDir = packagedDataDir();
        const mk = mkdirp(dataDir);
        if (mk === 0) {
            env.VBLIS_DATA_DIR = dataDir;
            print('[quickjs] 数据目录: ' + dataDir);
        } else {
            print('[quickjs] 无法创建数据目录 ' + dataDir + '（errno ' + mk + '），后端将使用安装目录');
        }
    }

    let pid;
    try {
        pid = os.exec([backend.exe, ...backend.args], {
            block: false,
            cwd: backend.cwd,
            stdin: readFd,
            stdout: logFd >= 0 ? logFd : 1,
            stderr: logFd >= 0 ? logFd : 2,
            env,
        });
    } catch (err) {
        print('[quickjs] 后端启动失败（' + backend.exe + '）: ' + err);
        os.close(readFd);
        if (logFd >= 0) os.close(logFd);
        return;
    }

    os.close(readFd);
    if (logFd >= 0) os.close(logFd);

    if (pid <= 0) {
        print('[quickjs] 后端启动失败（' + backend.exe + '），详见 ' + logPath);
        return;
    }
    print('[quickjs] 后端已启动 (pid ' + pid + ', ' + backend.label + ')，日志: ' + logPath);
    print('[quickjs] API: http://' + API_HOST + ':' + API_PORT);

    // 启动失败时（fd 无效、cwd 不存在、找不到解释器等）子进程会立刻退出；这里补一条可诊断的日志。
    for (let i = 0; i < 3; i++) {
        os.sleep(100);
        const [ret, status] = os.waitpid(pid, os.WNOHANG);
        if (ret === pid) {
            const code = (status >> 8) & 0xff;
            print('[quickjs] 后端进程已退出：status ' + status + '（exit code ' + code + '），详见 ' + logPath);
            return;
        }
    }
}

/**
 * 延迟 2 秒后用系统默认浏览器打开页面：等 httpd.serve() 真正开始监听再打开，
 * 避免浏览器先于服务器启动、只看到连接失败。VBLIS_NO_BROWSER=1 关闭，
 * VBLIS_SHELL_URL 覆盖地址（默认 http://127.0.0.1:11120）。
 */
function openBrowser() {
    if (std.getenv('VBLIS_NO_BROWSER') === '1') return;
    if (os.platform === 'win32') return;   // Windows 外壳尚未支持自动拉起/打开

    const opener = os.platform === 'darwin' ? 'open' : 'xdg-open';
    const url = std.getenv('VBLIS_SHELL_URL') || ('http://' + API_HOST + ':' + PORT);
    const quoted = "'" + url.replace(/'/g, "'\\''") + "'";
    // 用一个短命 sh 负责 sleep 与打开，外壳本体不阻塞在 serve() 之外的等待上
    const script = 'sleep 2; command -v ' + opener + ' >/dev/null 2>&1 && exec ' + opener + ' ' + quoted;

    const fdNull = os.open('/dev/null', os.O_RDWR);
    try {
        os.exec(['/bin/sh', '-c', script], {
            block: false,
            stdin: fdNull >= 0 ? fdNull : 0,
            stdout: fdNull >= 0 ? fdNull : 1,
            stderr: fdNull >= 0 ? fdNull : 2,
        });
        print('[quickjs] 2 秒后用默认浏览器打开 ' + url + '（VBLIS_NO_BROWSER=1 可关闭）');
    } catch (err) {
        print('[quickjs] 无法自动打开浏览器: ' + err);
    } finally {
        if (fdNull >= 0) os.close(fdNull);
    }
}

function handleRequest(req, res) {
    if (req.path === '/' || req.path === '/index.html') {
        res.type('text/html');
        return res.end(resources.html);
    }

    if (req.path === '/index.css') {
        res.type('text/css');
        return res.end(resources.css);
    }

    if (req.path === '/index.js') {
        res.type('text/javascript');
        return res.end(resources.js);
    }

    if (req.path === '/vite.svg') {
        res.type('image/svg+xml');
        return res.end(resources.svg);
    }

    res.status = 404;
    res.type('text/html');
    res.end(resources.error404);
}

/**
 * 单实例守卫：用一个 pid 文件记录「谁在占用这个外壳端口」。
 *
 * 之前这里想做 serve 失败重试，但 quickjs-ng 的 httpd 模块在 bind 失败时抛出的
 * InternalError 无法被 JS 的 try/catch 捕获（实测会直接冒到顶层并打印堆栈），
 * 所以改成在 serve 之前先判断有没有活着的实例：有就打开它的页面然后退出，
 * 避免用户双击两次看到一屏 InternalError。
 */
const RUNTIME_DIR = std.getenv('XDG_RUNTIME_DIR') || '/tmp';
const PID_FILE = joinPath(RUNTIME_DIR, 'vblis-shell-' + PORT + '.pid');
const SHELL_URL = 'http://' + API_HOST + ':' + PORT;

function readPidFile() {
    try {
        const text = std.loadFile(PID_FILE);
        if (!text) return 0;
        const pid = parseInt(String(text).trim(), 10);
        return Number.isFinite(pid) && pid > 0 ? pid : 0;
    } catch (err) {
        return 0;
    }
}

function pidAlive(pid) {
    if (!pid || pid === os.getpid()) return false;
    try {
        os.kill(pid, 0);
        return true;
    } catch (err) {
        return false;
    }
}

function dropPidFile() {
    try {
        os.remove(PID_FILE);
    } catch (err) {
        // 文件不存在或没有权限都无所谓，退出路径不报错
    }
}

function guardSingleInstance() {
    if (std.getenv('VBLIS_ALLOW_MULTI') === '1') return;

    const other = readPidFile();
    if (pidAlive(other)) {
        print('[quickjs] 检测到已有实例在运行（pid ' + other + '），直接打开 ' + SHELL_URL);
        std.out.flush();
        std.getenv('VBLIS_NO_BROWSER') === '1' || openBrowser();
        std.exit(0);
    }

    try {
        const f = std.open(PID_FILE, 'w');
        if (f) {
            f.puts(String(os.getpid()));
            f.close();
        }
    } catch (err) {
        print('[quickjs] 写 pid 文件失败（忽略）: ' + err);
    }
}

guardSingleInstance();
startBackend();
openBrowser();
httpd.serve({ port: PORT, binding: '127.0.0.1' }, handleRequest);
dropPidFile();
