#!/usr/bin/env python3
# ebbctl —— ExBarBrowser 结构化通道的统一客户端。
#
# 为什么存在：在此之前，每次要驱动程序都得手写一份 ctypes 管道客户端
# （读 ai-pipe.txt / ai-token.txt、CreateFileW、拼 JSON-RPC、处理订阅推送与
# 响应混流）。那不是"结构化操作"，是每次重新发明传输层。本模块把传输层收敛到
# 一处，调用方只说"调哪个命令、什么参数"。
#
# 用法（CLI）：
#   python tools/ebbctl.py list                      列出全部工具
#   python tools/ebbctl.py state                     读状态快照
#   python tools/ebbctl.py call bmk.list '{}'        调命令
#   python tools/ebbctl.py call bmk.move '{"id":"b-14"}' --approve
#   python tools/ebbctl.py events --after 5          补历史事件（一次性）
#   python tools/ebbctl.py subscribe --after 5 --count 3   订阅并收 N 条推送
#   python tools/ebbctl.py --profile <dir> ...  指定 profile 根（旧 --data-dir 仍可用）
#
# 作为模块：
#   from ebbctl import Client
#   c = Client()                     # 自动从 exe 根的 runtime\ 发现 pipe/token
#   r = c.call('bmk.list', {})       # 返回已解析的 dict（含 ok / structuredContent）
#   c.call('fs.delete', {...}, approve=True)
#
# 发现文件已从 data\ 迁到 runtime\。传入的目录既可以是新语义的 profile 根，
# 也可以是旧布局里直接放 ai-pipe.txt 的 data 目录 —— 两种都认，避免升级期
# 连不上正在运行的旧实例。
#
# 传输纪律（R21）：只走命名管道，不发任何输入消息、不动光标、不抢焦点。
import argparse
import ctypes
import ctypes.wintypes as wt
import io
import json
import os
import sys

# 默认 profile 根 = 本仓库根（脚本在 tools\ 下）。发现文件在其 runtime\ 子目录。
_HERE = os.path.dirname(os.path.abspath(__file__))
_DEFAULT_ROOT = os.path.dirname(_HERE)


def runtime_dir_of(profile=None):
    """把 profile 根（或旧数据目录）解析成真正放发现文件的 runtime 目录。

    优先级：显式 EXBAR_RUNTIME_DIR > 传入目录本身就是 runtime > 传入目录直接
    放着 ai-pipe.txt（旧布局）> <profile>/runtime（新布局）。最后一项即使目录
    还不存在也返回，让错误信息指出正确的位置，而不是误导用户去 data\\ 找。
    """
    explicit = os.environ.get("EXBAR_RUNTIME_DIR")
    if explicit:
        return os.path.abspath(explicit)
    p = os.path.abspath(profile or _DEFAULT_ROOT)
    if os.path.basename(p).lower() == "runtime" or os.path.isfile(os.path.join(p, "ai-pipe.txt")):
        return p
    legacy = os.path.join(p, "data")
    if os.path.isfile(os.path.join(legacy, "ai-pipe.txt")):
        return legacy
    return os.path.join(p, "runtime")

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileW.restype = ctypes.c_void_p
_k32.CreateFileW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p,
                             wt.DWORD, wt.DWORD, ctypes.c_void_p]
_k32.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                          ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
_k32.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                           ctypes.POINTER(wt.DWORD), ctypes.c_void_p]
_k32.PeekNamedPipe.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wt.DWORD,
                               ctypes.POINTER(wt.DWORD), ctypes.POINTER(wt.DWORD),
                               ctypes.POINTER(wt.DWORD)]
_INVALID = ctypes.c_void_p(-1).value

_GENERIC_RW = 0xC0000000
_OPEN_EXISTING = 3
_BUF = 1 << 20  # 1 MiB：命令结果含 schema/事件，64 KiB 不够


class ChannelError(RuntimeError):
    """通道不可用（程序没开 / 没启用 AI / 管道文件是上次的残留）。"""


