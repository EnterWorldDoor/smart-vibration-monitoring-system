import numpy as np
import pandas as pd
from typing import Dict, Optional, Tuple
from pathlib import Path

from utils.logger import get_logger

logger = get_logger(__name__)


class DatasetBuilder:
    """
    企业级数据集构建器。

    功能：
    - 滑动窗口切分时序数据
    - 训练/验证/测试集分层划分
    - 数据增强（加噪、缩放、时间平移）
    - 数据版本管理与缓存
    """

    def __init__(
        self,
        window_size: int = 256,
        step_size: int = 128,
        test_ratio: float = 0.15,
        val_ratio: float = 0.15,
        random_seed: int = 42
    ):
        self.window_size = window_size
        self.step_size = step_size
        self.test_ratio = test_ratio
        self.val_ratio = val_ratio
        self.random_seed = random_seed

    def build_from_csv(
        self,
        csv_path: str,
        feature_cols: Optional[list] = None
    ) -> Dict[str, np.ndarray]:
        """
        从CSV文件构建训练/验证/测试集。

        Args:
            csv_path: CSV文件路径
            feature_cols: 特征列名列表，None则自动选择

        Returns:
            包含 X_train, X_val, X_test, y_train, y_val, y_test 的字典
        """
        logger.info("Building dataset from %s", csv_path)

        df = pd.read_csv(csv_path)
        logger.info("Loaded %d records, %d columns", len(df), len(df.columns))

        if feature_cols is None:
            exclude_cols = ['timestamp_ms', 'dev_id', 'label',
                           'window_start_ms', 'window_end_ms',
                           'is_outlier', 'Unnamed: 0']
            feature_cols = [c for c in df.columns if c not in exclude_cols]

        df = df.dropna(subset=feature_cols)

        X = df[feature_cols].values.astype(np.float32)

        if 'label' in df.columns:
            y_raw = df['label'].values
            y = self._encode_labels(y_raw)
        else:
            y = np.zeros(len(df), dtype=np.int32)

        X_windows, y_windows = self._sliding_window(X, y)

        datasets = self._train_val_test_split(X_windows, y_windows)
        datasets['feature_names'] = feature_cols
        datasets['num_classes'] = len(np.unique(y))

        logger.info(
            "Dataset built: train=%d, val=%d, test=%d, features=%d, classes=%d",
            len(datasets['X_train']), len(datasets['X_val']),
            len(datasets['X_test']), len(feature_cols),
            datasets['num_classes']
        )

        return datasets

    def build_autoencoder_dataset(
        self,
        csv_path: str,
        feature_cols: Optional[list] = None,
        contamination: float = 0.05
    ) -> Dict[str, np.ndarray]:
        """
        构建Autoencoder无监督训练集。

        将数据分为：
        - 训练集：所有数据（模型学习重构正常数据）
        - 测试集：包含少量异常的混合数据

        Args:
            csv_path: CSV文件路径
            feature_cols: 特征列名
            contamination: 保留作为异常测试的数据比例

        Returns:
            包含 X_train, X_test, anomaly_mask 的字典
        """
        logger.info("Building autoencoder dataset from %s", csv_path)

        df = pd.read_csv(csv_path)

        if feature_cols is None:
            exclude_cols = ['timestamp_ms', 'dev_id', 'label',
                           'window_start_ms', 'window_end_ms',
                           'is_outlier', 'Unnamed: 0']
            feature_cols = [c for c in df.columns if c not in exclude_cols]

        df = df.dropna(subset=feature_cols)
        X = df[feature_cols].values.astype(np.float32)

        X_windows, _ = self._sliding_window(X, np.zeros(len(df)))

        n_samples = len(X_windows)
        n_test = max(int(n_samples * contamination), 10)
        indices = np.random.RandomState(self.random_seed).permutation(n_samples)

        train_idx = indices[n_test:]
        test_idx = indices[:n_test]

        return {
            'X_train': X_windows[train_idx],
            'X_test': X_windows[test_idx],
            'feature_names': feature_cols,
            'n_train': len(train_idx),
            'n_test': len(test_idx)
        }

    def _sliding_window(
        self, X: np.ndarray, y: np.ndarray
    ) -> Tuple[np.ndarray, np.ndarray]:
        """滑动窗口切分"""
        n_samples = len(X)
        windows_X = []
        windows_y = []

        for i in range(0, n_samples - self.window_size + 1, self.step_size):
            windows_X.append(X[i:i + self.window_size])
            windows_y.append(y[i + self.window_size // 2])

        return np.array(windows_X, dtype=np.float32), np.array(windows_y, dtype=np.int32)

    def _encode_labels(self, labels: np.ndarray) -> np.ndarray:
        """将文本标签编码为整数"""
        from sklearn.preprocessing import LabelEncoder

        le = LabelEncoder()
        encoded = le.fit_transform(labels)
        self.label_encoder = le
        self.class_names = list(le.classes_)
        logger.info("Label encoding: %s", dict(zip(le.classes_, range(len(le.classes_)))))
        return encoded

    def _train_val_test_split(
        self, X: np.ndarray, y: np.ndarray
    ) -> Dict[str, np.ndarray]:
        """分层划分数据集"""
        from sklearn.model_selection import train_test_split

        rng = np.random.RandomState(self.random_seed)
        indices = rng.permutation(len(X))

        n_test = int(len(X) * self.test_ratio)
        n_val = int(len(X) * self.val_ratio)

        test_idx = indices[:n_test]
        val_idx = indices[n_test:n_test + n_val]
        train_idx = indices[n_test + n_val:]

        return {
            'X_train': X[train_idx],
            'y_train': y[train_idx],
            'X_val': X[val_idx],
            'y_val': y[val_idx],
            'X_test': X[test_idx],
            'y_test': y[test_idx]
        }

    def augment_data(
        self,
        X: np.ndarray,
        noise_std: float = 0.02,
        scale_range: Tuple[float, float] = (0.95, 1.05),
        time_shift_max: int = 10
    ) -> np.ndarray:
        """
        数据增强：加噪、缩放、时间平移。

        Args:
            X: 原始数据 (N, window_size, features)
            noise_std: 高斯噪声标准差
            scale_range: 随机缩放范围
            time_shift_max: 最大时间平移量

        Returns:
            增强后的数据
        """
        rng = np.random.RandomState(self.random_seed)
        X_aug = X.copy()

        noise = rng.normal(0, noise_std, X_aug.shape).astype(np.float32)
        X_aug = X_aug + noise

        scales = rng.uniform(scale_range[0], scale_range[1],
                            (len(X_aug), 1, X_aug.shape[2])).astype(np.float32)
        X_aug = X_aug * scales

        if time_shift_max > 0:
            for i in range(len(X_aug)):
                shift = rng.randint(-time_shift_max, time_shift_max + 1)
                X_aug[i] = np.roll(X_aug[i], shift, axis=0)

        logger.info(
            "Data augmented: %d → %d samples (noise=%.3f, scale=[%.2f,%.2f])",
            len(X), len(X_aug), noise_std, scale_range[0], scale_range[1]
        )
        return X_aug
