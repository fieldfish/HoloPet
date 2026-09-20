"""providers/volcengine_streaming.py — 火山引擎豆包语音识别大模型 流式 WebSocket ASR (R8_R3_R3)。

协议依据: 火山官方文档 (docs.volcengine.com/docs/6561/1354869,
1395846)。端点: wss://openspeech.bytedance.com/api/v3/sauc/bigmodel
(可选 bigmodel_async 由配置指定; 未开通时按官方返回错误处理, 不猜其他域名)。

握手 HTTP 头 (值只来自环境变量, 绝不落盘/日志):
  X-Api-App-Key     ← HOLOPET_VOLC_ASR_APP_KEY
  X-Api-Access-Key  ← HOLOPET_VOLC_ASR_ACCESS_KEY
  X-Api-Resource-Id ← HOLOPET_VOLC_ASR_RESOURCE_ID
  X-Api-Request-Id  ← 本轮生成的 UUID
  X-Api-Connect-Id  ← 同 UUID

帧协议:
  1) 文本帧: 完整客户端请求 (user/audio/request);
  2) 二进制帧: 16k/16bit/单声道 PCM 原始帧, 目标 ~200ms/包;
  3) 文本帧: 结束包 (last_package=true);
  4) 服务端文本帧: {"result":[{"text": ...}]} — 非 final 为 partial,
     final 由 sequence<0 / is_final / 结束包后的关闭判定;
  5) 服务端关闭 = 最终结果已给出。

依赖: websockets==14.1 (见 agent/requirements.txt; BSD-3-Clause)。
"""
import asyncio
import gzip
import json
import logging
import os
import struct
import time
import uuid
from dataclasses import dataclass, field
from typing import Optional

from .stt import AudioFormat, SttError, TranscriptResult

LOG = logging.getLogger("holopet_agentd")

ENDPOINT = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel"

# R8_R3_R3_R2: v3 sauc 二进制帧头 (与官方 demo 一致):
#   byte0 = 版本1<<4 | 头长4B        byte1 = 消息类型<<4 | flags
#   byte2 = 序列化<<4 | 压缩         byte3 = 保留
# 消息类型: 0b0001 全量请求 / 0b0010 音频 / 0b1001 服务端结果 / 0b1111 错误;
# flags: 0b0000 普通 / 0b0001 带序号 / 0b0010 末包;
# 序列化: 0b0001 JSON / 0b0000 原始; 压缩: 0b0001 gzip。
HDR_FULL_CLIENT = bytes([0x11, 0x10, 0x11, 0x00])
HDR_AUDIO = bytes([0x11, 0x21, 0x01, 0x00])       # 音频帧: flags 0b0001 带序号
HDR_AUDIO_LAST = bytes([0x11, 0x23, 0x01, 0x00])  # 末包: flags 0b0011 末+负序号
MSG_ERROR = 0x0F
MSG_FULL_SERVER = 0b1001


def _frame(header: bytes, payload: bytes, seq: Optional[int] = None) -> bytes:
    """header [+ 4B 大端序号(带序帧)] + 4B 大端长度 + gzip(压缩后) 负载。"""
    body = gzip.compress(payload)
    mid = struct.pack(">i", seq) if seq is not None else b""
    return header + mid + struct.pack(">I", len(body)) + body

ERR_MISSING_CREDS = "missing_credentials"
ERR_AUTH = "auth_error"
ERR_RATE_LIMITED = "rate_limited"
ERR_TIMEOUT = "timeout"
ERR_NETWORK = "network_error"
ERR_PROTOCOL = "protocol_error"
ERR_EMPTY = "empty_transcript"

# R8_R3_R3_R2 (必修 A §3.2): 稳定细分类 — 本地参数错误不得再记网络故障。
ERR_VOLC_CONFIG_MISSING = "volc_config_missing"
ERR_VOLC_CLIENT_INCOMPAT = "volc_client_incompatible"
ERR_VOLC_CAPABILITY = "volc_capability_not_enabled"
ERR_VOLC_AUTH_REJECTED = "volc_auth_rejected"
ERR_VOLC_HANDSHAKE_REJECTED = "volc_handshake_rejected"
ERR_VOLC_NETWORK = "volc_network_unreachable"
ERR_VOLC_STREAM_TIMEOUT = "volc_stream_timeout"
ERR_VOLC_PROTOCOL = "volc_protocol_error"

