"""holopet_agentd/main.py — HoloPet Agent 服务 (JSON Lines IPC)  V6-R3

正式包 (自 ai_worker/holopet_ai 迁入; 旧入口仅剩薄兼容 launcher)。

R3 变化 (并发边界收口, 对应 R2 缺陷):
  - 每连接 send_lock: 所有 JSONL 发送经过同一串行函数, 一行消息不可交错
    (R2: reader 线程的 cancel 回执与 turn 线程事件可交错 sendall)。
  - _cancel_flags / _transcripts / active turn 状态由 _state_lock 保护。
  - 有界 turn 执行: 单 worker 执行器 (最多 1 个 active turn);
    忙时新 start_turn 在 reader 中立即 busy 拒绝 (不再每请求建线程)。
  - disconnect → 取消该连接的 active turn、清理暂存状态;
    已死连接不再被后台线程反复 sendall。
  - 错误结构化: {"type":"error","code":"<稳定码>","text":...}。
  - 传输: Linux 生产支持 Unix Domain Socket (--uds); Windows 开发回退
    loopback TCP (默认 127.0.0.1, 强制回环)。

保留 R2 行为:
  - 真实 turn 串行 (锁覆盖整个 turn)
  - transcript 按 request_id 保存, 作为 LLM 实际 user content
  - STT/TTS Provider 合同; Fake 确定性离线
  - 工具结果真实回灌 (tool_call_id 对应)
  - cancel 只影响匹配 request_id; 完成后清理
"""

import argparse
import json
import hashlib
import logging
import os
import threading
import time
import queue
import socket

from . import protocol
from .prompt_loader import PromptComposer, PromptError
from .providers import make_provider
from .providers.errors import ProviderError, event_error_code
from .audio.capture import AudioCapture
from .audio.player import AudioPlayer
from .providers.stt import (AudioFormat, FakeSttProvider, OpenAiCompatSttProvider,
                            TranscriptResult)
from .providers.volcengine_streaming import (
    VolcengineStreamingSttProvider, FALLBACKABLE)
from .providers.tts import FakeTtsProvider, OpenAiCompatTtsProvider
from .response import ResponseFormatter
from .response.interpreter import interpret
from .tools import dispatch, is_dangerous_name, TOOL_REGISTRY
from .tools.whitelist import (validate_tool_calls, TOOL_WHITELIST,
                              confirm_required, TOOL_SCHEMAS,
                              READONLY_TOOLS)
from .router import (route_decision, detect_explicit_override,
                       detect_insufficient)
from .router.route_decision import DEFAULT_INSUFFICIENT_MARKERS
from .agent import (AgentRuntime, AgentAudit, AgentStore, AgentBudgets,
                    ConfirmationGate, TurnOutcome)
from .tools import TOOL_NAMES

# R8_R4_R1: 白名单功能工具 schema 广告进 LLM tools 列表。
# openai_compat.tool_schemas() 只返回 TOOL_REGISTRY (V6 内部工具);
# 不扩展则 §5 功能工具在真实链路永不可见 (Pi B7 实测定位)。
# 幂等: 模块重载/重复导入不叠加重复 schema。
_registry_names = {x["function"]["name"] for x in TOOL_REGISTRY}
TOOL_REGISTRY.extend(
    x for x in TOOL_SCHEMAS if x["function"]["name"] not in _registry_names)

LOG = logging.getLogger("holopet_agentd")

# 进程级共享 Agent 状态：待确认槽位跨轮存活（确认回复是新一轮），
# 进程重启后自然失效；审计与幂等库在同一进程内共享。
_SHARED_GATE = ConfirmationGate()
_SHARED_AUDIT = AgentAudit()
_SHARED_STORES: dict = {}          # path -> AgentStore (按路径缓存)
_SHARED_STORE_LOCK = threading.Lock()


def _shared_store(config: dict):
    """惰性打开进程级 SQLite 存储（0600；路径可配置且不进入发行包）。

    显式空串 (config state_db_path=="" 或 HOLOPET_AGENT_STATE=="") = 禁用
    (测试隔离; 生产缺省启用默认路径)。按路径缓存: 同一进程可同时服务
    不同路径/禁用组合, 不被首个调用固化。
    """
    cfg_path = config.get("state_db_path")
    env_path = os.environ.get("HOLOPET_AGENT_STATE")
    if cfg_path == "" or env_path == "":
        return None
    path = (cfg_path
            or env_path
            or os.path.join(
                os.environ.get(
                    "XDG_DATA_HOME",
                    os.path.join(os.path.expanduser("~"),
                                 ".local", "share")),
                "holopet", "agent_state.sqlite3"))
    with _SHARED_STORE_LOCK:
        if path not in _SHARED_STORES:
            store = AgentStore(path)
            try:
                store.open()
                _SHARED_STORES[path] = store
            except Exception as e:   # noqa: BLE001 — 存储故障不阻断服务
                LOG.warning("agent store open failed: %s",
                            type(e).__name__)
                _SHARED_STORES[path] = None
        return _SHARED_STORES[path]


# R8_R3: 音频/Provider 构造钩子 — 生产实现; 测试通过 monkeypatch 注入替身
# (与既有 _runner_factory 注入风格一致, 不访问真实设备/公网)。
def make_audio_capture(cfg: dict) -> "AudioCapture":
    return AudioCapture(cfg)


def make_audio_player(cfg: dict) -> "AudioPlayer":
    return AudioPlayer(cfg)


def make_stt_provider(cfg: dict) -> "OpenAiCompatSttProvider":
    return OpenAiCompatSttProvider(cfg)


def make_tts_provider(cfg: dict) -> "OpenAiCompatTtsProvider":
    return OpenAiCompatTtsProvider(cfg)


class _ToolExecutorAdapter:
    """R5_R4: runtime 工具执行器适配 — 白名单走 UDS tool_request/tool_result,
    内部工具走 dispatch; 校验/确认判定/副作用分类集中在单一目录语义。"""

    def __init__(self, owner: "TurnRunner", rid: str, send, cancel_event):
        self._o = owner
        self._rid = rid
        self._send = send
        self._cancel = cancel_event

    def validate(self, tool_calls: list) -> tuple:
        accepted, rejected = [], []
        for call in tool_calls:
            fn = call.get("function") or {}
            name = fn.get("name", "")
            if is_dangerous_name(name):
                rejected.append((name, "dangerous_name"))
                LOG.warning("dangerous tool rejected: %s", name)
                continue
            if name in TOOL_WHITELIST:
                acc, rej = validate_tool_calls([call])
                if acc:
                    # validate_tool_calls 把解析后的 arguments 放 call 顶层;
                    # runtime 读 function.arguments — 这里归一化
                    c = acc[0]
                    fn2 = dict(c.get("function") or {})
                    fn2["arguments"] = c.get("arguments", {})
                    c2 = dict(c)
                    c2["function"] = fn2
                    accepted.append(c2)
                else:
                    rejected.append(rej[0] if rej else (name, "unknown_tool"))
                    LOG.warning("tool rejected: %s (%s)", name,
                                rej[0][0] if rej else "unknown_tool")
                continue
            if name in TOOL_NAMES:
                raw = fn.get("arguments")
                if isinstance(raw, str):
                    try:
                        args = json.loads(raw or "{}")
                    except (json.JSONDecodeError, ValueError):
                        args = {}
                else:
                    args = raw or {}
                c2 = dict(call)
                f2 = dict(fn)
                f2["arguments"] = args
                c2["function"] = f2
                accepted.append(c2)
                continue
            rejected.append((name, "unknown_tool"))
            LOG.warning("tool rejected: %s (unknown_tool)", name)
        return accepted, rejected

    def needs_confirmation(self, tool_name: str, arguments: dict) -> bool:
        return confirm_required(tool_name, arguments)

    def has_side_effect(self, tool_name: str) -> bool:
        # R5_R4 (B): 目录驱动 — 读取类无副作用; 其余记幂等
        return tool_name not in READONLY_TOOLS

    def execute(self, tool_name: str, arguments: dict, call_id: str) -> dict:
        if self._cancel.is_set():
            return {"ok": False, "code": "cancelled", "result": {}}
        if tool_name in TOOL_WHITELIST:
            req = {"v": "1", "request_id": self._rid,
                   "tool_call_id": call_id, "name": tool_name,
                   "arguments": json.dumps(arguments, ensure_ascii=False)}
            if not self._send(self._rid, "tool_request",
                              text=json.dumps(req, ensure_ascii=False)):
                return {"ok": False, "code": "send_failed", "result": {}}
            r = self._o._await_tool_result(self._rid, call_id, self._cancel)
            if "error" in r:
                self._send(self._rid, "tool_call", value=tool_name)
                return {"ok": False, "code": r["error"], "result": {}}
            ok = bool(r.get("ok"))
            self._send(self._rid, "tool_call", value=tool_name)
            return {"ok": ok, "code": r.get("code", ""),
                    "result": r.get("result", {})}
        # R5_R4 (B): 偏好工具 (python target, 存储于 agentd SQLite)
        if tool_name in ("remember_preference", "list_preferences",
                         "forget_preference"):
            return self._execute_preference(tool_name, arguments)
        # 内部工具 (dispatch)
        try:
            result = dispatch(tool_name, arguments)
            LOG.info("tool %s -> ok", tool_name)
            self._send(self._rid, "tool_call", value=tool_name)
            return {"ok": True, "code": "", "result": result or {}}
        except ValueError as e:
            LOG.warning("tool rejected: %s", e)
            self._send(self._rid, "tool_call", value=tool_name)
            return {"ok": False, "code": "tool_rejected", "result": {}}

    def _execute_preference(self, tool_name: str, arguments: dict) -> dict:
        store = _shared_store(self._o._config)
        if store is None:
            return {"ok": False, "code": "store_unavailable", "result": {}}
        if tool_name == "remember_preference":
            key = str(arguments.get("key", ""))
            value = str(arguments.get("value", ""))
            if len(value) > 500:            # B3: 单项最多 500 码点
                return {"ok": False, "code": "value_too_long", "result": {}}
            code = store.remember_preference(key, value)
            self._send(self._rid, "tool_call", value=tool_name)
            return {"ok": code == "ok", "code": code, "result": {}}
        if tool_name == "list_preferences":
            rows = store.list_preferences()
            self._send(self._rid, "tool_call", value=tool_name)
            return {"ok": True, "code": "",
                    "result": {"preferences": rows}}
        # forget_preference: id='all' 经确认后 forget_all; 单条直删
        key = str(arguments.get("id", ""))
        if key == "all":
            store.forget_all_preferences()
            self._send(self._rid, "tool_call", value=tool_name)
            return {"ok": True, "code": "", "result": {}}
        code = store.forget_preference(key)
        self._send(self._rid, "tool_call", value=tool_name)
        return {"ok": code == "ok", "code": code, "result": {}}


