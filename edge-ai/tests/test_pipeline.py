import os
import sys
import pytest
import numpy as np
import pandas as pd
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class TestDataPipeline:
    """数据管道集成测试"""

    def test_cleaner_basic(self):
        """测试数据清洗器基本功能"""
        from data_pipeline.data_cleaner import DataCleaner

        cleaner = DataCleaner()
        df = pd.DataFrame({
            'timestamp_ms': [1000, 2000, 2000, 3000],
            'dev_id': [1, 1, 1, 1],
            'rms_x': [0.5, 1.0, 1.0, np.nan],
            'rms_y': [0.3, 0.8, 0.8, 0.2],
            'rms_z': [0.1, 0.6, 0.6, 0.3],
            'overall_rms': [0.6, 1.2, 1.2, 1.0],
            'peak_freq': [50, 120, 120, 80],
            'peak_amp': [0.1, 0.2, 0.2, 0.15],
            'temperature_c': [25, 26, 26, 26],
            'humidity_rh': [60, 62, 62, 61]
        })

        cleaned = cleaner.clean(df)
        assert len(cleaned) <= 4
        assert cleaned['rms_x'].isnull().sum() == 0

    def test_feature_extractor(self):
        """测试特征提取"""
        from data_pipeline.feature_extractor import FeatureExtractor, FeatureConfig

        extractor = FeatureExtractor(FeatureConfig(window_size=64, overlap=32))
        df = pd.DataFrame({
            'timestamp_ms': range(200),
            'dev_id': [1] * 200,
            'rms_x': np.sin(np.linspace(0, 4*np.pi, 200)) * 0.5,
            'rms_y': np.cos(np.linspace(0, 4*np.pi, 200)) * 0.3,
            'rms_z': np.sin(np.linspace(0, 2*np.pi, 200)) * 0.1,
            'overall_rms': np.abs(np.sin(np.linspace(0, 4*np.pi, 200))) * 0.6,
            'peak_freq': np.ones(200) * 50,
            'peak_amp': np.ones(200) * 0.2,
            'temperature_c': np.ones(200) * 25,
            'humidity_rh': np.ones(200) * 60
        })

        features = extractor.extract_features(df)
        assert len(features) > 0
        assert 'rms_x' in features.columns
        assert 'skewness_x' in features.columns


class TestFallbackModel:
    """兜底模型测试"""

    def test_rule_engine_normal(self):
        """测试规则引擎正常诊断"""
        from models.fallback.rule_engine import RuleEngine

        engine = RuleEngine()
        features = {
            'overall_rms': 1.0,
            'peak_freq': 100,
            'temperature_c': 30,
            'crest_factor': 2.0,
            'kurtosis': 2.5
        }

        result = engine.diagnose(features)
        assert result['iso_zone'] == 'A'
        assert result['status'] == 'normal'

    def test_rule_engine_critical(self):
        """测试规则引擎危险诊断"""
        from models.fallback.rule_engine import RuleEngine

        engine = RuleEngine()
        features = {
            'overall_rms': 10.0,
            'peak_freq': 600,
            'temperature_c': 90,
            'crest_factor': 7.0,
            'kurtosis': 5.0
        }

        result = engine.diagnose(features)
        assert result['iso_zone'] in ('C', 'D')
        assert len(result['triggered_rules']) > 0

    def test_statistical_detector(self):
        """测试SPC统计检测器"""
        from models.fallback.statistical_detector import StatisticalDetector

        detector = StatisticalDetector(warm_up_period=10)

        for _ in range(10):
            detector.update(5.0)

        r = detector.update(5.0)
        assert not r['is_anomaly']

        r = detector.update(20.0)
        assert r['is_anomaly']


class TestEnsemble:
    """级联编排测试"""

    def test_fallback_only_cascade(self):
        """测试纯规则引擎级联"""
        from models.ensemble.model_cascade import ModelCascade

        cascade = ModelCascade(primary_model=None)
        features = {
            'overall_rms': 1.0,
            'peak_freq': 100,
            'temperature_c': 25
        }

        X = np.random.randn(1, 256, 6).astype(np.float32)
        result = cascade.infer(X, features)

        assert result.model_used == 'fallback_rule_engine'
        assert result.fallback_triggered
