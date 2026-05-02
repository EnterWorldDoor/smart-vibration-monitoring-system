import numpy as np
import time
from typing import Dict, Optional
from pathlib import Path

from models.primary.vibration_cnn import VibrationCNNLSTM
from models.primary.autoencoder import VibrationAutoencoder
from models.fallback.rule_engine import RuleEngine
from models.ensemble.model_cascade import ModelCascade, CascadeResult
from data_pipeline.feature_extractor import FeatureExtractor
from utils.logger import get_logger

logger = get_logger(__name__)


class InferenceEngine:
    """
    企业级统一推理引擎 (PC端验证/测试用，Anaconda 环境)。

    用途:
    - 训练后在PC端验证模型推理效果
    - 对比 Keras vs TFLite 推理精度
    - 模拟ESP32双模型级联逻辑进行功能测试
    - 实际部署在 ESP32-S3 (ai_service.c + TFLite Micro)

    使用示例:
        conda activate edgevib-tf
        engine = InferenceEngine()
        engine.load_primary('models/saved_models/primary_best.h5')
        result = engine.infer(window_data, feature_dict)
    """

    def __init__(
        self,
        confidence_threshold: float = 0.85,
        model_dir: str = 'models/saved_models'
    ):
        self.confidence_threshold = confidence_threshold
        self.model_dir = Path(model_dir)

        self.primary_model: Optional[VibrationCNNLSTM] = None
        self.autoencoder: Optional[VibrationAutoencoder] = None
        self.fallback_engine: RuleEngine = RuleEngine()
        self.cascade: Optional[ModelCascade] = None

        self.feature_extractor = FeatureExtractor()

        self._stats = {
            'total_inferences': 0,
            'online_inferences': 0,
            'batch_inferences': 0,
            'avg_inference_time_ms': 0.0,
            'total_inference_time_ms': 0.0,
            'start_time': time.time()
        }

        self._result_cache = {}
        self._max_cache_size = 100

        logger.info("InferenceEngine initialized (threshold=%.2f)", confidence_threshold)

    def load_primary(self, model_path: str) -> bool:
        """
        加载主模型。

        Args:
            model_path: Keras .h5 模型路径

        Returns:
            True 加载成功
        """
        try:
            self.primary_model = VibrationCNNLSTM()
            self.primary_model.load(model_path)
            self._rebuild_cascade()
            logger.info("Primary model loaded: %s", model_path)
            return True
        except Exception as e:
            logger.error("Failed to load primary model: %s", e)
            self.primary_model = None
            self._rebuild_cascade()
            return False

    def unload_primary(self):
        """卸载主模型，完全降级到规则引擎"""
        self.primary_model = None
        self._rebuild_cascade()
        logger.info("Primary model unloaded, running on fallback only")

    def load_autoencoder(self, model_path: str) -> bool:
        """
        加载自编码器模型。

        Args:
            model_path: Autoencoder .h5 模型路径

        Returns:
            True 加载成功
        """
        try:
            self.autoencoder = VibrationAutoencoder()
            self.autoencoder.load(model_path)
            logger.info("Autoencoder loaded: %s", model_path)
            return True
        except Exception as e:
            logger.error("Failed to load autoencoder: %s", e)
            self.autoencoder = None
            return False

    def load_fallback_config(self, config_path: str):
        """加载兜底规则引擎配置"""
        self.fallback_engine.load_config(config_path)
        self._rebuild_cascade()
        logger.info("Fallback config loaded: %s", config_path)

    def infer(
        self,
        X: np.ndarray,
        features: Optional[Dict[str, float]] = None
    ) -> CascadeResult:
        """
        单次推理。

        Args:
            X: 输入数据 (window_size, num_features)
            features: 特征字典

        Returns:
            CascadeResult
        """
        start = time.perf_counter()
        result = self.cascade.infer(X, features)
        elapsed = (time.perf_counter() - start) * 1000

        self._stats['total_inferences'] += 1
        self._stats['online_inferences'] += 1
        self._stats['total_inference_time_ms'] += elapsed
        self._stats['avg_inference_time_ms'] = (
            self._stats['total_inference_time_ms'] /
            max(self._stats['online_inferences'], 1)
        )

        return result

    def infer_from_raw(
        self,
        window_data: np.ndarray,
        features: Optional[Dict[str, float]] = None
    ) -> CascadeResult:
        """
        从原始窗口数据进行推理。

        Args:
            window_data: 原始传感器数据 (window_size, num_channels)
            features: 特征字典（可选，用于规则引擎）

        Returns:
            CascadeResult
        """
        return self.infer(window_data, features)

    def batch_infer(
        self,
        X: np.ndarray,
        feature_dicts: Optional[list] = None
    ) -> list:
        """
        批量推理。

        Args:
            X: (N, window_size, num_features)
            feature_dicts: 特征字典列表

        Returns:
            CascadeResult 列表
        """
        results = self.cascade.batch_infer(X, feature_dicts)
        n = len(results)
        self._stats['total_inferences'] += n
        self._stats['batch_inferences'] += n
        return results

    def detect_anomaly(self, X: np.ndarray) -> np.ndarray:
        """
        使用Autoencoder进行无监督异常检测。

        Args:
            X: (N, features)

        Returns:
            布尔数组 True=异常
        """
        if self.autoencoder is None:
            logger.warning("Autoencoder not loaded, using rule engine fallback")
            return np.zeros(len(X), dtype=bool)
        return self.autoencoder.detect(X)

    def get_status(self) -> Dict:
        """获取推理引擎状态"""
        return {
            'primary_loaded': self.primary_model is not None,
            'autoencoder_loaded': self.autoencoder is not None,
            'fallback_configured': True,
            'confidence_threshold': self.confidence_threshold,
            'stats': self._stats,
            'cascade_stats': self.cascade.get_stats() if self.cascade else {},
            'uptime_seconds': time.time() - self._stats['start_time']
        }

    def _rebuild_cascade(self):
        """重建级联控制器"""
        self.cascade = ModelCascade(
            primary_model=self.primary_model,
            fallback_engine=self.fallback_engine,
            confidence_threshold=self.confidence_threshold
        )
