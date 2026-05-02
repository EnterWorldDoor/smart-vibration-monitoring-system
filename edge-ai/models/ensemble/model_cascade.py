import numpy as np
import time
from typing import Dict, Optional, Tuple
from dataclasses import dataclass

from models.primary.vibration_cnn import VibrationCNNLSTM
from models.fallback.rule_engine import RuleEngine
from utils.logger import get_logger

logger = get_logger(__name__)


@dataclass
class CascadeResult:
    """级联推理输出结构"""
    prediction: int
    predicted_class: str
    confidence: float
    model_used: str
    inference_time_ms: float
    fallback_triggered: bool
    fallback_reason: str
    diagnosis: Dict
    raw_outputs: Optional[Dict] = None


class ModelCascade:
    """
    企业级双模型级联控制器。

    核心逻辑:
        1. 输入特征 → 主模型推理
        2. 主模型置信度 >= 阈值 → 输出主模型结果
        3. 主模型置信度 < 阈值 → 降级到兜底规则引擎
        4. 融合两个模型的结果生成最终诊断

    设计原则:
        - 主模型不可用时自动完全降级
        - 每次推理记录模型选择原因
        - 支持运行时阈值动态调整
    """

    def __init__(
        self,
        primary_model: Optional[VibrationCNNLSTM] = None,
        fallback_engine: Optional[RuleEngine] = None,
        confidence_threshold: float = 0.85,
        class_names: Optional[list] = None
    ):
        self.primary = primary_model
        self.fallback = fallback_engine or RuleEngine()
        self.confidence_threshold = confidence_threshold
        self.class_names = class_names or [
            'normal', 'imbalance', 'misalignment', 'bearing_fault'
        ]
        self._stats = {
            'total_inferences': 0,
            'primary_used': 0,
            'fallback_used': 0,
            'fallback_by_confidence': 0,
            'fallback_by_error': 0,
            'total_inference_time_ms': 0.0
        }

    def infer(
        self,
        X: np.ndarray,
        features: Optional[Dict[str, float]] = None
    ) -> CascadeResult:
        """
        执行级联推理。

        Args:
            X: 输入数据 (window_size, num_features) 或 (1, window_size, num_features)
            features: 特征字典（用于规则引擎），None则从X提取

        Returns:
            CascadeResult 推理结果
        """
        start_time = time.perf_counter()

        if X.ndim == 2:
            X = np.expand_dims(X, axis=0)

        self._stats['total_inferences'] += 1

        if self.primary is not None:
            try:
                probas = self.primary.predict(X, return_confidence=True)
                confidence = float(np.max(probas, axis=1)[0])
                prediction = int(np.argmax(probas, axis=1)[0])

                if confidence >= self.confidence_threshold:
                    elapsed = (time.perf_counter() - start_time) * 1000
                    self._stats['primary_used'] += 1
                    self._stats['total_inference_time_ms'] += elapsed

                    diag = {
                        'status': 'normal' if prediction == 0 else 'warning',
                        'iso_zone': self._map_class_to_zone(prediction),
                        'triggered_rules': [],
                        'confidence': round(confidence, 4),
                        'diagnosis_summary':
                            f"主模型诊断: {self.class_names[prediction]} "
                            f"(置信度 {confidence:.1%})"
                    }

                    return CascadeResult(
                        prediction=prediction,
                        predicted_class=self.class_names[prediction],
                        confidence=round(confidence, 4),
                        model_used='primary_cnn_lstm',
                        inference_time_ms=round(elapsed, 2),
                        fallback_triggered=False,
                        fallback_reason='',
                        diagnosis=diag
                    )
                else:
                    self._stats['fallback_by_confidence'] += 1
                    logger.debug(
                        "Low confidence %.3f, falling back to rule engine", confidence
                    )
            except Exception as e:
                self._stats['fallback_by_error'] += 1
                logger.warning("Primary model error: %s, falling back", e)
        else:
            self._stats['fallback_by_error'] += 1
            logger.debug("Primary model not loaded, using fallback")

        self._stats['fallback_used'] += 1

        feat = features or {}
        diagnosis = self.fallback.diagnose(feat)

        status_to_pred = {
            'normal': 0,
            'info': 0,
            'warning': 1,
            'critical': 3
        }
        prediction = status_to_pred.get(diagnosis['status'], 1)

        elapsed = (time.perf_counter() - start_time) * 1000
        self._stats['total_inference_time_ms'] += elapsed

        reason = (
            "primary_low_confidence" if self.primary is not None
            else "primary_not_loaded"
        )

        return CascadeResult(
            prediction=prediction,
            predicted_class=self._zone_to_class(diagnosis.get('iso_zone', 'C')),
            confidence=diagnosis.get('confidence', 0.8),
            model_used='fallback_rule_engine',
            inference_time_ms=round(elapsed, 2),
            fallback_triggered=True,
            fallback_reason=reason,
            diagnosis=diagnosis
        )

    def batch_infer(
        self, X: np.ndarray, feature_dicts: Optional[list] = None
    ) -> list:
        """
        批量级联推理。

        Args:
            X: 批量数据 (N, window_size, num_features)
            feature_dicts: 每条数据的特征字典列表

        Returns:
            CascadeResult 列表
        """
        results = []
        for i in range(len(X)):
            feat = feature_dicts[i] if feature_dicts else None
            res = self.infer(X[i:i+1], features=feat)
            results.append(res)
        return results

    def evaluate_cascade(
        self,
        X_test: np.ndarray,
        y_test: np.ndarray,
        feature_dicts: Optional[list] = None
    ) -> Dict:
        """
        评估级联系统性能。

        Args:
            X_test: 测试数据
            y_test: 真实标签
            feature_dicts: 特征字典列表

        Returns:
            评估指标字典
        """
        from utils.metrics import compute_classification_metrics

        results = self.batch_infer(X_test, feature_dicts)
        y_pred = np.array([r.prediction for r in results])

        metrics = compute_classification_metrics(
            y_test, y_pred, None, self.class_names
        )

        metrics['fallback_rate'] = (
            self._stats['fallback_used'] / max(self._stats['total_inferences'], 1)
        )
        metrics['avg_inference_time_ms'] = (
            self._stats['total_inference_time_ms'] /
            max(self._stats['total_inferences'], 1)
        )

        logger.info(
            "Cascade evaluation: accuracy=%.4f, fallback_rate=%.2f%%",
            metrics['accuracy'], metrics['fallback_rate'] * 100
        )
        return metrics

    def get_stats(self) -> Dict:
        """获取级联统计信息"""
        return self._stats.copy()

    def reset_stats(self):
        """重置统计"""
        for k in self._stats:
            self._stats[k] = 0 if k != 'total_inference_time_ms' else 0.0

    def _map_class_to_zone(self, prediction: int) -> str:
        zone_map = {0: 'A', 1: 'C', 2: 'B', 3: 'D'}
        return zone_map.get(prediction, 'C')

    def _zone_to_class(self, zone: str) -> str:
        zone_class_map = {'A': 'normal', 'B': 'imbalance', 'C': 'misalignment', 'D': 'bearing_fault'}
        return zone_class_map.get(zone, 'unknown')
