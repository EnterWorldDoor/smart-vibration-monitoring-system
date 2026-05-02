import logging
import sys
from pathlib import Path
from typing import Optional

_loggers = {}


def get_logger(
    name: str,
    level: str = "INFO",
    log_file: Optional[str] = None
) -> logging.Logger:
    """
    获取或创建统一日志器实例。

    支持控制台输出和可选文件输出，自动避免重复添加handler。

    Args:
        name: 日志器名称，通常使用 __name__
        level: 日志级别，默认 INFO
        log_file: 可选的日志文件路径

    Returns:
        配置好的 Logger 实例

    Example:
        >>> logger = get_logger(__name__)
        >>> logger.info("Edge-AI module initialized")
    """
    if name in _loggers:
        return _loggers[name]

    logger = logging.getLogger(name)
    logger.setLevel(getattr(logging, level.upper(), logging.INFO))

    if not logger.handlers:
        console_handler = logging.StreamHandler(sys.stdout)
        console_handler.setLevel(getattr(logging, level.upper(), logging.INFO))
        formatter = logging.Formatter(
            '%(asctime)s | %(levelname)-8s | %(name)s | %(message)s',
            datefmt='%Y-%m-%d %H:%M:%S'
        )
        console_handler.setFormatter(formatter)
        logger.addHandler(console_handler)

        if log_file:
            log_path = Path(log_file)
            log_path.parent.mkdir(parents=True, exist_ok=True)
            file_handler = logging.FileHandler(log_file, encoding='utf-8')
            file_handler.setLevel(logging.DEBUG)
            file_handler.setFormatter(formatter)
            logger.addHandler(file_handler)

    _loggers[name] = logger
    return logger
