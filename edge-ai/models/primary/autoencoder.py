import numpy as np
import os
from typing import Dict, Optional, Tuple

os.environ['TF_CPP_MIN_LOG_LEVEL'] = '2'

import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

from utils.logger import get_logger

logger = get_logger(__name__)


class VibrationAutoencoder:
    """
    振动数据自编码器 — 无监督异常检测。

    原理:
        - 编码器将高维振动特征压缩到低维潜在空间
        - 解码器从潜在空间重构原始信号
        - 重构误差 (MSE) 作为异常分数
        - 正常数据重构误差小，异常数据重构误差大

    架构:
        Encoder: Input → Dense(128)→Dense(64)→Dense(32)→Dense(16)→Latent(8)
        Decoder: Latent(8)→Dense(16)→Dense(32)→Dense(64)→Dense(128)→Output

    创新点:
        - 可作为CNN分类器的预训练权重初始化
        - 支持迁移学习：编码器权重转移到分类器
    """

    def __init__(
        self,
        input_dim: int = 24,
        latent_dim: int = 8,
        encoder_layers: Optional[list] = None
    ):
        self.input_dim = input_dim
        self.latent_dim = latent_dim
        self.encoder_layers = encoder_layers or [128, 64, 32, 16]
        self.encoder: Optional[keras.Model] = None
        self.decoder: Optional[keras.Model] = None
        self.autoencoder: Optional[keras.Model] = None
        self.history: Optional[Dict] = None
        self._threshold: Optional[float] = None

    def build(self) -> keras.Model:
        """
        构建自编码器模型。

        Returns:
            编译好的Autoencoder Keras Model
        """
        encoder_input = keras.Input(shape=(self.input_dim,), name='encoder_input')
        x = encoder_input

        for units in self.encoder_layers:
            x = layers.Dense(units, activation='relu', name=f'enc_dense_{units}')(x)
            x = layers.BatchNormalization(name=f'enc_bn_{units}')(x)

        latent = layers.Dense(
            self.latent_dim, activation='relu', name='latent'
        )(x)

        self.encoder = keras.Model(
            encoder_input, latent, name='encoder'
        )

        decoder_input = keras.Input(shape=(self.latent_dim,), name='decoder_input')
        x = decoder_input

        for units in reversed(self.encoder_layers):
            x = layers.Dense(units, activation='relu', name=f'dec_dense_{units}')(x)
            x = layers.BatchNormalization(name=f'dec_bn_{units}')(x)

        decoder_output = layers.Dense(
            self.input_dim, activation='linear', name='decoder_output'
        )(x)

        self.decoder = keras.Model(
            decoder_input, decoder_output, name='decoder'
        )

        autoencoder_input = keras.Input(shape=(self.input_dim,), name='autoencoder_input')
        reconstructed = self.decoder(self.encoder(autoencoder_input))

        self.autoencoder = keras.Model(
            autoencoder_input, reconstructed, name='vibration_autoencoder'
        )

        self.autoencoder.compile(
            optimizer=keras.optimizers.Adam(learning_rate=0.001),
            loss='mse',
            metrics=['mae']
        )

        logger.info(
            "Autoencoder built: input=%d, latent=%d, encoder_layers=%s, params=%d",
            self.input_dim, self.latent_dim,
            self.encoder_layers, self.autoencoder.count_params()
        )
        return self.autoencoder

    def train(
        self,
        X_train: np.ndarray,
        X_val: np.ndarray,
        epochs: int = 150,
        batch_size: int = 32,
        patience: int = 20,
        model_dir: str = 'models/saved_models'
    ) -> Dict:
        """
        训练自编码器。

        Args:
            X_train: 训练数据（正常数据为主）
            X_val: 验证数据
            epochs: 最大轮数
            batch_size: 批次大小
            patience: 早停耐心
            model_dir: 保存目录

        Returns:
            训练历史
        """
        if self.autoencoder is None:
            self.build()

        os.makedirs(model_dir, exist_ok=True)

        callbacks = [
            keras.callbacks.EarlyStopping(
                monitor='val_loss',
                patience=patience,
                restore_best_weights=True,
                verbose=1
            ),
            keras.callbacks.ReduceLROnPlateau(
                monitor='val_loss',
                factor=0.5,
                patience=10,
                min_lr=1e-6,
                verbose=1
            ),
            keras.callbacks.ModelCheckpoint(
                filepath=os.path.join(model_dir, 'autoencoder_best.h5'),
                monitor='val_loss',
                save_best_only=True,
                verbose=1
            )
        ]

        logger.info(
            "Training autoencoder: epochs=%d, batch=%d, samples=%d",
            epochs, batch_size, len(X_train)
        )

        history = self.autoencoder.fit(
            X_train, X_train,
            validation_data=(X_val, X_val),
            epochs=epochs,
            batch_size=batch_size,
            callbacks=callbacks,
            verbose=1
        )

        self.history = history.history

        best_val_loss = np.min(history.history['val_loss'])
        logger.info("Autoencoder training complete: best_val_loss=%.6f", best_val_loss)
        return history.history

    def compute_anomaly_scores(self, X: np.ndarray) -> np.ndarray:
        """
        计算异常分数（重构误差）。

        Args:
            X: 输入数据 (N, input_dim)

        Returns:
            异常分数数组 (N,)
        """
        if self.autoencoder is None:
            raise RuntimeError("Autoencoder not built or loaded")

        X_reconstructed = self.autoencoder.predict(X, verbose=0)
        scores = np.mean(np.square(X - X_reconstructed), axis=1)
        return scores

    def fit_threshold(
        self, X_train: np.ndarray, percentile: float = 95.0
    ) -> float:
        """
        基于训练数据拟合异常判定阈值。

        Args:
            X_train: 训练数据（正常数据）
            percentile: 分位数阈值，默认95分位

        Returns:
            阈值
        """
        scores = self.compute_anomaly_scores(X_train)
        self._threshold = float(np.percentile(scores, percentile))
        logger.info(
            "Threshold fitted: percentile=%.1f, threshold=%.6f",
            percentile, self._threshold
        )
        return self._threshold

    def detect(self, X: np.ndarray, threshold: Optional[float] = None) -> np.ndarray:
        """
        异常检测。

        Args:
            X: 输入数据
            threshold: 判定阈值，None使用已拟合的阈值

        Returns:
            布尔数组 True=异常
        """
        scores = self.compute_anomaly_scores(X)
        thresh = threshold or self._threshold
        if thresh is None:
            raise RuntimeError("Threshold not set. Call fit_threshold first.")
        return scores > thresh

    def get_encoder_weights(self) -> list:
        """获取编码器权重（用于迁移学习）"""
        if self.encoder is None:
            raise RuntimeError("Encoder not built")
        return self.encoder.get_weights()

    def save(self, filepath: str):
        """保存自编码器"""
        if self.autoencoder is None:
            raise RuntimeError("No model to save")
        os.makedirs(os.path.dirname(filepath), exist_ok=True)
        self.autoencoder.save(filepath)
        logger.info("Autoencoder saved: %s", filepath)

    def load(self, filepath: str):
        """加载自编码器"""
        if not os.path.exists(filepath):
            raise FileNotFoundError(f"Model not found: {filepath}")
        self.autoencoder = keras.models.load_model(filepath)
        logger.info("Autoencoder loaded: %s", filepath)
