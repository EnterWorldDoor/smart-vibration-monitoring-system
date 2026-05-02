import numpy as np
import time
from typing import Dict, Optional
from collections import deque

from utils.logger import get_logger
from utils.metrics import compute_psi

logger = get_logger(__name__)


class DriftDetector:
    """
    模型漂移检测器。

    监测模型推理过程中的数据漂移和模型性能退化:
    - PSI (Population Stability Index): 特征分布偏移检测
    - 预测分布变化检测
    - 置信度趋势监测
    - 自动漂移告警

    PSI 判定标准:
        PSI < 0.1:  无漂移
        0.1 ~ 0.25: 轻微漂移，建议关注
        PSI > 0.25: 显著漂移，建议模型再训练
    """

    def __init__(
        self,
        reference_window_size: int = 500,
        feature_count: int = 24,
        drift_threshold_warn: float = 0.1,
        drift_threshold_alert: float = 0.25
    ):
        self.reference_window_size = reference_window_size
        self.feature_count = feature_count
        self.drift_threshold_warn = drift_threshold_warn
        self.drift_threshold_alert = drift_threshold_alert

        self._reference_data: Optional[np.ndarray] = None
        self._current_data: deque = deque(maxlen=reference_window_size)
        self._drift_history: deque = deque(maxlen=100)
        self._last_check_time: float = time.time()
        self._check_interval_seconds: float = 60.0

    def set_reference(self, data: np.ndarray):
        """
        设置参考分布（基线）。

        Args:
            data: 参考数据 (N, features)
        """
        self._reference_data = data.reshape(-1)
        logger.info("Reference distribution set: %d samples", data.shape[0])

    def update(self, features: np.ndarray):
        """
        更新当前数据窗口。

        Args:
            features: 特征向量或 (1, features) 数组
        """
        if features.ndim > 1:
            features = features.flatten()
        self._current_data.extend(features.tolist())

    def check_drift(self) -> Dict:
        """
        执行漂移检测。

        Returns:
            漂移检测结果字典
        """
        if self._reference_data is None:
            return {'status': 'no_reference', 'psi': 0.0, 'drift_detected': False}

        if len(self._current_data) < 100:
            return {'status': 'insufficient_data', 'psi': 0.0, 'drift_detected': False}

        psi = compute_psi(self._reference_data, np.array(self._current_data))

        self._drift_history.append({
            'timestamp': time.time(),
            'psi': psi
        })

        if psi >= self.drift_threshold_alert:
            status = 'alert'
            logger.warning("Drift ALERT: PSI=%.4f (threshold=%.2f)", psi, self.drift_threshold_alert)
        elif psi >= self.drift_threshold_warn:
            status = 'warning'
            logger.info("Drift WARNING: PSI=%.4f (threshold=%.2f)", psi, self.drift_threshold_warn)
        else:
            status = 'stable'

        return {
            'status': status,
            'psi': round(psi, 6),
            'drift_detected': psi >= self.drift_threshold_warn,
            'threshold_warn': self.drift_threshold_warn,
            'threshold_alert': self.drift_threshold_alert,
            'reference_samples': len(self._reference_data),
            'current_samples': len(self._current_data),
            'history_mean_psi': round(
                np.mean([h['psi'] for h in self._drift_history]), 6
            )
        }

    def get_history(self) -> list:
        """获取漂移历史"""
        return list(self._drift_history)

    def reset(self):
        """重置当前数据窗口"""
        self._current_data.clear()


class AlertManager:
    """
    告警管理器。

    集中管理Edge-AI系统的所有告警:
    - 推理置信度过低告警
    - 兜底模型频繁触发告警
    - 数据漂移告警
    - 系统资源告警

    告警级别:
        INFO:    信息通知
        WARNING: 需要关注但不紧急
        CRITICAL: 需要立即处理
    """

    ALERT_COOLDOWN_SECONDS = {
        'info': 60,
        'warning': 300,
        'critical': 60
    }

    def __init__(self):
        self.alerts: deque = deque(maxlen=200)
        self._last_alert_time: Dict[str, float] = {}
        self._alert_counts: Dict[str, int] = {}

    def raise_alert(
        self,
        alert_type: str,
        level: str,
        message: str,
        metadata: Optional[Dict] = None
    ):
        """
        发出告警。

        Args:
            alert_type: 告警类型标识
            level: 告警级别 info/warning/critical
            message: 告警消息
            metadata: 附加元数据
        """
        now = time.time()

        cooldown = self.ALERT_COOLDOWN_SECONDS.get(level, 60)
        last_time = self._last_alert_time.get(alert_type, 0)
        if now - last_time < cooldown:
            return

        self._last_alert_time[alert_type] = now
        self._alert_counts[alert_type] = self._alert_counts.get(alert_type, 0) + 1

        alert = {
            'timestamp': now,
            'type': alert_type,
            'level': level,
            'message': message,
            'count': self._alert_counts[alert_type],
            'metadata': metadata or {}
        }

        self.alerts.append(alert)

        log_levels = {
            'info': logger.info,
            'warning': logger.warning,
            'critical': logger.error
        }
        log_func = log_levels.get(level, logger.warning)
        log_func("[%s] %s: %s", level.upper(), alert_type, message)

    def get_recent_alerts(self, n: int = 20) -> list:
        """获取最近N条告警"""
        return list(self.alerts)[-n:]

    def get_alert_counts(self) -> Dict[str, int]:
        """获取各类型告警计数"""
        return self._alert_counts.copy()

    def clear(self):
        """清除所有告警"""
        self.alerts.clear()
        self._last_alert_time.clear()
        self._alert_counts.clear()
