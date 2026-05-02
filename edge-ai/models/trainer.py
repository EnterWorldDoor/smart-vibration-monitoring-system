import numpy as np
import time
from typing import Dict, Optional, List
from pathlib import Path

from data_pipeline.feature_extractor import FeatureExtractor, FeatureConfig
from data_pipeline.data_cleaner import DataCleaner
from data_pipeline.dataset_builder import DatasetBuilder
from models.primary.vibration_cnn import VibrationCNNLSTM
from models.primary.autoencoder import VibrationAutoencoder
from models.fallback.rule_engine import RuleEngine, RuleEngineConfig
from models.ensemble.model_cascade import ModelCascade
from deployment.tflite_converter import TFLiteConverter
from inference.inference_engine import InferenceEngine
from monitoring.drift_detector import DriftDetector, AlertManager
from utils.logger import get_logger
from utils.config_loader import ConfigLoader

logger = get_logger(__name__)


class EdgeAIPipeline:
    """
    Edge-AI 完整训练/部署流水线编排器 (PC端 Anaconda 环境运行)。

    训练 → TFLite转换 → C头文件生成，模型最终部署到 ESP32-S3。

    Usage:
        conda activate edgevib-tf
        python -c "from models.trainer import EdgeAIPipeline; \\
            EdgeAIPipeline().run_full_pipeline()"
    """

    def __init__(
        self,
        data_csv: str = 'data_collection/training_data.csv',
        model_dir: str = 'models/saved_models',
        output_dir: str = 'datasets'
    ):
        self.data_csv = data_csv
        self.model_dir = Path(model_dir)
        self.output_dir = Path(output_dir)
        self.model_dir.mkdir(parents=True, exist_ok=True)
        self.output_dir.mkdir(parents=True, exist_ok=True)

        self.cleaner = DataCleaner()
        self.extractor = FeatureExtractor(FeatureConfig(
            window_size=256, overlap=128, sampling_rate_hz=400
        ))
        self.builder = DatasetBuilder(window_size=256, step_size=128)

        self.primary_model = VibrationCNNLSTM(input_shape=(256, 24))
        self.autoencoder = VibrationAutoencoder(input_dim=24)
        self.fallback = RuleEngine()
        self.converter = TFLiteConverter(str(self.model_dir))

        self.cascade: Optional[ModelCascade] = None
        self.engine: Optional[InferenceEngine] = None

    def run_data_pipeline(self) -> Path:
        """运行数据管道：清洗 → 特征提取 → 保存"""
        import pandas as pd

        logger.info("=== Stage 1: Data Pipeline ===")

        df = pd.read_csv(self.data_csv)
        logger.info("Loaded raw data: %d records", len(df))

        df_clean = self.cleaner.clean(df)
        report = self.cleaner.generate_report()
        logger.info("Data quality: %.1f%% complete", report['completeness_percent'])

        df_features = self.extractor.extract_features(df_clean)
        features_path = self.output_dir / 'features_train.csv'
        df_features.to_csv(features_path, index=False)
        logger.info("Features saved: %s (%d rows, %d cols)",
                    features_path, len(df_features), len(df_features.columns))

        return features_path

    def run_autoencoder_pipeline(self, features_path: Path) -> Dict:
        """运行Autoencoder无监督训练管道"""
        import pandas as pd
        import json

        logger.info("=== Stage 2: Autoencoder Unsupervised Training ===")

        ae_dataset = self.builder.build_autoencoder_dataset(
            str(features_path)
        )

        self.autoencoder.build()
        history = self.autoencoder.train(
            ae_dataset['X_train'].reshape(-1, 24),
            ae_dataset['X_test'].reshape(-1, 24),
            epochs=100,
            model_dir=str(self.model_dir)
        )

        self.autoencoder.fit_threshold(
            ae_dataset['X_train'].reshape(-1, 24)
        )

        self.autoencoder.save(str(self.model_dir / 'autoencoder_best.h5'))

        return {
            'best_val_loss': float(np.min(history['val_loss'])),
            'threshold': self.autoencoder._threshold,
            'model_path': str(self.model_dir / 'autoencoder_best.h5')
        }

    def run_fallback_calibration(self, features_path: Path):
        """标定兜底规则引擎阈值"""
        import pandas as pd

        logger.info("=== Stage 3: Fallback Rule Engine Calibration ===")

        df_features = pd.read_csv(features_path)
        self.fallback.calibrate_from_data(df_features)

        config_path = self.model_dir / 'fallback_config.yaml'
        self.fallback.save_config(str(config_path))

        logger.info("Fallback calibrated and saved: %s", config_path)

    def run_primary_training(
        self, features_path: Path
    ) -> Dict:
        """
        运行主模型训练管道。

        需要标注数据时使用。对于无标注数据，
        跳过此步骤或使用Autoencoder重构误差作为伪标签。
        """
        import pandas as pd
        import json

        logger.info("=== Stage 4: Primary Model Training ===")

        datasets = self.builder.build_from_csv(
            str(features_path)
        )

        self.primary_model.build()
        history = self.primary_model.train(
            datasets['X_train'], datasets['y_train'],
            datasets['X_val'], datasets['y_val'],
            epochs=80,
            model_dir=str(self.model_dir)
        )

        self.primary_model.save(str(self.model_dir / 'primary_best.h5'))

        metrics = self.primary_model.evaluate(
            datasets['X_test'], datasets['y_test']
        )

        metrics_path = self.model_dir / '..' / '..' / 'reports' / 'evaluation_metrics.json'
        metrics_path.parent.mkdir(parents=True, exist_ok=True)
        with open(metrics_path, 'w') as f:
            json.dump(metrics, f, indent=2)

        return metrics

    def run_deployment_pipeline(self) -> Dict:
        """运行部署管道：TFLite转换 + 量化 + C头文件导出"""
        logger.info("=== Stage 5: Deployment Pipeline ===")

        primary_h5 = self.model_dir / 'primary_best.h5'

        results = {}

        if primary_h5.exists():
            tflite_path, info = self.converter.convert_keras_to_tflite(
                str(primary_h5),
                output_name='edgevib_classifier',
                optimization='dynamic'
            )
            results['tflite_dynamic'] = info

            tflite_int8_path, int8_info = self.converter.convert_keras_to_tflite(
                str(primary_h5),
                output_name='edgevib_classifier',
                optimization='int8',
                representative_data=(
                    np.load(str(self.output_dir / 'X_train_sample.npy'))
                    if (self.output_dir / 'X_train_sample.npy').exists()
                    else None
                )
            )
            results['tflite_int8'] = int8_info

            header_path = self.model_dir / '..' / '..' / 'deployment' / 'esp32_model_data.h'
            self.converter.generate_c_header(
                tflite_path, str(header_path)
            )
            results['c_header'] = str(header_path)

        else:
            logger.warning("No primary model found, skipping TFLite conversion")

        return results

    def init_inference_engine(self) -> InferenceEngine:
        """初始化推理引擎"""
        engine = InferenceEngine(
            confidence_threshold=0.85,
            model_dir=str(self.model_dir)
        )

        primary_path = self.model_dir / 'primary_best.h5'
        if primary_path.exists():
            engine.load_primary(str(primary_path))

        ae_path = self.model_dir / 'autoencoder_best.h5'
        if ae_path.exists():
            engine.load_autoencoder(str(ae_path))

        fallback_path = self.model_dir / 'fallback_config.yaml'
        if fallback_path.exists():
            engine.load_fallback_config(str(fallback_path))

        self.engine = engine
        return engine

    def run_full_pipeline(self) -> Dict:
        """一键运行完整流水线"""
        logger.info("=== EdgeVib Edge-AI Full Pipeline ===")
        start_time = time.time()

        features_path = self.run_data_pipeline()

        ae_results = self.run_autoencoder_pipeline(features_path)

        self.run_fallback_calibration(features_path)

        primary_results = self.run_primary_training(features_path)

        deploy_results = self.run_deployment_pipeline()

        elapsed = time.time() - start_time

        summary = {
            'pipeline_duration_seconds': round(elapsed, 1),
            'autoencoder': ae_results,
            'primary': primary_results,
            'deployment': deploy_results,
            'model_dir': str(self.model_dir),
            'output_dir': str(self.output_dir)
        }

        logger.info("=== Pipeline Complete (%.1fs) ===", elapsed)
        return summary