class TurnRunner:
    """单个 active turn 的完整处理 (worker 侧状态机 + 工具回灌)。

    R4 (P3) 最小语音状态流:
      start_turn → state listening → transcript → state thinking
      → content 0..N (显示层完整内容, 分片不截断)
      → response_complete → expression → state speaking
      → TTS (播报 ResponseFormatter 短文本) → done
    错误路径: 稳定错误码 + state idle (最终状态可回 idle)。
    """

    def __init__(self, config: dict):
        self._config = config
        self._provider = make_provider(config)
        # R8_R4: 快/强/本地三路 (config 可选段; 缺省回落主 provider)
        # 缺省回落主 provider (运行期解析, 保持测试注入 self._provider 语义)
        self._has_fast = bool(config.get("fast_llm"))
        self._has_strong = bool(config.get("strong_llm"))
        self._fast_provider = None
        self._strong_provider = None
        if self._has_fast:
            try:
                self._fast_provider = make_provider(config["fast_llm"])
            except Exception:
                self._fast_provider = None
        if self._has_strong:
            try:
                self._strong_provider = make_provider(config["strong_llm"])
            except Exception:
                self._strong_provider = None
        try:
            lcfg = config.get("local_llm")
            self._local_provider = make_provider(lcfg) if lcfg else None
        except Exception:
            self._local_provider = None
        # R8_R2 (工作包 A): 分文件提示词组合 (恰好一条 system message)
        self._prompt = PromptComposer.from_config(config)
        self._system_text = None
        self._prompt_error = None
        try:
            self._system_text = self._prompt.compose()
        except PromptError as e:
            # 构造期失败不抛: 每轮开始时给稳定错误 (见 run())
            self._prompt_error = e
            LOG.warning("prompt compose failed: %s", e.code)
        # R8_R4_R3_R4: 菜单 AI 模式显式覆盖 (fast/deep/local; auto/缺省=None)
        self._ai_mode_explicit = None
        # R5_R4: 确认门/审计默认按实例 (测试直构隔离); 服务器注入共享会话态
        self._gate = ConfirmationGate()
        self._audit = AgentAudit()
        # R8_R4_R3_R4_R3: 真实断网探测缓存
        self._online_cached = True
        self._online_checked_at = 0.0
        self._online_cache_s = 8.0
        # R8_R2 (工作包 B): text_only=true 时不做任何 TTS 伪造 (含 Fake 电平)
        self._text_only = bool(config.get("text_only", False))
        self._stt = FakeSttProvider(config.get("stt_inject") or {})
        tts_inject = config.get("tts_inject") or {}
        self._tts = FakeTtsProvider(
            error_code=tts_inject.get("error_code", ""))
        self._formatter = ResponseFormatter(config.get("response_policy") or {})
        self._content_chunk_chars = int(config.get("content_chunk_chars", 40))
        self._max_tool_rounds = int(config.get("max_tool_rounds", 4))
        self._max_history = int(config.get("max_history_turns", 8))
        self._history = []      # 跨轮 LLM 历史 (有限轮)
        self._audio_fmt = AudioFormat(
            sample_rate=int(config.get("sample_rate", 16000)),
            bit_depth=int(config.get("bit_depth", 16)),
            channels=int(config.get("channels", 1)),
            byte_order=config.get("byte_order", "little"),
            max_len_bytes=int(config.get("max_audio_bytes", 512 * 1024)))
        self._audio_stop = threading.Event()
        # R8_R3: 真实语音后端。text_only=true 时一律不构造 (零音频副作用)。
        self._capture = None
        self._player = None
        self._audio_last = None          # 最近一次采集元数据（不保存原始音频）
        self._volc_cfg = None            # R8_R3_R3: 火山流式主力 ASR 配置
        self._volc_asr = None
        self._turn_metrics = {}          # §6.3 延迟指标 (不含正文)
        # R8_R3_R3: agentd 侧观测追加进同一语音审计文件 (config voice_audit
        # 或环境变量 HOLOPET_VOICE_AUDIT; O_APPEND 单行写, 不覆盖 C++ 行)
        self._voice_audit = (config.get("voice_audit") or
                             os.environ.get("HOLOPET_VOICE_AUDIT") or "")
        self._va_seq = 0
        self._va_t0 = time.monotonic()
        self._t_llm_done = None
        self._max_wait_s = 40.0
        # R8_R4: 工具回执通道 (WorkerServer 注入; 键 (rid, tool_call_id))
        self._tool_results = {}
        self._tool_cond = threading.Condition()
        self._client_key = None   # 服务器注入: (client,rid,cid) 键前缀
        self._tool_wait_s = 10.0
        if not self._text_only:
            cap_cfg = config.get("audio_capture") or {}
            play_cfg = config.get("audio_playback") or {}
            asr_cfg = config.get("asr") or {}
            tts_cfg = config.get("tts") or {}
            if cap_cfg.get("provider") == "alsa":
                self._capture = make_audio_capture(cap_cfg)
                self._max_wait_s = float(
                    int(cap_cfg.get("max_record_seconds", 15))) + 5.0
                # WAV 容器比裸 PCM 多 ~44B 头: 放行上限按采集上限放大
                self._audio_fmt.max_len_bytes = int(
                    cap_cfg.get("max_audio_bytes",
                                self._audio_fmt.max_len_bytes)) + 4096
            if play_cfg.get("provider") == "alsa":
                self._player = make_audio_player(play_cfg)
            if asr_cfg.get("provider") == "volcengine_streaming":
                self._volc_cfg = asr_cfg
                # 备用 Provider 复用在 asr 块内的 openai_compatible 参数
                self._stt = make_stt_provider(asr_cfg)
            elif asr_cfg.get("provider") == "openai_compatible":
                self._stt = make_stt_provider(asr_cfg)
            if tts_cfg.get("provider") == "openai_compatible":
                self._tts = make_tts_provider(tts_cfg)


    def _vlog(self, rid, kind, **fields):
        """向语音审计追加一行观测 (kind+数值, 无正文); 失败静默 (审计属 C++)。"""
        if not self._voice_audit:
            return
        self._va_seq += 1
        doc = {"event": "obs", "source": "agentd",
               "aseq": self._va_seq, "kind": kind,
               "request_id": rid,
               "t_ms": int((time.monotonic() - self._va_t0) * 1000)}
        doc.update({k: v for k, v in fields.items() if v is not None})
        try:
            with open(self._voice_audit, "a", encoding="utf-8") as f:
                f.write(json.dumps(doc, ensure_ascii=False) + "\n")
                f.flush()
        except OSError:
            pass
    def set_transcript(self, request_id: str, text: str) -> None:
        self._stt.set_transcript(request_id, text)

    def set_ai_mode(self, mode: str) -> None:
        """R8_R4_R3_R4: 每轮 AI 模式 (菜单选择, C++ 经 start_turn 携带)。
        fast/deep/local → 显式覆盖; auto/缺省 → 走既有路由。"""
        self._ai_mode_explicit = (mode if mode in ("fast", "deep", "local")
                                  else None)

    def abort_audio(self) -> None:
        """R8_R3_R3_R2: 异常路径兜底 — 停采集进程并清理临时文件,
        不留 /tmp/holopet_r83_* 残留 (Pi 现场曾因崩溃路径留下 wav)。"""
        try:
            if self._capture is not None:
                self._capture.cancel()
        except Exception:                   # noqa: BLE001 — 兜底清理不抛
            pass
        try:
            if self._volc_asr is not None:
                self._volc_asr.close()
        except Exception:                   # noqa: BLE001
            pass

    # ---- R8_R3_R3_R2 (必修 C §5.2): 每轮唯一 terminal 由 agentd 单点写入 ----
    def _count_page_shown(self, rid: str) -> int:
        if not self._voice_audit:
            return 0
        needle = '"request_id":"' + rid + '"'
        try:
            with open(self._voice_audit, encoding="utf-8",
                      errors="replace") as f:
                n = 0
                for ln in f:
                    if '"page_shown"' not in ln:
                        continue
                    if needle not in ln:
                        continue
                    n += 1
                return n
        except OSError:
            return 0

    def _write_voice_terminal(self, rid: str, tracker: dict) -> None:
        """按 §5.2 聚合字段写 terminal; 页面计数有界等待 C++ page_shown
        落盘 (≤3s, 两次稳定或超时按已观察值如实记录); 无 voice_audit 时跳过。"""
        if not self._voice_audit:
            return
        pages = 0
        prev = -1
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            pages = self._count_page_shown(rid)
            if pages == prev:
                break
            prev = pages
            time.sleep(0.15)
        doc = {
            "event": "terminal",
            "request_id": rid,
            "terminal_result": tracker.get("result", "error"),
            "done_seen": tracker.get("done_seen", "no"),
            "content_event_count": tracker.get("content_count", 0),
            "content_bytes": tracker.get("content_bytes", 0),
            "content_sha256": tracker.get("content_sha",
                                          hashlib.sha256()).hexdigest(),
            "page_shown_count": pages,
            "tts_started_seen": "yes" if tracker.get("tts_started", 0) else "no",
            "tts_level_count": tracker.get("tts_level", 0),
            "tts_finished_seen": "yes" if tracker.get("tts_finished", 0) else "no",
            "tts_played": tracker.get("tts_played", "no"),
            "final_expression": tracker.get("expression", ""),
            "final_runtime_state": tracker.get("state", ""),
            "provider_asr": tracker.get("provider_asr", ""),
            "asr_fallback": tracker.get("asr_fallback", "no"),
            "error_code": tracker.get("error_code", ""),
        }
        try:
            with open(self._voice_audit, "a", encoding="utf-8") as f:
                f.write(json.dumps(doc, ensure_ascii=False) + "\n")
                f.flush()
        except OSError:
            pass

    def _online(self) -> bool:
        """R8_R4_R3_R4_R3: 网络可用性 —
        1) env HOLOPET_OFFLINE=1 / config offline_mode → False;
        2) 否则有界探测 fast/主 provider 主机 TCP 可达性 (结果缓存 8s),
           真实断网时自动切 local_llm (不依赖环境变量)。
        """
        if os.environ.get("HOLOPET_OFFLINE") == "1":
            return False
        if self._config.get("offline_mode", False):
            return False
        # 有界 TCP 探针 (缓存 8s): 任一 fast/主 provider 主机可达 → 在线
        now = time.monotonic()
        if now - self._online_checked_at < self._online_cache_s:
            return self._online_cached
        self._online_checked_at = now
        import socket as _socket
        from urllib.parse import urlparse as _urlparse
        hosts = set()
        for key in ("fast_llm",):
            cfg = self._config.get(key)
            if cfg and cfg.get("base_url"):
                hosts.add(_urlparse(cfg["base_url"]).hostname)
        main_url = self._config.get("base_url")
        if main_url:
            hosts.add(_urlparse(main_url).hostname)
        hosts.discard(None)
        if not hosts:
            # 无云端主机可探测 (fake/本地配置) → 保持在线语义
            self._online_cached = True
            return True
        for h in hosts:
            try:
                with _socket.create_connection(
                        (h, 443), timeout=2.0):
                    self._online_cached = True
                    return True
            except OSError:
                continue
        self._online_cached = False
        return False

    def _await_tool_result(self, rid, tool_call_id, cancel_event):
        """有界等待 C++ tool_result (<=10s, cancel 感知); 超时/取消 → 稳定错误。"""
        deadline = time.monotonic() + self._tool_wait_s
        with self._tool_cond:
            while time.monotonic() < deadline:
                if cancel_event.is_set():
                    return {"error": "cancelled"}
                # R5_R4: 工具结果键位统一 — 服务器按
                # (client_id, rid, cid) 存, 直构 TurnRunner 测试按 (rid, cid);
                # 双键查找兼容两条路径
                doc = None
                if self._client_key is not None:
                    doc = self._tool_results.get(
                        (self._client_key, rid, tool_call_id))
                if doc is None:
                    doc = self._tool_results.get((rid, tool_call_id))
                if doc is not None:
                    return {"ok": bool(doc.get("ok")),
                            "code": doc.get("code", ""),
                            "result": doc.get("result", {})}
                self._tool_cond.wait(timeout=0.1)
        return {"error": "tool_timeout"}

    def run(self, rid: str, cancel_event: threading.Event, send):
        """一轮 (由单 worker 执行器调用): 事件经 send(rid, type, **fields) 发出"""
        send(rid, "state", value="listening")

        # 1) R8_R3: 配置 audio_capture.provider=alsa 时做真实采集;
        #    否则沿用 R8_R2 合同载体 Fake PCM (text_only / 离线测试路径)。
        audio = b""
        import time as _time
        _t_turn0 = _time.monotonic()
        _t_stop = _t_final = _t_llm_first = _t_llm_done = _t_tts_req = None
        _asr_used = ""
        _fallback_used = "no"
        _primary_attempted = "no"
        _primary_result = ""
        _fallback_reason = ""
        _fallback_count = 0
        _stt_done = False
        if self._capture is not None and not self._online():
            # R8_R4_R3_R4_R4: 离线时云端 ASR 必失败 — 可读稳定错误,
            # 不空等网络超时 (文字/本地模型路径不受影响)
            send(rid, "state", value="idle")
            send(rid, "error", code="stt_offline",
                 text="当前离线，语音暂不可用")
            return
        if self._capture is not None:
            if not self._capture.start(rid):
                reason = (self._capture.config_reason()
                          or "capture_process_error")
                self._audio_failure(rid, send, reason)
                return
            volc = None
            if self._volc_cfg is not None:
                # 测试注入点 (_volc_asr) 与生产构造等价
                volc = self._volc_asr or VolcengineStreamingSttProvider(
                    self._volc_cfg)
                _asr_used = "volcengine_streaming"
                _primary_attempted = "yes"
                self._vlog(rid, "asr_provider_selected", value="volcengine_streaming")
                if volc.start() is not None:
                    _primary_result = volc.last_error() or "unknown"
                    LOG.warning("volc asr start failed: %s", _primary_result)
                    self._vlog(rid, "asr_primary_error", reason=_primary_result)
                    volc.close()
                    volc = None
                else:
                    _primary_result = "success"
            waited = 0.0
            while waited < self._max_wait_s:
                if cancel_event.is_set():
                    if volc is not None:
                        volc.cancel()
                    self._capture.cancel()
                    return
                if self._audio_stop.wait(0.2):
                    break
                waited += 0.2
                # 流式: 边录边发增量 PCM (~200ms/包)
                # R8_R3_R3_R2: read_incremental 返回 bytes 本体 — 直接 feed;
                # 不得对 bytes 迭代 (逐字节迭代产生 int, 真机曾因此 TypeError)。
                if volc is not None:
                    chunk = self._capture.read_incremental()
                    if chunk:
                        volc.feed(chunk)
            if cancel_event.is_set():
                if volc is not None:
                    volc.cancel()
                self._capture.cancel()
                return
            if volc is not None:
                chunk = self._capture.read_incremental()
                if chunk:
                    volc.feed(chunk)
            cap = self._capture.stop()
            if cap.error:
                if volc is not None:
                    volc.cancel()
                self._audio_failure(rid, send, cap.error.code)
                return
            _t_stop = _time.monotonic()
            audio = cap.wav_bytes
            self._audio_last = {"duration_ms": cap.duration_ms,
                                "pcm_bytes": cap.pcm_bytes,
                                "device_class": cap.device_class,
                                "exit_code": cap.exit_code}
            LOG.info("capture done rid=%s duration_ms=%d pcm_bytes=%d dev=%s",
                     rid, cap.duration_ms, cap.pcm_bytes, cap.device_class)
            if volc is not None:
                result = volc.finish(timeout_ms=int(
                    (self._volc_cfg.get("final_timeout_ms", 30000))))
                st = volc.stats()
                _t_final = _time.monotonic()
                LOG.info("volc asr done rid=%s error=%s stats=%s",
                         rid, result.error.code if result.error else "-",
                         json.dumps(st.as_dict(), ensure_ascii=False))
                if result.error and result.error.code in FALLBACKABLE \
                        and _fallback_used == "no":
                    _fallback_used = "yes"
                    _fallback_reason = result.error.code
                    _fallback_count = 1
                    self._vlog(rid, "asr_fallback", reason=result.error.code)
                    result = self._stt.transcribe(rid, audio, self._audio_fmt)
                    _stt_done = True
                elif result.error:
                    self._audio_failure(rid, send, result.error.code)
                    return
                elif result.text:
                    _primary_result = "success"
                    _asr_used = "volcengine_streaming"
                    _stt_done = True
                    self._vlog(rid, "asr_first_partial", first_partial_ms=st.first_partial_ms)
                    self._vlog(rid, "asr_final", final_ms=st.final_ms)
                volc.close()
            else:
                _asr_used = "openai_compatible"
                if _primary_attempted == "yes" and _fallback_used == "no":
                    _fallback_used = "yes"
                    _fallback_reason = _primary_result or "primary_start_failed"
                    _fallback_count = 1
                    self._vlog(rid, "asr_fallback", reason=_fallback_reason)
                else:
                    self._vlog(rid, "asr_provider_selected", value="openai_compatible")
                result = self._stt.transcribe(rid, audio, self._audio_fmt)
                _stt_done = True
                _t_final = _time.monotonic()
            self._turn_metrics.update(
                ASR_PRIMARY="volcengine_streaming",
                ASR_PRIMARY_ATTEMPTED=_primary_attempted,
                ASR_PRIMARY_RESULT=_primary_result or "NOT_ATTEMPTED",
                ASR_FALLBACK_USED=_fallback_used,
                ASR_FALLBACK_REASON=_fallback_reason or "none",
                ASR_FALLBACK_COUNT=str(_fallback_count),
                CAPTURE_START_TO_FIRST_PARTIAL_MS=(st.first_partial_ms
                                                   if (volc is not None
                                                       and st.first_partial_ms
                                                       is not None)
                                                   else "NOT_OBSERVED"),
                STOP_TO_ASR_FINAL_MS=(int((_t_final - _t_stop) * 1000)
                                      if _t_final and _t_stop else
                                      "NOT_OBSERVED"),
                ASR_PROVIDER_USED=_asr_used)
        else:
            _asr_used = "fake"
            audio = b"\x00\x01\x02"
        if _asr_used == "fake":
            for _ in range(40):       # 最多 ~2s
                if cancel_event.is_set():
                    # R4-R1 (D6): 不中途发终态 cancel — 由 _run_turn_job 在
                    # turn 真正停止后发出
                    return
                if self._audio_stop.wait(0.05):
                    break
            if cancel_event.is_set():
                return
        # 2) STT: 单一结果出口 — capture 分支已完成识别的路径不得再次调用
        if not _stt_done:
            result = self._stt.transcribe(rid, audio, self._audio_fmt)
        if result.error:
            send(rid, "state", value="idle")
            send(rid, "error", code="stt_error",
                 text=f"stt:{result.error.code}")
            return
        user_text = result.text
        # 默认日志不记录转录正文，只记录长度和不可逆摘要。
        transcript_sha = hashlib.sha256(
            user_text.encode("utf-8", errors="replace")).hexdigest()[:16]
        LOG.info("transcript rid=%s chars=%d sha256=%s", rid,
                 len(user_text), transcript_sha)
        send(rid, "transcript", text=user_text)
        # R5_R4 (C): 待确认回复解释 — 上一轮遗留 WAIT_CONFIRMATION 时,
        # 本条输入按确认合同处理 (不进入常规路由/模型)
        if self._gate.pending is not None:
            self._handle_confirmation_turn(rid, user_text, send,
                                           cancel_event, _t_turn0)
            return
        send(rid, "state", value="thinking")
        _t_llm0 = _time.monotonic()
        self._t_llm0 = _t_llm0
        # R8_R4: 唯一路由决策 (每轮一次, 审计原因短码; 非字符数唯一判据)
        _route, _route_reason = route_decision(
            user_text, self._online(),
            self._ai_mode_explicit or detect_explicit_override(user_text))
        _route_provider = self._provider
        if _route == "strong":
            if self._has_strong:
                if self._strong_provider is None:
                    # R8_R4_R3_R4_R5_R3 (B): 指定模型 provider 不可用 —
                    # 稳定上报, 绝不静默换模型冒充 strong 成功
                    send(rid, "state", value="idle")
                    send(rid, "error", code="PROVIDER_MODEL_BLOCKED",
                         text="strong 模型不可用")
                    return
                _route_provider = self._strong_provider
        elif _route == "fast":
            if self._has_fast:
                if self._fast_provider is None:
                    send(rid, "state", value="idle")
                    send(rid, "error", code="PROVIDER_MODEL_BLOCKED",
                         text="fast 模型不可用")
                    return
                _route_provider = self._fast_provider
        elif _route == "local_llm":
            if self._local_provider is None:
                send(rid, "state", value="idle")
                send(rid, "error", code="local_llm_unavailable",
                     text="本地模型未就绪")
                return
            _route_provider = self._local_provider
        # R8_R4_R3_R4_R5_R3 (B): 记账本轮实际请求的模型名 (延迟记录配套);
        # 缺省回落主 provider 时取主配置 model
        _model_cfg_key = {"strong": "strong_llm", "fast": "fast_llm",
                          "local_llm": "local_llm"}.get(_route, "")
        _model_name = ((self._config.get(_model_cfg_key) or {}).get("model")
                       if _model_cfg_key else "")
        if not _model_name:
            _model_name = str(self._config.get("model", ""))
        self._turn_metrics["MODEL"] = _model_name
        self._turn_metrics["ROUTE"] = _route
        self._turn_metrics["ROUTE_REASON"] = _route_reason
        self._vlog(rid, "route_decision", route=_route, reason=_route_reason)
        LOG.info("turn begin rid=%s route=%s reason=%s", rid, _route,
                 _route_reason)

        # R8_R4_R2 (7.3): 用户可见等待反馈 — strong 立即发短提示 (300ms 内);
        # fast/local 用软等待计时器, soft_wait_ms 内无首段内容才提示;
        # 首段内容/错误/取消到达即解除, 慢回答期间绝不切 Error。
        _wait_timer = None
        _waiting_disarmed = False

        def _disarm_waiting():
            nonlocal _waiting_disarmed
            if _wait_timer is not None and not _waiting_disarmed:
                _wait_timer.cancel()
                _waiting_disarmed = True

        if _route == "strong":
            send(rid, "waiting",
                 text=str(self._config.get(
                     "waiting_text_strong",
                     "这个问题要多想一会儿，正在认真找答案")))
            self._turn_metrics["ROUTE_TO_WAITING_MS"] = int(
                (_time.monotonic() - _t_llm0) * 1000)
        elif _route in ("fast", "local_llm"):
            _soft_ms = int(self._config.get("soft_wait_ms", 4000))
            _soft_text = str(self._config.get("waiting_text_soft",
                                              "正在回答，请稍候"))
            _wait_timer = threading.Timer(
                _soft_ms / 1000.0,
                lambda: (send(rid, "waiting", text=_soft_text)
                         if not cancel_event.is_set() else None))
            _wait_timer.daemon = True
            _wait_timer.start()

        # 3) R5_R4: 独立 Agent Runtime 驱动 (规划/工具循环/预算/确认/审计)
        if not self._system_text:
            send(rid, "state", value="idle")
            send(rid, "error", code="prompt_error",
                 text=(self._prompt_error.code if self._prompt_error
                       else "prompt_error"))
            return

        def _esc():
            """fast→strong 升级 (最多一次, 由 runtime 保证)。"""
            self._turn_metrics["ROUTE_ORIGINAL"] = "fast"
            self._turn_metrics["ROUTE"] = "strong"
            self._turn_metrics["ESCALATED"] = "fast_to_strong"
            self._vlog(rid, "escalate", from_route="fast",
                       to_route="strong", reason="capability_hint")
            LOG.info("escalate rid=%s fast->strong", rid)
            return self._strong_provider

        runtime = AgentRuntime(
            model_client=_route_provider,
            tool_executor=_ToolExecutorAdapter(self, rid, send,
                                               cancel_event),
            audit=self._audit,
            confirmation=self._gate,
            budgets=AgentBudgets(
                max_rounds=int(self._config.get("max_tool_rounds", 4)),
                max_per_round=int(self._config.get(
                    "max_tools_per_round", 4)),
                max_total=int(self._config.get("max_total_tool_exec", 8))),
            store=_shared_store(self._config),
            max_fixup_requests=int(self._config.get("max_fixup_requests", 1)),
            escalate_cb=(_esc if (_route == "fast"
                                  and self._strong_provider is not None)
                         else None),
            insufficient_markers=tuple(
                self._config.get("escalation_markers")
                or DEFAULT_INSUFFICIENT_MARKERS),
        )
        out = runtime.run_turn(request_id=rid, user_text=user_text,
                               model_alias=_route,
                               system_text=self._system_text,
                               cancel_event=cancel_event)
        _disarm_waiting()

        if out.state.value == "wait_confirmation":
            self._turn_metrics["CONFIRMATION_PENDING"] = "yes"
            ask = out.answer_text or "请确认是否执行"
            for i in range(0, len(ask), self._content_chunk_chars):
                send(rid, "content", text=ask[i:i + self._content_chunk_chars])
            send(rid, "response_complete")
            send(rid, "expression", value="curious")
            _t_llm_done = _time.monotonic()
            self._t_llm_done = _t_llm_done
            if not self._text_only:
                self._speak(rid, ask, cancel_event, send)
            self._finish_turn_metrics(rid, send, _t_turn0, _t_llm_done)
            return

        if out.state.value == "cancelled":
            return          # 取消: 不产生迟到内容/TTS/历史 (与原合同一致)

        if out.state.value in ("model_failed", "timeout", "tool_failed"):
            from .providers.errors import EVENT_ERROR_CODE as _EVC
            event_code = _EVC.get(out.code)
            if event_code is None and out.code not in (
                    "agent_budget_exceeded", "tool_chain_blocked",
                    "confirmation_expired", "tool_failed",
                    "duplicate_ignored", "model_format_error"):
                event_code = "llm_failed"
            send(rid, "state", value="idle")
            if out.state.value == "timeout":
                send(rid, "error", code="llm_error", text="llm_timeout")
            else:
                send(rid, "error",
                     code=(event_code or out.code), text=out.code)
            self._turn_metrics["ERROR_CODE"] = out.code
            return

        # 4) 回答 → 解释层 → Formatter → content 分片 + response_complete
        content = out.answer_text
        if content:
            # R8_R2 (C): 独立解释层 — 合法 JSON {text,emotion} 取 text;
            # 纯文本/畸形 JSON 全文回退; emotion 白名单, 未知回落 neutral
            interp = interpret(content)
            display_text, speak_text = self._formatter.format(interp.text)
            # content 0..N: 显示层完整内容, 分片不截断
            for i in range(0, len(display_text), self._content_chunk_chars):
                send(rid, "content",
                     text=display_text[i:i + self._content_chunk_chars])
            send(rid, "response_complete")
            # 情绪: 模型原始响应 (Fake 路径) 优先, 否则解释层结果;
            # 两者都过同一白名单
            emotion = str(out.resp_extra.get("emotion") or "").strip().lower()
            if emotion not in ("neutral", "happy", "curious", "surprised",
                               "sad", "sleepy", "concerned"):
                emotion = interp.emotion
            send(rid, "expression", value=emotion)
            _t_llm_done = _time.monotonic()
            self._t_llm_done = _t_llm_done
            if _t_final:
                self._turn_metrics["ASR_FINAL_TO_LLM_FIRST_CONTENT_MS"] = int(
                    (_t_llm0 - _t_final) * 1000)
            if not self._text_only:
                self._speak(rid, speak_text, cancel_event, send)
            # text_only=true: 无任何 TTS 事件 (不伪造语音)
        else:
            # R8_R4_R3_R1: 空回答给温和反馈 (Pi §11 实测"第二次无结果" —
            # DeepSeek 偶发返回空 content 时用户无感知)。仍恰好一次
            # response_complete, 但发一条固定提示内容并走 TTS 播报,
            # 绝不伪造模型回答正文。
            self._turn_metrics["EMPTY_ANSWER_FALLBACK"] = "yes"
            _t_llm_done = _time.monotonic()
            self._t_llm_done = _t_llm_done
            fallback = str(self._config.get(
                "empty_answer_text",
                "抱歉，我这次没有给出回答，请再说一次吧"))
            for i in range(0, len(fallback), self._content_chunk_chars):
                send(rid, "content",
                     text=fallback[i:i + self._content_chunk_chars])
            send(rid, "response_complete")
            send(rid, "expression", value="neutral")
            if not self._text_only:
                self._speak(rid, fallback, cancel_event, send)
        self._finish_turn_metrics(rid, send, _t_turn0, _t_llm_done)
        return

    def _finish_turn_metrics(self, rid, send, _t_turn0, _t_llm_done):
        """R5_R4: 收口记账 (TURN_TOTAL/LLM_ANSWER_MS/审计下沉) + idle/done。"""
        import time as _time
        self._turn_metrics["TURN_TOTAL_MS"] = int(
            (_time.monotonic() - _t_turn0) * 1000)
        # R8_R4_R3_R4_R5_R3 (B): 每轮 LLM 应答延迟 (路由决策到首段内容)
        if _t_llm_done:
            self._turn_metrics["LLM_ANSWER_MS"] = int(
                (_t_llm_done - getattr(self, "_t_llm0", _t_llm_done)) * 1000)
        self._vlog(rid, "turn_metrics",
                   **{k: str(v) for k, v in self._turn_metrics.items()})
        LOG.info("turn_metrics rid=%s %s", rid,
                 json.dumps(self._turn_metrics, ensure_ascii=False))
        send(rid, "state", value="idle")
        send(rid, "done")

    def _handle_confirmation_turn(self, rid, user_text, send, cancel_event,
                                  _t_turn0):
        """R5_R4 (C): 待确认回复回合 — 解释确认/取消/过期并执行恰好一次。"""
        send(rid, "state", value="thinking")
        runtime = AgentRuntime(
            model_client=self._provider,        # 本路径不请求模型
            tool_executor=_ToolExecutorAdapter(self, rid, send,
                                               cancel_event),
            audit=self._audit,
            confirmation=self._gate,
            store=_shared_store(self._config),
        )
        out = runtime.handle_confirmation(
            request_id=rid, user_text=user_text, model_alias="confirmation",
            cancel_event=cancel_event)
        self._turn_metrics["CONFIRMATION_TURN"] = "yes"
        self._turn_metrics["CONFIRMATION_OUTCOME"] = out.code
        text = out.answer_text or ""
        if out.state.value == "wait_confirmation":
            # 其他输入: 重新询问 (不延长 TTL, 由门内判定)
            text = text or "请明确回复：确认 或 取消"
        if out.state.value == "cancelled":
            text = text or "已取消，未做任何更改"
        if out.state.value in ("tool_failed",):
            send(rid, "state", value="idle")
            send(rid, "error", code=out.code, text=out.code)
            self._finish_turn_metrics(rid, send, _t_turn0, None)
            return
        if text:
            interp = interpret(text)
            display_text, speak_text = self._formatter.format(interp.text)
            for i in range(0, len(display_text), self._content_chunk_chars):
                send(rid, "content",
                     text=display_text[i:i + self._content_chunk_chars])
            send(rid, "response_complete")
            send(rid, "expression", value=interp.emotion)
            if not self._text_only:
                self._speak(rid, speak_text, cancel_event, send)
        self._finish_turn_metrics(rid, send, _t_turn0, None)

    def _audio_failure(self, rid, send, reason):
        """采集/ASR 前置失败: 稳定原因码 + 回 Idle (不得静默继续)。"""
        send(rid, "state", value="idle")
        send(rid, "error", code="stt_error", text="stt:%s" % reason)

    def _speak(self, rid, text, cancel_event, send):
        """TTS 合同: tts_started → chunks(tts_level) → tts_finished。

        R8_R3_R3: voice 取嵌套 tts.voice (不得被顶层 config.voice 覆盖);
        全程写可观察 obs/日志 (tts_request_started/succeeded/failed/
        playback_started/finished), 正文不落日志。"""
        import time as _time
        _t_req = _time.monotonic()
        voice = (self._config.get("tts") or {}).get("voice") or ""
        self._vlog(rid, "tts_request_started")
        audio = self._tts.synthesize(rid, text, voice)
        _req_ms = int((_time.monotonic() - _t_req) * 1000)
        if self._turn_metrics is not None and getattr(self, "_t_llm_done", None) is not None:
            self._turn_metrics["LLM_COMPLETE_TO_TTS_REQUEST_MS"] = int(
                (_t_req - self._t_llm_done) * 1000)
        if audio.error:
            self._vlog(rid, "tts_request_failed", code=audio.error.code,
                 elapsed_ms=_req_ms)
            LOG.warning("tts request failed rid=%s code=%s elapsed_ms=%d",
                        rid, audio.error.code, _req_ms)
            send(rid, "error", code="tts_error",
                 text=f"tts:{audio.error.code}")
            send(rid, "state", value="idle")
            return
        self._vlog(rid, "tts_request_succeeded", bytes=len(audio.audio),
                 mime=audio.mime, duration_ms=audio.duration_ms,
                 request_elapsed_ms=_req_ms)
        LOG.info("tts request ok rid=%s bytes=%d mime=%s duration_ms=%d "                 "elapsed_ms=%d", rid, len(audio.audio), audio.mime,
                 audio.duration_ms, _req_ms)
        if audio.error:
            send(rid, "error", code="tts_error",
                 text=f"tts:{audio.error.code}")
            send(rid, "state", value="idle")
            return
        if self._player is not None and audio.audio:
            started = {"ok": False}

            def _on_started():
                started["ok"] = bool(send(rid, "state", value="speaking")) and \
                    bool(send(rid, "tts_started"))
                if started["ok"]:
                    _t_play = _time.monotonic()
                    self._vlog(rid, "tts_playback_started")
                    if self._turn_metrics is not None:
                        self._turn_metrics["TTS_REQUEST_TO_PLAYBACK_MS"] = int(
                            (_t_play - _t_req) * 1000)

            res = self._player.play(rid, audio.audio, audio.mime,
                                    cancel_event=cancel_event,
                                    on_started=_on_started,
                                    estimator=lambda _b: audio.duration_ms)
            if res.error_code == "playback_cancelled":
                return      # R4-R1 (D6): 终态 cancel 由外层发出
            if res.error_code:
                send(rid, "error", code="tts_error",
                     text="tts:%s" % res.error_code)
                send(rid, "state", value="idle")
                return
            if not started["ok"]:
                return
            for chunk in audio.chunks:
                if cancel_event.is_set():
                    return
                if not send(rid, "tts_level", rms=chunk.rms):
                    return
            send(rid, "tts_finished")
            return
        if not send(rid, "state", value="speaking"):
            return
        if not send(rid, "tts_started"):
            return
        for chunk in audio.chunks:
            if cancel_event.is_set():
                return      # R4-R1 (D6): 停止播放, 终态 cancel 由外层发出
            if not send(rid, "tts_level", rms=chunk.rms):
                return
        send(rid, "tts_finished")


