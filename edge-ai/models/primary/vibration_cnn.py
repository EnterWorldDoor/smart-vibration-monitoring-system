import numpy as np
import os
from typing import Dict, List, Optional, Tuple

os.environ['TF_CPP_MIN_LOG_LEVEL'] = '2'

import tensorflow as tf
from tensorflow import keras
from tensorflow.keras import layers

from utils.logger import get_logger

logger = get_logger(__name__)


class VibrationCNNLSTM:
    """
    企业级 1D-CNN + LSTM 混合振动分类模型。

    架构设计:
        Input (window_size, features)
        → Conv1D(filters=32, kernel=5) + BN + ReLU + Dropout
        → Conv1D(filters=64, kernel=5) + BN + ReLU + Dropout
        → Conv1D(filters=128, kernel=3) + BN + ReLU
        → MaxPooling1D
        → LSTM(64, return_sequences=False)
        → Dense(64) + BN + ReLU + Dropout
        → Dense(num_classes, softmax)

    特性:
        - BatchNormalization 加速收敛
        - Dropout 防止过拟合
        - 早停 + 学习率衰减
        - 模型检查点自动保存
        - 置信度校准
    """

    def __init__(
        self,
        input_shape: Tuple[int, int] = (256, 24),
        num_classes: int = 4,
        class_names: Optional[List[str]] = None
    ):
        self.input_shape = input_shape
        self.num_classes = num_classes
        self.class_names = class_names or [
            'normal', 'imbalance', 'misalignment', 'bearing_fault'
        ][:num_classes]
        self.model: Optional[keras.Model] = None
        self.history: Optional[Dict] = None

    def build(self) -> keras.Model:
        """
        构建CNN+LSTM混合模型。

        Returns:
            编译好的Keras Model
        """
        inputs = keras.Input(shape=self.input_shape, name='vibration_input')

        x = layers.Conv1D(32, kernel_size=5, padding='same', name='conv1')(inputs)
        x = layers.BatchNormalization(name='bn1')(x)
        x = layers.ReLU(name='relu1')(x)
        x = layers.Dropout(0.2, name='drop1')(x)

        x = layers.Conv1D(64, kernel_size=5, padding='same', name='conv2')(x)
        x = layers.BatchNormalization(name='bn2')(x)
        x = layers.ReLU(name='relu2')(x)
        x = layers.Dropout(0.2, name='drop2')(x)

        x = layers.Conv1D(128, kernel_size=3, padding='same', name='conv3')(x)
        x = layers.BatchNormalization(name='bn3')(x)
        x = layers.ReLU(name='relu3')(x)

        x = layers.MaxPooling1D(pool_size=2, name='maxpool')(x)

        x = layers.LSTM(64, return_sequences=False, name='lstm')(x)

        x = layers.Dense(64, name='dense1')(x)
        x = layers.BatchNormalization(name='bn4')(x)
        x = layers.ReLU(name='relu4')(x)
        x = layers.Dropout(0.3, name='drop3')(x)

        outputs = layers.Dense(
            self.num_classes, activation='softmax', name='output'
        )(x)

        model = keras.Model(inputs=inputs, outputs=outputs, name='vibration_cnn_lstm')

        model.compile(
            optimizer=keras.optimizers.Adam(learning_rate=0.001),
            loss='sparse_categorical_crossentropy',
            metrics=['accuracy']
        )

        self.model = model
        logger.info(
            "Model built: input=%s, classes=%d, params=%d",
            self.input_shape, self.num_classes, model.count_params()
        )
        return model

    def train(
        self,
        X_train: np.ndarray,
        y_train: np.ndarray,
        X_val: np.ndarray,
        y_val: np.ndarray,
        epochs: int = 100,
        batch_size: int = 32,
        patience: int = 15,
        model_dir: str = 'models/saved_models'
    ) -> Dict:
        """
        训练模型。

        Args:
            X_train: 训练特征
            y_train: 训练标签
            X_val: 验证特征
            y_val: 验证标签
            epochs: 最大训练轮数
            batch_size: 批次大小
            patience: 早停耐心值
            model_dir: 模型保存目录

        Returns:
            训练历史字典
        """
        if self.model is None:
            self.build()

        callbacks = [
            keras.callbacks.EarlyStopping(
                monitor='val_accuracy',
                patience=patience,
                restore_best_weights=True,
                verbose=1
            ),
            keras.callbacks.ReduceLROnPlateau(
                monitor='val_loss',
                factor=0.5,
                patience=8,
                min_lr=1e-6,
                verbose=1
            ),
            keras.callbacks.ModelCheckpoint(
                filepath=os.path.join(model_dir, 'primary_best.h5'),
                monitor='val_accuracy',
                save_best_only=True,
                verbose=1
            ),
            keras.callbacks.CSVLogger(
                os.path.join(model_dir, '..', '..', 'logs', 'training_history.csv')
            )
        ]

        os.makedirs(model_dir, exist_ok=True)

        logger.info(
            "Training: epochs=%d, batch=%d, train_samples=%d, val_samples=%d",
            epochs, batch_size, len(X_train), len(X_val)
        )

        history = self.model.fit(
            X_train, y_train,
            validation_data=(X_val, y_val),
            epochs=epochs,
            batch_size=batch_size,
            callbacks=callbacks,
            verbose=1
        )

        self.history = history.history

        best_epoch = np.argmax(history.history['val_accuracy']) + 1
        best_val_acc = np.max(history.history['val_accuracy'])
        logger.info(
            "Training complete: best_val_accuracy=%.4f at epoch %d",
            best_val_acc, best_epoch
        )

        return history.history

    def predict(
        self, X: np.ndarray, return_confidence: bool = True
    ) -> np.ndarray:
        """
        模型推理。

        Args:
            X: 输入数据 (N, window_size, features)
            return_confidence: 是否返回概率而非类别

        Returns:
            如果 return_confidence=True: 概率数组 (N, num_classes)
            否则: 类别索引 (N,)
        """
        if self.model is None:
            raise RuntimeError("Model not built or loaded")

        probas = self.model.predict(X, verbose=0)
        if return_confidence:
            return probas
        return np.argmax(probas, axis=1)

    def evaluate(
        self, X_test: np.ndarray, y_test: np.ndarray
    ) -> Dict:
        """
        评估模型性能。

        Args:
            X_test: 测试特征
            y_test: 测试标签

        Returns:
            评估指标字典
        """
        from utils.metrics import compute_classification_metrics

        y_prob = self.predict(X_test, return_confidence=True)
        y_pred = np.argmax(y_prob, axis=1)

        metrics = compute_classification_metrics(
            y_test, y_pred, y_prob, self.class_names
        )
        logger.info(
            "Evaluation: accuracy=%.4f, f1=%.4f",
            metrics['accuracy'], metrics['f1_weighted']
        )
        return metrics

    def save(self, filepath: str):
        """保存Keras模型"""
        if self.model is None:
            raise RuntimeError("No model to save")
        os.makedirs(os.path.dirname(filepath), exist_ok=True)
        self.model.save(filepath)
        logger.info("Model saved: %s", filepath)

    def load(self, filepath: str):
        """加载Keras模型"""
        if not os.path.exists(filepath):
            raise FileNotFoundError(f"Model not found: {filepath}")
        self.model = keras.models.load_model(filepath)
        logger.info("Model loaded: %s", filepath)

    def get_confidence(self, X: np.ndarray) -> np.ndarray:
        """获取预测置信度（最大概率）"""
        probas = self.predict(X, return_confidence=True)
        return np.max(probas, axis=1)