# 允许降级的稳定错误集
FALLBACKABLE = {
    ERR_MISSING_CREDS, ERR_AUTH, ERR_RATE_LIMITED, ERR_TIMEOUT,
    ERR_NETWORK, ERR_PROTOCOL, ERR_EMPTY,
    ERR_VOLC_CONFIG_MISSING, ERR_VOLC_CLIENT_INCOMPAT, ERR_VOLC_CAPABILITY,
    ERR_VOLC_AUTH_REJECTED, ERR_VOLC_HANDSHAKE_REJECTED, ERR_VOLC_NETWORK,
    ERR_VOLC_STREAM_TIMEOUT, ERR_VOLC_PROTOCOL,
}


def _websocket_header_kwarg(websockets_module):
    """按函数签名选择鉴权头参数名: 14.1 正式路径 = additional_headers。

    旧参数名 (extra_headers) 只存在于 websockets<14; 14.1+ 的 connect 不再
    接收该名 (会一路透传到 create_connection 抛 TypeError)。返回 None 表示
    客户端与 Provider 不兼容 (caller 记 volc_client_incompatible)。
    """
    import inspect
    try:
        params = inspect.signature(websockets_module.connect).parameters
    except (TypeError, ValueError):
        return None
    if "additional_headers" in params:
        return "additional_headers"
    if "extra_headers" in params:
        return "extra_headers"
    return None


def _endpoint_host(endpoint):
    host = ""
    try:
        u = endpoint.split("://", 1)[1]
        host = u.split("/", 1)[0].split(":", 1)[0]
    except IndexError:
        host = ""
    return host


@dataclass
class StreamingStats:
    provider: str = "volcengine_streaming"
    first_partial_ms: Optional[int] = None     # 采集开始 → 首个 partial
    final_ms: Optional[int] = None             # 采集开始 → final
    bytes_sent: int = 0
    chunks_sent: int = 0
    partials: int = 0
    error_code: str = ""
    http_status: int = 0                       # 握手拒绝时的脱敏状态码 (0=无)

    def as_dict(self) -> dict:
        return {
            "provider": self.provider,
            "first_partial_ms": self.first_partial_ms,
            "final_ms": self.final_ms,
            "bytes_sent": self.bytes_sent,
            "chunks_sent": self.chunks_sent,
            "partials": self.partials,
            "error_code": self.error_code,
        }


class VolcengineStreamingSttProvider:
    """流式 ASR: 边录边发。同一轮只能用一个实例; 取消必须调用 cancel()。"""

    def __init__(self, config: dict, loop_factory=None):
        self._config = config or {}
        self._endpoint = (self._config.get("endpoint") or
                          self._config.get("base_url") or ENDPOINT)
        self._app_key_env = (self._config.get("app_key_env_name")
                             or "HOLOPET_VOLC_ASR_APP_KEY")
        self._access_key_env = (self._config.get("access_key_env_name")
                                or "HOLOPET_VOLC_ASR_ACCESS_KEY")
        self._resource_id_env = (self._config.get("resource_id_env_name")
                                 or "HOLOPET_VOLC_ASR_RESOURCE_ID")
        self._chunk_ms = int(self._config.get("chunk_ms", 200))
        self._timeout = int(self._config.get("request_timeout_ms", 60000)) / 1000.0
        self._language = self._config.get("language") or "zh"
        self._loop_factory = loop_factory

    # ---- 凭据存在性 (不取用值) ----
    def credentials_present(self) -> dict:
        return {
            "app_key": bool(os.environ.get(self._app_key_env, "")),
            "access_key": bool(os.environ.get(self._access_key_env, "")),
            "resource_id": bool(os.environ.get(self._resource_id_env, "")),
        }

    # ---- 流式会话 (Provider 自身代理单会话, 供 TurnRunner 直用) ----
    def __init_extra(self):
        pass

    def _session(self) -> "VolcStreamSession":
        if getattr(self, "_sess", None) is None:
            self._sess = VolcStreamSession(self)
        return self._sess

    def open_session(self) -> "VolcStreamSession":
        return self._session()

    def start(self):
        return self._session().start()

    def feed(self, pcm: bytes) -> None:
        self._session().feed(pcm)

    def finish(self, timeout_ms: int = 30000):
        return self._session().finish(timeout_ms)

    def stats(self):
        return self._session().stats

    def cancel(self) -> None:
        self._session().cancel()

    def close(self) -> None:
        if getattr(self, "_sess", None) is not None:
            self._sess.close()
            self._sess = None

    def last_error(self) -> str:
        s = getattr(self, "_sess", None)
        return s._error if s is not None else "" 


