"""prompt_loader.py — 分文件 Prompt 组合加载器 (R8_R2 工作包 A)

职责:
  - 从配置指定的 prompt_dir 按"文件名前缀升序"读取固定清单的 .md 文件,
    组合为恰好一条 system message 文本 (位于用户消息之前)。
  - 稳定错误: 缺目录 / 缺必需文件 / 读取失败 / 非 UTF-8 / 空组合 / 超限。
  - 日志只记录文件名、总字节数、成功与否; 不打印完整 system prompt。

配置:
  prompt_dir: 相对 agent 包目录或绝对路径; 默认 "prompts"。
  prompt_max_bytes: 组合上限 (默认 65536 = 64 KiB)。

用法 (TurnRunner):
  composer = PromptComposer.from_config(config)
  system_text = composer.compose()   # 抛 PromptError (稳定 code)
"""

import logging
import os

LOG = logging.getLogger("holopet_agentd")

# 固定清单 (前缀升序即组合顺序; 不依赖文件系统返回顺序)
REQUIRED_PROMPT_FILES = (
    "00_identity.md",
    "10_personality.md",
    "20_behavior.md",
    "30_tools.md",
    "40_emotion.md",
    "50_memory_boundary.md",
    "60_safety.md",
)

DEFAULT_MAX_BYTES = 64 * 1024


class PromptError(RuntimeError):
    """稳定分类的提示词加载错误。code 供上层错误映射使用。"""

    def __init__(self, code: str, detail: str = ""):
        super().__init__(code if not detail else f"{code}: {detail}")
        self.code = code
        self.detail = detail


def _default_prompt_dir() -> str:
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompts")


class PromptComposer:
    def __init__(self, prompt_dir: str = "", max_bytes: int = DEFAULT_MAX_BYTES):
        if not prompt_dir:
            prompt_dir = _default_prompt_dir()
        elif not os.path.isabs(prompt_dir):
            # 相对路径按 agent 包目录解析 (配置可写 "prompts" 或自定义子目录)
            prompt_dir = os.path.join(
                os.path.dirname(os.path.abspath(__file__)), prompt_dir)
        self.prompt_dir = os.path.normpath(prompt_dir)
        self.max_bytes = int(max_bytes)

    @classmethod
    def from_config(cls, config: dict) -> "PromptComposer":
        cfg = config or {}
        return cls(
            prompt_dir=str(cfg.get("prompt_dir") or ""),
            max_bytes=int(cfg.get("prompt_max_bytes", DEFAULT_MAX_BYTES)),
        )

    def compose(self) -> str:
        """按固定顺序组合; 失败抛 PromptError(code)。"""
        if not os.path.isdir(self.prompt_dir):
            raise PromptError("prompt_dir_missing", self.prompt_dir)
        parts = []
        total = 0
        names = []
        for name in REQUIRED_PROMPT_FILES:
            path = os.path.join(self.prompt_dir, name)
            if not os.path.isfile(path):
                raise PromptError("prompt_file_missing", name)
            try:
                with open(path, "rb") as f:
                    raw = f.read()
            except OSError as e:                       # 读取失败
                raise PromptError("prompt_read_failed",
                                  f"{name}: {type(e).__name__}") from e
            try:
                text = raw.decode("utf-8")             # 严格 UTF-8
            except UnicodeDecodeError as e:
                raise PromptError("prompt_not_utf8", name) from e
            if text.startswith("\ufeff"):              # 兼容 BOM
                text = text.lstrip("\ufeff")
            total += len(raw)
            if total > self.max_bytes:
                raise PromptError("prompt_too_large",
                                  f"{total} > {self.max_bytes}")
            parts.append(text.strip())
            names.append(name)
        combined = "\n\n".join(p for p in parts if p).strip()
        if not combined:
            raise PromptError("prompt_empty", "all files empty")
        # 日志: 只记录文件名列表/总字节/成功标记; 不打印内容
        LOG.info("prompt composed: files=%d bytes=%d ok",
                 len(names), len(combined.encode("utf-8")))
        return combined
