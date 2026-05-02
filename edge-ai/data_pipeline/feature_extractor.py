import numpy as np
import pandas as pd
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass

from utils.logger import get_logger

logger = get_logger(__name__)


@dataclass
class FeatureConfig:
    """特征提取配置"""
    window_size: int = 256
    overlap: int = 128
    sampling_rate_hz: int = 400
    enable_frequency_features: bool = True
    enable_statistical_features: bool = True
    freq_bands: List[Tuple[float, float]] = None

    def __post_init__(self):
        if self.freq_bands is None:
            self.freq_bands = [
                (0, 50), (50, 100), (100, 200), (200, 400)
            ]


class FeatureExtractor:
    """
    企业级特征提取器。

    从原始振动数据中提取:
    - 时域特征 (RMS, 峰值, 波峰因子, 偏度, 峰度等)
    - 频域特征 (主频, 频带能量, 谐波失真等)
    - 统计特征 (均值, 方差, 分位数等)

    输入: (N, features) 原始数据数组
    输出: (M, 24) 特征矩阵，M = 滑动窗口数
    """

    def __init__(self, config: Optional[FeatureConfig] = None):
        self.config = config or FeatureConfig()
        self._scaler_params: Optional[Dict] = None

    def extract_features(self, df: pd.DataFrame) -> pd.DataFrame:
        """
        从DataFrame提取完整特征矩阵。

        Args:
            df: 原始数据DataFrame，包含 rms_x/y/z, overall_rms,
                peak_freq, peak_amp, temperature_c, humidity_rh 等列

        Returns:
            特征DataFrame，每行为一个滑动窗口的特征向量
        """
        logger.info("Starting feature extraction: window=%d, overlap=%d",
                    self.config.window_size, self.config.overlap)

        feature_rows = []
        step = self.config.window_size - self.config.overlap
        n = len(df)

        sensor_cols = ['rms_x', 'rms_y', 'rms_z', 'overall_rms',
                       'peak_freq', 'peak_amp']
        env_cols = ['temperature_c', 'humidity_rh']

        available_sensor_cols = [c for c in sensor_cols if c in df.columns]
        available_env_cols = [c for c in env_cols if c in df.columns]

        for start in range(0, n - self.config.window_size + 1, step):
            end = start + self.config.window_size
            window = df.iloc[start:end]
            row = {}

            row['window_start_ms'] = int(window['timestamp_ms'].iloc[0])
            row['window_end_ms'] = int(window['timestamp_ms'].iloc[-1])
            row['dev_id'] = int(window['dev_id'].iloc[0])

            sensor_data = window[available_sensor_cols].values

            time_features = self._extract_time_domain_features(sensor_data)
            row.update(time_features)

            if self.config.enable_frequency_features:
                freq_features = self._extract_frequency_features(sensor_data)
                row.update(freq_features)

            if self.config.enable_statistical_features:
                stat_features = self._extract_statistical_features(sensor_data)
                row.update(stat_features)

            for col in available_env_cols:
                row[col] = float(window[col].mean())

            if 'label' in df.columns:
                row['label'] = window['label'].mode()[0]
            else:
                row['label'] = 'unknown'

            feature_rows.append(row)

        result = pd.DataFrame(feature_rows)
        logger.info("Feature extraction complete: %d windows → %d features",
                    len(feature_rows), len(result.columns))
        return result

    def _extract_time_domain_features(
        self, data: np.ndarray
    ) -> Dict[str, float]:
        """
        提取时域特征。

        Args:
            data: (window_size, num_channels) 数组

        Returns:
            时域特征字典
        """
        features = {}

        channel_names = ['x', 'y', 'z', 'overall']

        for i in range(min(data.shape[1], 4)):
            ch = data[:, i]
            prefix = channel_names[i]

            features[f'rms_{prefix}'] = float(np.sqrt(np.mean(np.square(ch))))
            features[f'peak_{prefix}'] = float(np.max(np.abs(ch)))
            features[f'mean_{prefix}'] = float(np.mean(ch))

            rms_val = features[f'rms_{prefix}']
            if rms_val > 1e-10:
                features[f'crest_factor_{prefix}'] = float(
                    features[f'peak_{prefix}'] / rms_val
                )
            else:
                features[f'crest_factor_{prefix}'] = 0.0

            mean_val = features[f'mean_{prefix}']
            std_val = float(np.std(ch))
            if std_val > 1e-10:
                centered = ch - mean_val
                features[f'skewness_{prefix}'] = float(
                    np.mean(centered ** 3) / (std_val ** 3)
                )
                features[f'kurtosis_{prefix}'] = float(
                    np.mean(centered ** 4) / (std_val ** 4)
                )
            else:
                features[f'skewness_{prefix}'] = 0.0
                features[f'kurtosis_{prefix}'] = 0.0

        return features

    def _extract_frequency_features(
        self, data: np.ndarray
    ) -> Dict[str, float]:
        """
        提取频域特征。

        Args:
            data: (window_size, num_channels) 数组

        Returns:
            频域特征字典
        """
        features = {}

        for i in range(min(data.shape[1], 4)):
            prefix = ['x', 'y', 'z', 'overall'][i]
            ch = data[:, i]

            fft_vals = np.abs(np.fft.rfft(ch))
            freq_bins = np.fft.rfftfreq(
                len(ch), d=1.0/self.config.sampling_rate_hz
            )

            peak_idx = np.argmax(fft_vals[1:]) + 1
            features[f'dominant_freq_{prefix}'] = float(freq_bins[peak_idx])
            features[f'dominant_amp_{prefix}'] = float(fft_vals[peak_idx])

            total_power = float(np.sum(fft_vals ** 2))

            for band_low, band_high in self.config.freq_bands:
                band_mask = (freq_bins >= band_low) & (freq_bins < band_high)
                band_power = float(np.sum(fft_vals[band_mask] ** 2))
                band_name = f'freq_band_{int(band_low)}_{int(band_high)}'
                if total_power > 1e-10:
                    features[f'{band_name}_{prefix}'] = band_power / total_power
                else:
                    features[f'{band_name}_{prefix}'] = 0.0

        return features

    def _extract_statistical_features(
        self, data: np.ndarray
    ) -> Dict[str, float]:
        """
        提取统计特征。

        Args:
            data: (window_size, num_channels) 数组

        Returns:
            统计特征字典
        """
        features = {}

        flat = data.flatten()
        features['overall_mean'] = float(np.mean(flat))
        features['overall_std'] = float(np.std(flat))
        features['overall_max'] = float(np.max(np.abs(flat)))
        features['overall_q95'] = float(np.percentile(np.abs(flat), 95))
        features['zero_crossing_rate'] = float(
            np.mean(np.abs(np.diff(np.signbit(data[:, 0]).astype(int)))) / 2
        )
        features['energy'] = float(np.sum(np.square(flat)))

        return features

    def extract_single(self, data: np.ndarray) -> np.ndarray:
        """
        从单个窗口数据提取特征向量 (用于实时推理)。

        Args:
            data: (window_size, num_channels) 数组

        Returns:
            特征向量 (24,)
        """
        all_features = {}
        all_features.update(self._extract_time_domain_features(data))
        all_features.update(self._extract_frequency_features(data))
        all_features.update(self._extract_statistical_features(data))

        sorted_keys = sorted(all_features.keys())
        return np.array([all_features[k] for k in sorted_keys], dtype=np.float32)

    def fit_scaler(self, df: pd.DataFrame):
        """从数据拟合归一化参数"""
        feature_cols = [c for c in df.columns
                       if c.startswith(('rms_', 'peak_', 'mean_',
                                        'crest_', 'skewness_', 'kurtosis_',
                                        'dominant_', 'freq_band_',
                                        'overall_', 'zero_', 'energy'))]
        if not feature_cols:
            self._scaler_params = {'mean': 0, 'std': 1}
            return

        data = df[feature_cols].values
        self._scaler_params = {
            'mean': data.mean(axis=0).tolist(),
            'std': np.clip(data.std(axis=0), 1e-10, None).tolist(),
            'feature_names': feature_cols
        }
        logger.info("Scaler fitted on %d features", len(feature_cols))

    def transform(self, df: pd.DataFrame) -> pd.DataFrame:
        """使用已拟合的归一化参数变换数据"""
        if self._scaler_params is None:
            logger.warning("Scaler not fitted, returning raw data")
            return df

        feature_names = self._scaler_params['feature_names']
        available = [c for c in feature_names if c in df.columns]

        scaled = df.copy()
        data = scaled[available].values
        mean = np.array(self._scaler_params['mean'][:len(available)])
        std = np.array(self._scaler_params['std'][:len(available)])

        scaled[available] = (data - mean) / std
        return scaled

    def fit_transform(self, df: pd.DataFrame) -> pd.DataFrame:
        """拟合并变换数据"""
        self.fit_scaler(df)
        return self.transform(df)