class Client:
    """一个到 ExBarBrowser AI 通道的客户端。每条 call 独立建连接（与服务端单连接模型匹配）。"""

    def __init__(self, profile=None):
        self.profile_dir = os.path.abspath(profile or _DEFAULT_ROOT)
        self.runtime_dir = runtime_dir_of(self.profile_dir)
        # data_dir 保留为兼容别名：调用方大量把它继续传给 Client()，而 Client
        # 现在会自己解析 runtime。真正的文件路径只看 runtime_dir。
        self.data_dir = self.profile_dir
        self._pipe_file = os.path.join(self.runtime_dir, "ai-pipe.txt")
        self._token_file = os.path.join(self.runtime_dir, "ai-token.txt")

    def _discover(self):
        if not (os.path.isfile(self._pipe_file) and os.path.isfile(self._token_file)):
            raise ChannelError(
                "通道发现文件不存在：%s / %s\n"
                "程序是否以 --ai 启动，或 settings.json 里 aiEnabled=true？"
                % (self._pipe_file, self._token_file))
        pipe = io.open(self._pipe_file, encoding="utf-8").read().strip()
        token = io.open(self._token_file, encoding="utf-8-sig").read().strip()
        return pipe, token

    def _connect(self, pipe, retries=20, interval=0.25):
        """连接管道，带短暂重试。

        为什么需要重试：服务端是单连接模型，ServeLoop 每次处理完一笔会关掉旧管道、
        重建新管道；正好在重建窗口期连接会拿到 err=2/231。程序刚启动时也要等
        StartServer 完成。客户端不该把这些正常的时序当成"通道不可用"——那会让
        本来成功的自动化随机失败（最难查的那类 flaky）。
        """
        import time
        last_err = 0
        for _ in range(retries):
            h = _k32.CreateFileW(pipe, _GENERIC_RW, 0, None, _OPEN_EXISTING, 0, None)
            if h not in (None, 0, _INVALID):
                return h
            last_err = ctypes.get_last_error()
            time.sleep(interval)
        raise ChannelError("连不上管道 %s（err=%d）；程序可能已退出或通道关闭"
                           % (pipe, last_err))

    @staticmethod
    def _write_line(h, obj):
        raw = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
        n = wt.DWORD(0)
        if not _k32.WriteFile(ctypes.c_void_p(h), raw, len(raw), ctypes.byref(n), None):
            raise ChannelError("写管道失败 err=%d" % ctypes.get_last_error())

    @staticmethod
    def _read_line(h):
        # 服务端按行回应；这里逐字节读到换行（简单可靠，避免一次 Read 拼到多行）。
        acc = bytearray()
        one = ctypes.create_string_buffer(1)
        while True:
            m = wt.DWORD(0)
            if not _k32.ReadFile(ctypes.c_void_p(h), one, 1, ctypes.byref(m), None) or m.value == 0:
                break
            acc += one.raw[0:1]
            if acc[-1:] == b"\n":
                break
            if len(acc) > _BUF:
                raise ChannelError("响应行超过 %d 字节" % _BUF)
        return acc.decode("utf-8", "replace").strip()

    @staticmethod
    def _read_line_timed(h, timeout_s):
        """订阅用：等到有数据或超时，返回一行。空闲时不会永久阻塞。"""
        import time
        acc = bytearray()
        one = ctypes.create_string_buffer(1)
        deadline = time.time() + timeout_s
        while True:
            avail = wt.DWORD(0)
            if not _k32.PeekNamedPipe(ctypes.c_void_p(h), None, 0, None,
                                      ctypes.byref(avail), None):
                return ""
            if avail.value == 0:
                if time.time() >= deadline:
                    return acc.decode("utf-8", "replace").strip()
                time.sleep(0.02)
                continue
            m = wt.DWORD(0)
            if not _k32.ReadFile(ctypes.c_void_p(h), one, 1, ctypes.byref(m), None) or m.value == 0:
                break
            acc += one.raw[0:1]
            if acc[-1:] == b"\n":
                break
            if len(acc) > _BUF:
                raise ChannelError("响应行超过 %d 字节" % _BUF)
        return acc.decode("utf-8", "replace").strip()

    def request(self, req, read_lines=1):
        """发一个旧格式请求对象，返回解析后的响应（或事件列表）。"""
        pipe, token = self._discover()
        req = dict(req)
        req.setdefault("token", token)
        h = self._connect(pipe)
        try:
            self._write_line(h, req)
            out = []
            for _ in range(read_lines):
                line = self._read_line(h)
                if not line:
                    break
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    out.append({"ok": False, "error": "non_json_response", "raw": line})
            return out[0] if read_lines == 1 else out
        finally:
            _k32.CloseHandle(ctypes.c_void_p(h))

    # ---- 语义化方法 ----
    def call(self, name, args=None, approve=False):
        return self.request({"method": "tools/call", "name": name,
                             "args": args or {}, "approve": bool(approve)})

    def list_tools(self):
        return self.request({"method": "tools/list"})

    def state(self):
        return self.request({"method": "state"})

    def events_since(self, after=0, limit=0):
        return self.request({"method": "events/since", "after": after, "limit": limit})

    def subscribe(self, after=0, count=None, timeout_s=30):
        """保持连接收推送。count=None 时一直收（调用方自行中断）。"""
        pipe, token = self._discover()
        h = self._connect(pipe)
        got = []
        try:
            self._write_line(h, {"method": "events/subscribe", "token": token, "after": after})
            # 第一行是订阅确认（不是事件）
            ack = json.loads(self._read_line(h))
            n = 0
            import time
            deadline = time.time() + timeout_s
            while (count is None or n < count) and time.time() < deadline:
                line = self._read_line_timed(h, 0.2)
                if not line:
                    continue
                evt = json.loads(line)
                if evt.get("event"):
                    got.append(evt)
                    n += 1
            return {"ack": ack, "events": got}
        finally:
            _k32.CloseHandle(ctypes.c_void_p(h))