class VolcStreamSession:
    """单轮流式会话 (同步封装 asyncio)。"""

    def __init__(self, owner: VolcengineStreamingSttProvider):
        self._o = owner
        self._loop = None
        self._ws = None
        self._text_parts: list = []
        self._last_partial = ""
        self._final_text = ""
        self._finalized = False
        self._cancelled = False
        self._error = None
        self.stats = StreamingStats()
        self._t0 = None
        self._header_kwarg = None
        self._ws_version = ""
        self._http_status = 0
        self._seq = 1            # 音频帧序号: 全量请求占 1, 音频从 2 递增

    # ---- 生命周期 ----
    def start(self) -> Optional[str]:
        """建立连接并发送首包; 返回稳定错误码或 None。

        R8_R3_R3_R2: 本地客户端不兼容 (依赖缺失/签名不匹配/参数 TypeError)
        必须记 volc_client_incompatible, 不得归入网络故障。
        """
        creds = self._o.credentials_present()
        if not all(creds.values()):
            self._error = ERR_VOLC_CONFIG_MISSING
            return self._error
        try:
            import websockets
        except ImportError:
            self._error = ERR_VOLC_CLIENT_INCOMPAT
            return self._error
        self._ws_version = getattr(websockets, "__version__", "unknown")
        self._header_kwarg = _websocket_header_kwarg(websockets)
        if self._header_kwarg is None:
            self._error = ERR_VOLC_CLIENT_INCOMPAT
            LOG.warning(
                "volc preflight: websockets_version=%s header_parameter=none "
                "endpoint_host=%s resource_id_present=%s credentials_present=yes "
                "-> %s",
                self._ws_version, _endpoint_host(self._o._endpoint),
                creds["resource_id"], ERR_VOLC_CLIENT_INCOMPAT)
            return self._error
        # §3.1.4: 启动前记录非秘密字段 (绝不含任何凭据值/header/URL 查询)
        LOG.info(
            "volc preflight: websockets_version=%s header_parameter=%s "
            "endpoint_host=%s resource_id_present=%s credentials_present=yes",
            self._ws_version, self._header_kwarg,
            _endpoint_host(self._o._endpoint), creds["resource_id"])
        self._loop = asyncio.new_event_loop()
        try:
            self._loop.run_until_complete(self._connect())
        except Exception:
            # _connect 内部已按类别设置 self._error (细分类)
            self._error = self._error or ERR_VOLC_NETWORK
            self.close()
        return self._error

    def feed(self, pcm: bytes) -> None:
        """发送一段 PCM (原始帧)。"""
        if self._ws is None or self._error or self._cancelled:
            return
        if not pcm:
            return
        self.stats.bytes_sent += len(pcm)
        self.stats.chunks_sent += 1
        try:
            self._loop.run_until_complete(self._send_binary(pcm))
        except Exception:
            self._error = self._error or ERR_VOLC_NETWORK

    def finish(self, timeout_ms: int = 30000) -> TranscriptResult:
        """发送结束包并等待 final transcript。"""
        if self._error:
            return TranscriptResult(error=SttError(self._error, self._error))
        try:
            self._loop.run_until_complete(self._finish(timeout_ms))
        except Exception:
            self._error = self._error or ERR_VOLC_STREAM_TIMEOUT
        if self._error:
            return TranscriptResult(error=SttError(self._error, self._error))
        if not self._final_text.strip():
            return TranscriptResult(error=SttError(ERR_EMPTY, "no final text"))
        return TranscriptResult(text=self._final_text, confidence=1.0)

    def cancel(self) -> None:
        self._cancelled = True
        self.close()

    def close(self) -> None:
        if self._loop is not None:
            try:
                self._loop.run_until_complete(self._close_ws())
            except Exception:
                pass
            try:
                self._loop.close()
            except Exception:
                pass
            self._loop = None
        self._ws = None

    # ---- asyncio 内部 ----
    async def _connect(self):
        import websockets
        rid = uuid.uuid4().hex
        headers = {
            "X-Api-App-Key": os.environ[self._o._app_key_env],
            "X-Api-Access-Key": os.environ[self._o._access_key_env],
            "X-Api-Resource-Id": os.environ[self._o._resource_id_env],
            "X-Api-Request-Id": rid,
            "X-Api-Connect-Id": rid,
        }
        # R8_R3_R3_R2: 14.1 正式路径必须用 additional_headers; 旧参数名绝不进入。
        connect_kwargs = {"open_timeout": 15, "close_timeout": 5}
        connect_kwargs[self._header_kwarg] = headers
        try:
            self._ws = await websockets.connect(
                self._o._endpoint, **connect_kwargs)
        except TypeError:
            # 本地签名不兼容: 不得归类为网络故障
            self._error = ERR_VOLC_CLIENT_INCOMPAT
            raise
        except asyncio.TimeoutError:
            self._error = ERR_VOLC_NETWORK
            raise
        except OSError:
            self._error = ERR_VOLC_NETWORK
            raise
        except Exception as exc:
            invalid_status = getattr(websockets, "exceptions", None)
            status = 0
            if invalid_status is not None:
                is_cls = getattr(invalid_status, "InvalidStatus", None)
                if is_cls is not None and isinstance(exc, is_cls):
                    resp = getattr(exc, "response", None)
                    status = int(getattr(resp, "status_code", 0) or 0)
            if status:
                self._http_status = status
                self.stats.http_status = status
                if status == 403:
                    self._error = ERR_VOLC_CAPABILITY
                elif status == 401:
                    self._error = ERR_VOLC_AUTH_REJECTED
                else:
                    self._error = ERR_VOLC_HANDSHAKE_REJECTED
                LOG.warning("volc handshake rejected: http_status=%d -> %s",
                            status, self._error)
            raise
        req = {
            "user": {"uid": "holopet"},
            "audio": {"format": "pcm", "rate": 16000, "bits": 16,
                      "channel": 1, "codec": "raw"},
            "request": {"model_name": "bigmodel", "enable_itn": True,
                        "enable_punc": True, "result_type": "full",
                        "language": self._o._language},
        }
        await self._ws.send(_frame(HDR_FULL_CLIENT,
                                   json.dumps(req, ensure_ascii=False)
                                   .encode("utf-8")))
        self._t0 = time.monotonic()

    async def _send_binary(self, pcm: bytes):
        self._seq += 1
        await self._ws.send(_frame(HDR_AUDIO, bytes(pcm), seq=self._seq))
        # 非阻塞地收一轮 partial (不等待); _drain 是普通协程, 必须 await
        # (R8_R3_R3_R2: 对协程用 async for 会得到未 await 的对象 →
        # 真机首包后即 volc_protocol_error)。
        try:
            await self._drain()
        except Exception:
            self._error = self._error or ERR_VOLC_PROTOCOL

    async def _drain(self):
        """读取已到达的消息直到没有更多 (有界)。"""
        import websockets
        try:
            while True:
                msg = await asyncio.wait_for(self._ws.recv(), timeout=0.05)
                handled = self._handle(msg)
                if handled is False:
                    break
        except asyncio.TimeoutError:
            pass

    async def _finish(self, timeout_ms: int):
        # R8_R3_R3_R2: 末包 = 音频帧 + flags 0b0011, 序号取负 (官方 demo 语义:
        # 服务端要求全量请求后音频序号从 2 递增, 末包负序号结束)。
        self._seq += 1
        await self._ws.send(_frame(HDR_AUDIO_LAST, b"", seq=-self._seq))
        deadline = time.monotonic() + timeout_ms / 1000.0
        import websockets
        while time.monotonic() < deadline and not self._finalized:
            try:
                msg = await asyncio.wait_for(self._ws.recv(), timeout=0.5)
            except asyncio.TimeoutError:
                continue
            if self._handle(msg) is False:
                break
        if not self._finalized and self._final_text.strip():
            self._finalized = True
        if not self._finalized:
            self._error = self._error or ERR_VOLC_STREAM_TIMEOUT
        await self._close_ws()

    def _handle(self, msg) -> object:
        """处理一条服务端**二进制**帧 (R8_R3_R3_R2 v3 协议); 返回 False 结束。"""
        if isinstance(msg, str):
            # 保底: 文本帧不应出现, 按协议错误处理
            self._error = ERR_VOLC_PROTOCOL
            return False
        try:
            b = bytes(msg)
            mtype = b[1] >> 4
            flags = b[1] & 0x0F
            comp = b[2] & 0x0F
            off = 4
            if mtype == MSG_ERROR:
                code = struct.unpack(">I", b[off:off + 4])[0]
                off += 4
                size = struct.unpack(">I", b[off:off + 4])[0]
                off += 4
                payload = b[off:off + size]
                if comp & 0x01:
                    payload = gzip.decompress(payload)
                detail = payload.decode("utf-8", "replace")[:120]
                LOG.warning("volc server error frame: code=%d detail=%s",
                            code, detail)
                self._error = ERR_VOLC_PROTOCOL
                return False
            seq = 0
            if flags & 0x01:
                seq = struct.unpack(">i", b[off:off + 4])[0]
                off += 4
            size = struct.unpack(">I", b[off:off + 4])[0]
            off += 4
            payload = b[off:off + size]
            if comp & 0x01:
                payload = gzip.decompress(payload)
            doc = json.loads(payload.decode("utf-8"))
        except Exception:
            self._error = ERR_VOLC_PROTOCOL
            return False
        text = ""
        if isinstance(doc, dict):
            res = doc.get("result")
            # R8_R3_R3_R2: 文本在 result.text (嵌套) — 顶层无 text 键;
            # 上一版只认 result 为列表/顶层 text, 真实文本全被漏掉 (partials=0)。
            if isinstance(res, dict) and isinstance(res.get("text"), str):
                text = res["text"]
            elif isinstance(res, list) and res and isinstance(res[0], dict):
                text = str(res[0].get("text") or "")
            elif isinstance(doc.get("text"), str):
                text = doc["text"]
        if not seq and isinstance(doc, dict):
            seq = doc.get("sequence", 0) or 0
        if text and not self._text_parts:
            if self.stats.first_partial_ms is None:
                self.stats.first_partial_ms = int(
                    (time.monotonic() - self._t0) * 1000) if self._t0 else None
        if text:
            self.stats.partials += 1
            if text not in self._text_parts:
                self._text_parts.append(text)
            # R8_R4_R3_R4_R5_R3 (v8): VOLC 流式 partial 是同一句的渐进细化
            # (全文覆盖式, 非增量) — 拼接全部 distinct partial 会产生
            # "启动启动三启动3分钟…" 式乱码 (Pi 19:02 实测转录), 模型据此
            # 只能猜时长 (实测建出 60s/90s 而非 3 分钟)。final 取最新 partial。
            self._last_partial = text
        # 结束判定: sequence<0 或服务端帧 flags 带 0b0010 (官方 demo 语义)
        if (isinstance(seq, int) and seq < 0) or (flags & 0b0010):
            self._final_text = self._last_partial or text
            self._finalized = True
            if self.stats.final_ms is None and self._t0:
                self.stats.final_ms = int((time.monotonic() - self._t0) * 1000)
            return False
        return True

    async def _close_ws(self):
        if self._ws is not None:
            try:
                await self._ws.close()
            except Exception:
                pass
            self._ws = None