class ClientConn:
    """单连接: 发送串行化 + 存活状态 (R3)。

    所有 JSONL 发送必须经 send(); 内部持锁, 保证一行消息不可交错。
    发送失败 → 标记 dead 并返回 False, 调用方停止后续发送。
    """

    def __init__(self, conn):
        self.sock = conn
        self.send_lock = threading.Lock()
        self.dead = False

    def send(self, rid: str, msg_type: str, **fields) -> bool:
        if self.dead:
            return False
        line = protocol.encode(
            protocol.make_message(rid, msg_type, **fields)).encode("utf-8")
        try:
            with self.send_lock:
                self.sock.sendall(line)
            return True
        except (ConnectionError, OSError):
            self.dead = True
            return False


class _TurnWorker:
    """R4-R1 (D7): 有界单 worker (daemon 线程) — 替代 ThreadPoolExecutor。

    退出可兑现 deadline: shutdown(timeout) 在超时后如实报告未收尾,
    不无限等待; 线程为 daemon, 不会卡住进程退出 (systemd 可收尾)。
    """

    def __init__(self, target):
        self._q = queue.Queue(maxsize=1)
        self._target = target
        self._stopped = False
        self._thread = threading.Thread(target=self._loop, daemon=True,
                                        name="turn-worker")
        self._thread.start()

    def _loop(self):
        while True:
            item = self._q.get()
            if item is None:
                break
            fn, args = item
            try:
                fn(*args)
            except Exception:    # noqa: BLE001 — _run_turn 自身已兜底
                pass
        self._stopped = True

    def submit(self, fn, *args) -> bool:
        if self._stopped:
            return False
        try:
            self._q.put_nowait((fn, args))
            return True
        except queue.Full:
            return False

    def shutdown(self, timeout: float) -> bool:
        try:
            self._q.put_nowait(None)
        except queue.Full:
            pass
        self._thread.join(timeout)
        return not self._thread.is_alive()


