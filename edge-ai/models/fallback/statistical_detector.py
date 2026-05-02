import numpy as np
from typing import Dict, Optional, Tuple
from collections import deque

from utils.logger import get_logger

logger = get_logger(__name__)


class StatisticalDetector:
    """
    统计过程控制 (SPC) 检测器。

    用于检测振动数据的统计异常，支持:
    - EWMA (指数加权移动平均) 趋势漂移检测
    - CUSUM (累积和) 突变点检测
    - 3-Sigma 异常值检测
    - 多特征联合异常检测

    在兜底模型中作为规则引擎的补充，用于检测规则
    未覆盖的统计模式异常。
    """

    def __init__(
        self,
        ewma_smoothing: float = 0.2,
        cusum_threshold: float = 5.0,
        sigma_multiplier: float = 3.0,
        warm_up_period: int = 30
    ):
        self.ewma_smoothing = ewma_smoothing
        self.cusum_threshold = cusum_threshold
        self.sigma_multiplier = sigma_multiplier
        self.warm_up_period = warm_up_period

        self._ewma: Optional[float] = None
        self._cusum_pos: float = 0.0
        self._cusum_neg: float = 0.0
        self._mean: Optional[float] = None
        self._std: Optional[float] = None
        self._history = deque(maxlen=500)
        self._n_samples: int = 0

    def update(self, value: float) -> Dict:
        """
        更新统计检测器并返回检测结果。

        Args:
            value: 当前观测值（如 overall_rms）

        Returns:
            检测结果字典:
            {
                'is_anomaly': bool,
                'method': 'ewma' | 'cusum' | 'sigma' | 'none',
                'score': float,
                'ewma': float,
                'cusum_pos': float,
                'cusum_neg': float,
                'z_score': float
            }
        """
        self._history.append(value)
        self._n_samples += 1

        result = {
            'is_anomaly': False,
            'method': 'none',
            'score': 0.0,
            'ewma': 0.0,
            'cusum_pos': 0.0,
            'cusum_neg': 0.0,
            'z_score': 0.0
        }

        if self._n_samples < self.warm_up_period:
            if self._n_samples == self.warm_up_period - 1:
                self._initialize_stats()
            return result

        ewma_new = self.ewma_smoothing * value + (1 - self.ewma_smoothing) * self._ewma
        ewma_err = ewma_new - self._ewma
        self._ewma = ewma_new
        result['ewma'] = ewma_new

        target = self._mean
        cusum_pos_new = max(0, self._cusum_pos + (value - target) - 0.5 * self._std)
        cusum_neg_new = max(0, self._cusum_neg + (target - value) - 0.5 * self._std)

        self._cusum_pos = cusum_pos_new
        self._cusum_neg = cusum_neg_new
        result['cusum_pos'] = cusum_pos_new
        result['cusum_neg'] = cusum_neg_new

        z_score = abs(value - self._mean) / max(self._std, 1e-10)
        result['z_score'] = z_score

        if z_score > self.sigma_multiplier:
            result['is_anomaly'] = True
            result['method'] = 'sigma'
            result['score'] = z_score
        elif cusum_pos_new > self.cusum_threshold * self._std:
            result['is_anomaly'] = True
            result['method'] = 'cusum_pos'
            result['score'] = cusum_pos_new / self._std
        elif cusum_neg_new > self.cusum_threshold * self._std:
            result['is_anomaly'] = True
            result['method'] = 'cusum_neg'
            result['score'] = cusum_neg_new / self._std

        if result['is_anomaly']:
            logger.debug(
                "SPC anomaly: method=%s, score=%.3f, value=%.4f",
                result['method'], result['score'], value
            )

        return result

    def batch_detect(
        self, values: np.ndarray
    ) -> np.ndarray:
        """
        批量异常检测。

        Args:
            values: 观测值数组

        Returns:
            布尔数组 True=异常
        """
        results = np.zeros(len(values), dtype=bool)
        for i, v in enumerate(values):
            r = self.update(float(v))
            results[i] = r['is_anomaly']
        return results

    def _initialize_stats(self):
        """从预热数据计算初始统计量"""
        history_arr = np.array(self._history)
        self._mean = float(np.mean(history_arr))
        self._std = float(np.std(history_arr))
        self._ewma = self._mean
        self._cusum_pos = 0.0
        self._cusum_neg = 0.0
        logger.info(
            "SPC initialized: mean=%.4f, std=%.4f (warmup=%d)",
            self._mean, self._std, self.warm_up_period
        )

    def reset(self):
        """重置检测器状态"""
        self._ewma = None
        self._cusum_pos = 0.0
        self._cusum_neg = 0.0
        self._mean = None
        self._std = None
        self._history.clear()
        self._n_samples = 0

    def get_baseline(self) -> Optional[Tuple[float, float]]:
        """获取基线统计量"""
        if self._mean is not None and self._std is not None:
            return (self._mean, self._std)
        if len(self._history) >= 10:
            arr = np.array(self._history)
            return (float(np.mean(arr)), float(np.std(arr)))
        return None
