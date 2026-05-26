"""llama-cpp-python wrapper for local LLM inference."""

import structlog
from pathlib import Path
from typing import Optional

logger = structlog.get_logger(__name__)


class LLMEngine:
    """Thin wrapper around llama-cpp-python Llama.

    All generate() calls are synchronous — callers must use run_in_executor
    to avoid blocking the asyncio event loop.
    """

    def __init__(self, model_path: str, n_ctx: int = 2048,
                 n_threads: int = 4, temperature: float = 0.7,
                 top_p: float = 0.9):
        self.model_path = model_path
        self.n_ctx = n_ctx
        self.n_threads = n_threads
        self.temperature = temperature
        self.top_p = top_p
        self._model: Optional[object] = None
        self._model_name = "qwen2.5-1.5b-instruct"
        self._model_version = "q4_k_m"

    @property
    def model_name(self) -> str:
        return self._model_name

    @property
    def model_version(self) -> str:
        return self._model_version

    @property
    def is_loaded(self) -> bool:
        return self._model is not None

    def load(self):
        """Load the GGUF model into memory. Blocks until complete."""
        if not Path(self.model_path).exists():
            raise FileNotFoundError(f"Model not found: {self.model_path}")

        from llama_cpp import Llama
        logger.info("loading model", path=self.model_path)
        self._model = Llama(
            model_path=self.model_path,
            n_ctx=self.n_ctx,
            n_threads=self.n_threads,
            verbose=False,
        )
        logger.info("model loaded", n_ctx=self.n_ctx, n_threads=self.n_threads)

    def chat(self, messages: list[dict], max_tokens: int = 512) -> dict:
        """Run chat completion with full message history.

        Args:
            messages: List of {"role": "...", "content": "..."} dicts.
            max_tokens: Max tokens to generate.

        Returns:
            dict with keys: text, tokens_used, generation_time_ms
        """
        if not self._model:
            raise RuntimeError("Model not loaded")

        import time
        t0 = time.perf_counter()
        result = self._model.create_chat_completion(
            messages=messages,
            max_tokens=max_tokens,
            temperature=self.temperature,
            top_p=self.top_p,
        )
        elapsed = (time.perf_counter() - t0) * 1000

        text = result["choices"][0]["message"]["content"]
        usage = result.get("usage", {})
        tokens_used = usage.get("total_tokens", 0)

        logger.debug("generation complete",
                     tokens=tokens_used, time_ms=round(elapsed, 0))
        return {
            "text": text.strip(),
            "tokens_used": tokens_used,
            "generation_time_ms": elapsed,
        }

    def unload(self):
        """Release model memory."""
        self._model = None
        logger.info("model unloaded")