class WorkerServer:
    # R4-R1 (D5): transcript 资源上限
    MAX_TRANSCRIPT_ENTRIES = 64
    MAX_TRANSCRIPT_ENTRY_BYTES = 8 * 1024
    MAX_TRANSCRIPT_TOTAL_BYTES = 256 * 1024

    def __init__(self, config: dict, host: str = "127.0.0.1", port: int = 47650,
                 uds_path: str = ""):
        self._config = config
        self._host = host
        self._port = port
        self._uds_path = uds_path
        if uds_path:
            if not hasattr(socket, "AF_UNIX"):
                raise ValueError("Unix Domain Socket 不受此平台支持 "
                                 "(Windows 开发请用 loopback TCP)")
        elif host not in ("127.0.0.1", "localhost"):
            raise ValueError("TCP 模式只允许监听回环地址 (生产用 --uds)")
        # R3: 共享状态锁 — 保护 flags/transcripts/active
        self._state_lock = threading.Lock()
        self._cancel_flags = {}          # request_id -> {"cancel","stop"}
        # R4-R1 (D5): transcript 按 (连接, request_id) 绑定
        self._transcripts = {}           # (conn_id, rid) -> text
        self._tool_results = {}          # (conn_id, rid, tool_call_id) -> doc
        self._tool_cond = threading.Condition()
        self._active = None              # (ClientConn, request_id) or None
        self._stt = FakeSttProvider(self._config.get("stt_inject") or {})
        # R4-R1 (D7): 有界 turn 执行 — 单 daemon worker, 最多 1 个 active
        self._turn_worker = _TurnWorker(self._run_turn_job)
        self._runner_factory = TurnRunner   # 测试注入点 (阻塞 Provider 用例)
        # R4 (P2) + R4-R1 (D7): 生命周期 — 所有已接受 socket/reader 登记
        self._stop_event = threading.Event()
        self._listener = None
        self._shutdown_done = False
        self._uds_created = False
        self._uds_identity = None       # R4-R1 (10.3): (st_dev, st_ino)
        self._clients = set()            # 已接受 ClientConn (持 _state_lock 或专用锁)
        self._readers = set()            # reader 线程
        self._clients_lock = threading.Lock()
        self._turn_finished = threading.Event()   # 当前 turn 真正退出信号

    # ---- 共享状态 (全部持 _state_lock) ----

    def _register_turn(self, conn: ClientConn, rid: str):
        with self._state_lock:
            if self._active is not None:
                return False
            self._active = (conn, rid)
            self._cancel_flags[rid] = {"cancel": threading.Event(),
                                       "stop": threading.Event()}
            self._turn_finished.clear()
            return True

    def _finish_turn(self, rid: str):
        # R8_R4: 清理本轮工具回执
        with self._tool_cond:
            for k in [k for k in self._tool_results if k[1] == rid]:
                self._tool_results.pop(k, None)
        with self._state_lock:
            self._cancel_flags.pop(rid, None)
            self._transcripts.pop((id(self._active[0]), rid), None) \
                if self._active else None
            self._active = None
        self._turn_finished.set()

    def _set_transcript(self, conn: ClientConn, rid: str, text: str) -> bool:
        """R4-R1 (D5): transcript 绑定连接 + 条目/单条/总字节上限;
        返回 False=被拒 (超限或非 active 写入)。"""
        if len(text.encode("utf-8", "replace")) > self.MAX_TRANSCRIPT_ENTRY_BYTES:
            return False
        with self._state_lock:
            key = (id(conn), rid)
            if len(self._transcripts) >= self.MAX_TRANSCRIPT_ENTRIES and \
                    key not in self._transcripts:
                return False
            total = sum(len(v.encode("utf-8", "replace"))
                        for v in self._transcripts.values())
            if total + len(text.encode("utf-8", "replace")) \
                    > self.MAX_TRANSCRIPT_TOTAL_BYTES:
                return False
            self._transcripts[key] = text
        self._stt.set_transcript(rid, text)
        return True

    def _signal(self, rid: str, key: str) -> bool:
        with self._state_lock:
            state = self._cancel_flags.get(rid)
            if state is None:
                return False
            state[key].set()
            return True

    def _cancel_connection_turns(self, conn: ClientConn):
        """disconnect 收尾: 取消该连接的 active turn, 清理该连接暂存。"""
        with self._state_lock:
            active = self._active
            if active is not None and active[0] is conn:
                state = self._cancel_flags.get(active[1])
                if state is not None:
                    state["cancel"].set()
            # 只清该连接自己的 transcript (其他连接同 id 不受影响)
            self._transcripts.pop((id(conn), active[1]), None) \
                if active is not None and active[0] is conn else None
            # _active 由 _finish_turn 清 (turn 线程退出时)

    # ---- 传输 ----

    def serve_forever(self):
        if self._uds_path:
            return self._serve_uds()
        return self._serve_tcp()

    def _serve_tcp(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as srv:
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((self._host, self._port))
            srv.listen(4)
            srv.settimeout(0.5)          # R4: 可中断 accept (配合 shutdown)
            self._listener = srv
            LOG.info("holopet-agentd listening on tcp %s:%s (provider=%s)",
                     self._host, self._port,
                     self._config.get("provider", "fake"))
            while not self._stop_event.is_set():
                try:
                    conn, _addr = srv.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break            # 监听 socket 已被 shutdown() 关闭
                self._start_reader(conn)
        self._listener = None
        LOG.info("holopet-agentd tcp listener closed")

    def _prepare_uds_path(self):
        """R4-R1 (10.3): UDS 路径所有权。

        - 路径不存在 → 可 bind
        - 普通文件/目录/符号链接 → fail closed
        - socket: 先探测是否活跃服务 (connect 成功 → 拒绝第二实例);
          connect 失败且 lstat 仍指向同一 inode 才允许 unlink (stale)
        """
        import os
        import stat as stat_mod
        path = self._uds_path
        if not os.path.exists(path) and not os.path.lexists(path):
            return True
        st = os.lstat(path)               # lstat: 符号链接本身可见
        if not stat_mod.S_ISSOCK(st.st_mode):
            raise OSError(f"UDS path exists and is not a socket "
                          f"(fail closed): {path} (mode={oct(st.st_mode)})")
        # 探测活跃实例
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        probe.settimeout(0.5)
        try:
            probe.connect(path)
            active = True
        except OSError:
            active = False
        finally:
            probe.close()
        if active:
            raise OSError(f"active agentd already listening on {path} "
                          f"(refusing second instance)")
        # 确认 stale: lstat 仍指向同一 inode 才 unlink
        st2 = os.lstat(path)
        if (st2.st_dev, st2.st_ino) != (st.st_dev, st.st_ino):
            raise OSError(f"UDS path replaced during check (fail closed): {path}")
        os.unlink(path)                   # stale socket (上次未清理)
        return True

    def _serve_uds(self):
        import os
        import stat as stat_mod
        self._prepare_uds_path()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as srv:
            srv.bind(self._uds_path)
            srv.listen(4)
            srv.settimeout(0.5)          # R4: 可中断 accept
            self._listener = srv
            self._uds_created = True
            # R4-R1 (10.3): 记录 bind 后的 dev/inode — 退出只删除
            # "自己创建且身份未变"的 socket
            st = os.lstat(self._uds_path)
            self._uds_identity = (st.st_dev, st.st_ino)
            LOG.info("holopet-agentd listening on uds %s (provider=%s)",
                     self._uds_path, self._config.get("provider", "fake"))
            while not self._stop_event.is_set():
                try:
                    conn, _addr = srv.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break            # 监听 socket 已被 shutdown() 关闭
                self._start_reader(conn)
        self._listener = None
        self._cleanup_uds_path()
        LOG.info("holopet-agentd uds listener closed")

    def _cleanup_uds_path(self):
        """只删除自己创建且身份未变的 socket; 被替换 → 保留并报警。"""
        import os
        import stat as stat_mod
        if not self._uds_created:
            return
        path = self._uds_path
        try:
            st = os.lstat(path)
        except OSError:
            self._uds_created = False
            return
        if not stat_mod.S_ISSOCK(st.st_mode) or                 (st.st_dev, st.st_ino) != self._uds_identity:
            LOG.warning("UDS path replaced (not removing): %s", path)
            self._uds_created = False
            return
        try:
            os.unlink(path)
        except OSError:
            pass
        self._uds_created = False

    def shutdown(self, timeout: float = 5.0):
        """R4-R1 (D7): 有界优雅停止 — 幂等, deadline 实际控制等待。

        顺序: 拒绝新连接 (stop_event) → 关闭 listener → 关闭已接受
        socket → 置 cancel → 等 reader 与 turn 至 deadline → 清理资源。
        返回诊断 dict: {"graceful": bool, "pending_readers": int,
                        "turn_finished": bool, "timed_out": bool}。
        deadline 到达仍有任务 → graceful=False (如实报告, 不得伪称
        graceful PASS; daemon 线程由进程/systemd 收尾)。
        """
        if self._shutdown_done:
            return {"graceful": True, "pending_readers": 0,
                    "turn_finished": True, "timed_out": False}
        self._shutdown_done = True
        self._stop_event.set()
        listener = self._listener
        if listener is not None:
            try:
                listener.close()
            except OSError:
                pass
        # 关闭所有已接受 socket (reader recv 返回空 → 快速退出)
        with self._clients_lock:
            clients = list(self._clients)
        for c in clients:
            try:
                c.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                c.sock.close()
            except OSError:
                pass
        # 取消所有 active turn (有限时间内生效)
        with self._state_lock:
            for _rid, state in self._cancel_flags.items():
                state["cancel"].set()
                state["stop"].set()
        # 有界等待: turn worker + readers, 各占剩余 deadline 份额
        deadline = time.monotonic() + timeout
        turn_ok = self._turn_worker.shutdown(max(0.0, deadline - time.monotonic()))
        with self._clients_lock:
            readers = list(self._readers)
        pending = 0
        for r in readers:
            remain = deadline - time.monotonic()
            if remain <= 0:
                pending += 1
                continue
            r.join(remain)
            if r.is_alive():
                pending += 1
        # UDS 文件由 _serve_uds 的 with 块退出路径清理; 此处兜底
        self._cleanup_uds_path()
        timed_out = (time.monotonic() >= deadline)
        graceful = turn_ok and pending == 0 and not timed_out
        if not graceful:
            LOG.warning("shutdown NOT graceful: turn_finished=%s "
                        "pending_readers=%d timed_out=%s",
                        turn_ok, pending, timed_out)
        else:
            LOG.info("holopet-agentd shutdown complete (graceful)")
        return {"graceful": graceful, "pending_readers": pending,
                "turn_finished": turn_ok, "timed_out": timed_out}

    def _start_reader(self, conn):
        t = threading.Thread(target=self._serve_client, args=(conn,),
                             daemon=True, name="reader")
        with self._clients_lock:
            self._readers.add(t)
        t.start()

    def _serve_client(self, conn):
        client = ClientConn(conn)
        with self._clients_lock:
            self._clients.add(client)
        buf = b""
        try:
            with conn:
                client.send("", "hello")
                while not client.dead:
                    data = conn.recv(4096)
                    if not data:
                        # R4-R1 (D4): EOF 带非空残余半包 → truncated_line 诊断
                        if buf:
                            LOG.warning("truncated_line: EOF with %d bytes "
                                        "unframed residual", len(buf))
                        break
                    lines, buf, overflow = protocol.read_lines(buf, data)
                    if overflow:
                        # R4: 无换行部分包超过 MAX_LINE_BYTES → fail closed:
                        # 无法取得 request_id, 断开连接并释放缓冲 (不无限增长)
                        LOG.warning("partial line overflow, closing connection")
                        break
                    for line in lines:
                        self._handle_line(client, line)
        except (ConnectionError, OSError):
            pass
        finally:
            # R3: disconnect → 停止该连接 turn + 清理; 后台线程不再重试发送
            self._cancel_connection_turns(client)
            client.dead = True
            with self._clients_lock:
                self._clients.discard(client)
                self._readers.discard(threading.current_thread())

    def _handle_line(self, client: ClientConn, line: str):
        msg = protocol.parse_line(line)
        # R4-R1 (D3): 服务端按"请求方向"校验 (事件类型进入 → wrong_direction)
        err_code = protocol.validate_request(msg)
        mtype = msg.get("type", "")
        rid = str(msg.get("request_id", ""))

        if err_code is not None:
            # R3: 结构化拒绝 (含坏 JSON/未知 type/超长行/缺 request_id)
            client.send(rid, "error", code=err_code,
                        text=err_code if mtype == "__bad__" else
                        f"rejected type={mtype!r}")
            return

        if mtype == "start_turn":
            # R3: reader 内立即 busy 拒绝 (有界: 单 worker)
            if not self._register_turn(client, rid):
                client.send(rid, "error", code="busy", text="busy")
                return
            # 预取 transcript (持锁拷贝, 按连接绑定), turn 线程不触碰共享 dict
            with self._state_lock:
                text = self._transcripts.get((id(client), rid))
            ai_mode = str(msg.get("ai_mode") or "")
            if not self._turn_worker.submit(self._run_turn_job,
                                            client, rid, text, ai_mode):
                # R4 (P2): worker 不可用 → 回滚 _active/cancel 状态,
                # 不得留下悬挂的 active turn
                self._finish_turn(rid)
                client.send(rid, "error", code="internal",
                            text="turn worker unavailable")
        elif mtype == "transcript":
            if not self._set_transcript(client, rid, str(msg.get("text", ""))):
                client.send(rid, "error", code="internal",
                            text="transcript rejected (limit)")
        elif mtype == "stop_recording":
            # 提前结束录音: 只触发 audio_stop (不取消整轮)
            if not self._signal(rid, "stop"):
                client.send(rid, "error", code="unknown_request",
                            text=f"no active turn {rid}")
        elif mtype == "tool_result":
            cid = str(msg.get("tool_call_id", ""))
            with self._tool_cond:
                self._tool_results[(id(client), rid, cid)] = {
                    "ok": bool(msg.get("ok", False)),
                    "code": str(msg.get("code", "")),
                    "result": msg.get("result") or {},
                }
                self._tool_cond.notify_all()
        elif mtype == "cancel":
            # R4-R1 (D6): cancel 不立即回终态 — 终态 cancel 只在 turn
            # 真正停止 (不再产生 content/TTS/tool) 后由 _run_turn_job 发出;
            # 新 turn 只能等旧 turn 退出 (单 worker + _finish_turn 清 active)
            if not self._signal(rid, "cancel"):
                client.send(rid, "error", code="unknown_request",
                            text=f"no active turn {rid}")

    def _run_turn_job(self, client: ClientConn, rid: str, transcript_text,
                      ai_mode: str = ""):
        # R8_R3_R3_R2: 事件聚合器 — terminal 字段从本轮实际发送事件聚合
        tracker = {"content_count": 0, "content_bytes": 0,
                   "content_sha": hashlib.sha256(), "expression": "",
                   "state": "", "done_seen": "no", "tts_started": 0,
                   "tts_level": 0, "tts_finished": 0, "tts_played": "no",
                   "result": "error", "error_code": "",
                   "provider_asr": "", "asr_fallback": "no"}
        ran = False
        runner = None

        def s(r, t, **f):
            ok = client.send(r, t, **f)
            if r != rid:
                return ok
            if t == "content":
                txt = str(f.get("text") or "")
                tracker["content_count"] += 1
                tracker["content_bytes"] += len(txt.encode("utf-8"))
                tracker["content_sha"].update(txt.encode("utf-8"))
            elif t == "expression":
                tracker["expression"] = str(f.get("value") or "")
            elif t == "state":
                tracker["state"] = str(f.get("value") or "")
            elif t == "tts_started":
                tracker["tts_started"] += 1
            elif t == "tts_level":
                tracker["tts_level"] += 1
            elif t == "tts_finished":
                tracker["tts_finished"] += 1
                tracker["tts_played"] = "yes"
            elif t == "done":
                tracker["done_seen"] = "yes"
                tracker["result"] = "success"
            elif t == "error":
                tracker["result"] = "error"
                tracker["error_code"] = str(f.get("code") or "")
            elif t == "cancel":
                tracker["result"] = "cancelled"
            return ok

        try:
            runner = self._runner_factory(self._config)
            # R5_R4: 注入进程级会话态 (确认门跨轮存活; stub 工厂无该属性则跳过)
            if hasattr(runner, "_gate"):
                runner._gate = _SHARED_GATE
                runner._audit = _SHARED_AUDIT
            # R5_R4: 工具结果共享槽 (服务器 dict/cond) + 客户端键 —
            # 修复 (client,rid,cid) 与 (rid,cid) 键位错配导致结果从未回填
            if hasattr(runner, "_tool_results"):
                runner._tool_results = self._tool_results
                runner._tool_cond = self._tool_cond
                runner._client_key = id(client)
            with self._state_lock:
                state = self._cancel_flags.get(rid)
            if state is None:               # 已被 disconnect 清理
                return
            ran = True
            runner._audio_stop = state["stop"]
            # R8_R4: 工具回执通道注入 (键含连接 id, 与本轮 rid 绑定)
            with self._tool_cond:
                runner._tool_results = {
                    (r2, cid): doc
                    for (cc, r2, cid), doc in self._tool_results.items()
                    if cc == id(client)}
            runner._tool_cond = self._tool_cond
            if transcript_text is not None:
                runner.set_transcript(rid, transcript_text)
            runner.set_ai_mode(ai_mode)
            runner.run(rid, state["cancel"], s)
        except Exception:                   # noqa: BLE001 — 崩溃恢复
            LOG.exception("turn failed (request_id=%s)", rid)
            if runner is not None:
                runner.abort_audio()
            if not client.dead:
                s(rid, "error", code="turn_failed", text="turn failed")
        finally:
            # R4-R1 (D6): 终态 cancel 在 turn 真正停止后发送 (cancel 已请求时)
            with self._state_lock:
                state = self._cancel_flags.get(rid)
            if state is not None and state["cancel"].is_set() \
                    and tracker["result"] != "cancelled":
                s(rid, "cancel")
            if ran and runner is not None:
                m = runner._turn_metrics or {}
                tracker["provider_asr"] = str(m.get("ASR_PROVIDER_USED") or "")
                tracker["asr_fallback"] = str(m.get("ASR_FALLBACK_USED") or "no")
                runner._write_voice_terminal(rid, tracker)
            self._finish_turn(rid)


def load_config(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main(argv=None):
    # R8 (现场 UDS 生命周期): systemd stop / 脚本 kill -TERM 默认直接终止进程,
    # finally 里的 shutdown 不执行, UDS socket 文件残留且非优雅退出。
    # 注册 SIGTERM 与 SIGINT 同路 (抛 KeyboardInterrupt -> finally shutdown)。
    import signal as _signal

    def _on_sigterm(signum, frame):
        raise KeyboardInterrupt

    _signal.signal(_signal.SIGTERM, _on_sigterm)

    parser = argparse.ArgumentParser(description="HoloPet agentd (AI/Voice/Agent service)")
    parser.add_argument("--config", default="config/agent.example.json")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47650)
    parser.add_argument("--uds", default="",
                        help="Unix Domain Socket 路径 (Linux 生产; 优先于 TCP)")
    parser.add_argument("--fake", action="store_true")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args(argv)

    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    config = load_config(args.config)
    if args.fake:
        config["provider"] = "fake"
    server = WorkerServer(config, host=args.host, port=args.port,
                          uds_path=args.uds)
    # R4-R1 (D7): KeyboardInterrupt/异常/正常退出统一走 shutdown (有界)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.shutdown(timeout=5.0)


if __name__ == "__main__":
    main()
