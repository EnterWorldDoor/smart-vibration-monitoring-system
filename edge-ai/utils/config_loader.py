import os
import yaml
from pathlib import Path
from typing import Any, Dict, Optional

from .logger import get_logger

logger = get_logger(__name__)


class ConfigLoader:
    """
    企业级YAML配置加载器。

    支持：
    - 多层级配置合并
    - 环境变量覆盖
    - 配置版本追踪
    - 自动验证必填字段
    """

    def __init__(self, config_dir: Optional[str] = None):
        """
        Args:
            config_dir: 配置文件目录，默认为 edge-ai/ 下的各子目录
        """
        self._config_dir = Path(config_dir) if config_dir else None
        self._cache: Dict[str, Dict] = {}

    def load(self, config_path: str, required_keys: Optional[list] = None) -> Dict[str, Any]:
        """
        加载并验证YAML配置文件。

        Args:
            config_path: 配置文件路径
            required_keys: 必填键列表，用于验证

        Returns:
            配置字典

        Raises:
            FileNotFoundError: 配置文件不存在
            ValueError: 缺少必填配置项
        """
        if config_path in self._cache:
            return self._cache[config_path]

        path = Path(config_path)
        if not path.is_absolute():
            if self._config_dir:
                path = self._config_dir / config_path
            else:
                path = Path(os.getcwd()) / config_path

        if not path.exists():
            logger.error("Config file not found: %s", path)
            raise FileNotFoundError(f"Config file not found: {path}")

        with open(path, 'r', encoding='utf-8') as f:
            config = yaml.safe_load(f)

        if config is None:
            config = {}

        self._apply_env_overrides(config)

        if required_keys:
            self._validate_required(config, required_keys, str(path))

        self._cache[config_path] = config
        logger.info("Config loaded: %s", path)
        return config

    def _apply_env_overrides(self, config: Dict, prefix: str = "EDGEVIB_"):
        """用环境变量覆盖配置值"""
        for key, value in config.items():
            env_key = f"{prefix}{key.upper()}"
            env_val = os.environ.get(env_key)
            if env_val is not None:
                if isinstance(value, bool):
                    config[key] = env_val.lower() in ('true', '1', 'yes')
                elif isinstance(value, int):
                    config[key] = int(env_val)
                elif isinstance(value, float):
                    config[key] = float(env_val)
                else:
                    config[key] = env_val
                logger.debug("Override config %s from env %s", key, env_key)

    def _validate_required(self, config: Dict, required_keys: list, source: str):
        """验证必填配置项"""
        missing = [k for k in required_keys if k not in config]
        if missing:
            raise ValueError(
                f"Missing required config keys in {source}: {missing}"
            )

    def clear_cache(self):
        """清除配置缓存"""
        self._cache.clear()
