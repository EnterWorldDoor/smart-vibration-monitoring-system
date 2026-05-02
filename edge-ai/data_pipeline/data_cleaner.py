import numpy as np
import pandas as pd
from typing import Optional, Tuple

from utils.logger import get_logger

logger = get_logger(__name__)


class DataCleaner:
    """
    企业级数据清洗器。

    功能：
    - 去重（基于时间戳）
    - 缺失值填充（线性插值/均值）
    - 异常值检测（Z-Score / IQR）
    - 数据类型规范化
    - 数据质量报告生成
    """

    REQUIRED_COLUMNS = [
        'timestamp_ms', 'dev_id',
        'rms_x', 'rms_y', 'rms_z', 'overall_rms',
        'peak_freq', 'peak_amp',
        'temperature_c', 'humidity_rh'
    ]

    VALUE_RANGES = {
        'rms_x': (-10.0, 10.0),
        'rms_y': (-10.0, 10.0),
        'rms_z': (-10.0, 10.0),
        'overall_rms': (0.0, 20.0),
        'peak_freq': (0.0, 2000.0),
        'peak_amp': (0.0, 20.0),
        'temperature_c': (-40.0, 125.0),
        'humidity_rh': (0.0, 100.0)
    }

    def __init__(self, zscore_threshold: float = 3.0):
        self.zscore_threshold = zscore_threshold
        self.stats = {}

    def clean(self, df: pd.DataFrame) -> pd.DataFrame:
        """
        执行完整数据清洗流水线。

        Args:
            df: 原始数据DataFrame

        Returns:
            清洗后的DataFrame
        """
        initial_count = len(df)
        logger.info("Starting data cleaning: %d initial records", initial_count)

        cleaned = df.copy()
        cleaned = self._validate_columns(cleaned)
        cleaned = self._parse_types(cleaned)
        cleaned = self._remove_duplicates(cleaned)
        cleaned = self._handle_missing_values(cleaned)
        cleaned = self._filter_out_of_range(cleaned)
        cleaned = self._detect_outliers(cleaned)
        cleaned = cleaned.reset_index(drop=True)

        final_count = len(cleaned)
        self.stats = {
            'initial_count': initial_count,
            'final_count': final_count,
            'removed_count': initial_count - final_count,
            'completeness': final_count / max(initial_count, 1)
        }

        logger.info(
            "Data cleaning complete: %d → %d records (%.1f%% retained)",
            initial_count, final_count, self.stats['completeness'] * 100
        )
        return cleaned

    def _validate_columns(self, df: pd.DataFrame) -> pd.DataFrame:
        """验证必填列是否存在"""
        missing = [c for c in self.REQUIRED_COLUMNS if c not in df.columns]
        if missing:
            logger.error("Missing required columns: %s", missing)
            raise ValueError(f"Missing required columns: {missing}")
        return df

    def _parse_types(self, df: pd.DataFrame) -> pd.DataFrame:
        """规范化数据类型"""
        int_cols = ['timestamp_ms', 'dev_id']
        float_cols = ['rms_x', 'rms_y', 'rms_z', 'overall_rms',
                      'peak_freq', 'peak_amp', 'temperature_c', 'humidity_rh']

        for col in int_cols:
            if col in df.columns:
                df[col] = pd.to_numeric(df[col], errors='coerce').fillna(0).astype(int)

        for col in float_cols:
            if col in df.columns:
                df[col] = pd.to_numeric(df[col], errors='coerce').astype(float)

        return df

    def _remove_duplicates(self, df: pd.DataFrame) -> pd.DataFrame:
        """移除重复记录"""
        before = len(df)
        df = df.drop_duplicates(subset=['timestamp_ms', 'dev_id'], keep='last')
        removed = before - len(df)
        if removed > 0:
            logger.info("Removed %d duplicate records", removed)
        return df

    def _handle_missing_values(self, df: pd.DataFrame) -> pd.DataFrame:
        """处理缺失值"""
        null_counts = df.isnull().sum()
        null_cols = null_counts[null_counts > 0]

        if len(null_cols) == 0:
            return df

        logger.warning("Missing values detected: %s", null_cols.to_dict())

        for col in null_cols.index:
            missing_ratio = null_counts[col] / len(df)
            if missing_ratio > 0.3:
                logger.warning(
                    "Column %s has %.1f%% missing, filling with median",
                    col, missing_ratio * 100
                )
                df[col] = df[col].fillna(df[col].median())
            else:
                df[col] = df[col].interpolate(method='linear', limit_direction='both')

        return df

    def _filter_out_of_range(self, df: pd.DataFrame) -> pd.DataFrame:
        """过滤超出物理范围的值"""
        for col, (low, high) in self.VALUE_RANGES.items():
            if col not in df.columns:
                continue
            mask = (df[col] >= low) & (df[col] <= high)
            out_of_range = (~mask).sum()
            if out_of_range > 0:
                logger.warning(
                    "Column %s: %d values out of range [%.1f, %.1f], clipping",
                    col, out_of_range, low, high
                )
                df[col] = df[col].clip(low, high)

        return df

    def _detect_outliers(self, df: pd.DataFrame) -> pd.DataFrame:
        """
        使用Z-Score检测异常值。

        只标记不删除，添加 is_outlier 列。
        """
        outlier_mask = pd.Series(False, index=df.index)

        numeric_cols = ['rms_x', 'rms_y', 'rms_z', 'overall_rms',
                       'peak_freq', 'peak_amp']

        for col in numeric_cols:
            if col not in df.columns:
                continue
            mean = df[col].mean()
            std = df[col].std()
            if std < 1e-10:
                continue
            z_scores = np.abs((df[col] - mean) / std)
            outlier_mask |= (z_scores > self.zscore_threshold)

        df['is_outlier'] = outlier_mask.astype(int)
        outlier_count = outlier_mask.sum()
        if outlier_count > 0:
            logger.info(
                "Detected %d outlier records (%.1f%%)",
                outlier_count, outlier_count / len(df) * 100
            )

        return df

    def generate_report(self) -> dict:
        """生成数据质量报告"""
        return {
            'cleaning_stats': self.stats,
            'completeness_percent': self.stats.get('completeness', 0) * 100,
            'status': 'pass' if self.stats.get('completeness', 0) > 0.99 else 'review'
        }