def _payload(r):
    return r.get("structuredContent") or r.get("result") or r


def main(argv=None):
    ap = argparse.ArgumentParser(description="ExBarBrowser 结构化通道客户端")
    ap.add_argument("--profile", "--data-dir", dest="profile",
                    help="profile 根（旧 --data-dir 兼容；默认 exe 旁）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="列出全部工具")

    sub.add_parser("state", help="读状态快照")

    c = sub.add_parser("call", help="调用命令")
    c.add_argument("name")
    c.add_argument("args", nargs="?", default="{}", help="JSON 参数对象")
    c.add_argument("--approve", action="store_true", help="批准破坏性命令")
    c.add_argument("--verdict", action="store_true",
                   help="只输出一行 verdict 摘要（{verdict, ok, error, assertions}），供脚本判定")

    e = sub.add_parser("events", help="补历史事件")
    e.add_argument("--after", type=int, default=0)
    e.add_argument("--limit", type=int, default=0)

    s = sub.add_parser("subscribe", help="订阅并收推送")
    s.add_argument("--after", type=int, default=0)
    s.add_argument("--count", type=int, default=None)
    s.add_argument("--timeout", type=float, default=30)

    a = ap.parse_args(argv)
    cli = Client(a.profile)
    try:
        if a.cmd == "list":
            r = cli.list_tools()
        elif a.cmd == "state":
            r = cli.state()
        elif a.cmd == "call":
            try:
                args = json.loads(a.args)
            except json.JSONDecodeError as ex:
                ap.error("args 不是合法 JSON: %s" % ex)
            r = cli.call(a.name, args, approve=a.approve)
        elif a.cmd == "events":
            r = cli.events_since(a.after, a.limit)
        elif a.cmd == "subscribe":
            r = cli.subscribe(a.after, a.count, a.timeout)
        else:
            ap.error("未知命令")
    except ChannelError as ex:
        print(json.dumps({"ok": False, "error": str(ex)}, ensure_ascii=False))
        return 2

    # call --verdict：只给判定摘要，脚本不必解析整套 schema。
    # verdict 缺失（老版本程序）时回落到 ok —— 不编造更强的结论。
    if a.cmd == "call" and getattr(a, "verdict", False):
        summary = {
            "command": r.get("command"),
            "ok": r.get("ok"),
            "verdict": r.get("verdict", r.get("ok")),
        }
        if r.get("error"):
            summary["error"] = r["error"]
        if r.get("assertions"):
            summary["assertions"] = r["assertions"]
        print(json.dumps(summary, ensure_ascii=False))
        return 0 if summary["verdict"] else 1

    print(json.dumps(r, ensure_ascii=False, indent=2))
    # 退出码看 verdict（效果判定）；无 verdict 时回落 ok。
    if a.cmd == "call":
        v = r.get("verdict", r.get("ok"))
        return 0 if v else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
