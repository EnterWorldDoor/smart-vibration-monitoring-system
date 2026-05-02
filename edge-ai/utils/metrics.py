import numpy as np
from typing import Dict, List, Optional

from .logger import get_logger

logger = get_logger(__name__)


def compute_classification_metrics(
    y_true: np.ndarray,
    y_pred: np.ndarray,
    y_prob: Optional[np.ndarray] = None,
    class_names: Optional[List[str]] = None
) -> Dict:
    """
    计算分类模型完整评估指标。

    Args:
        y_true: 真实标签
        y_pred: 预测标签
        y_prob: 预测概率 (N, classes)
        class_names: 类别名称列表

    Returns:
        包含 accuracy, precision, recall, f1, confusion_matrix 的字典
    """
    from sklearn.metrics import (
        accuracy_score, precision_score, recall_score,
        f1_score, confusion_matrix, roc_auc_score
    )

    metrics = {}

    metrics['accuracy'] = float(accuracy_score(y_true, y_pred))

    metrics['precision_macro'] = float(
        precision_score(y_true, y_pred, average='macro', zero_division=0)
    )
    metrics['recall_macro'] = float(
        recall_score(y_true, y_pred, average='macro', zero_division=0)
    )
    metrics['f1_macro'] = float(
        f1_score(y_true, y_pred, average='macro', zero_division=0)
    )

    metrics['precision_weighted'] = float(
        precision_score(y_true, y_pred, average='weighted', zero_division=0)
    )
    metrics['recall_weighted'] = float(
        recall_score(y_true, y_pred, average='weighted', zero_division=0)
    )
    metrics['f1_weighted'] = float(
        f1_score(y_true, y_pred, average='weighted', zero_division=0)
    )

    cm = confusion_matrix(y_true, y_pred)
    metrics['confusion_matrix'] = cm.tolist()

    if y_prob is not None and y_prob.shape[1] > 1:
        try:
            metrics['roc_auc_ovr'] = float(
                roc_auc_score(y_true, y_prob, multi_class='ovr', average='macro')
            )
        except ValueError:
            metrics['roc_auc_ovr'] = 0.0

    if class_names:
        per_class = {}
        for i, name in enumerate(class_names):
            tp = cm[i, i]
            fp = cm[:, i].sum() - tp
            fn = cm[i, :].sum() - tp
            per_class[name] = {
                'precision': float(tp / (tp + fp)) if (tp + fp) > 0 else 0.0,
                'recall': float(tp / (tp + fn)) if (tp + fn) > 0 else 0.0,
                'f1': float(2 * tp / (2 * tp + fp + fn)) if (2 * tp + fp + fn) > 0 else 0.0,
                'support': int(cm[i, :].sum())
            }
        metrics['per_class'] = per_class

    return metrics


def compute_anomaly_detection_metrics(
    y_true: np.ndarray,
    anomaly_scores: np.ndarray,
    threshold: Optional[float] = None
) -> Dict:
    """
    计算异常检测评估指标。

    Args:
        y_true: 真实异常标签 (1=异常, 0=正常)
        anomaly_scores: 异常分数数组
        threshold: 判定阈值，None时自动选择最优

    Returns:
        包含 auc, best_threshold, precision, recall, f1 的字典
    """
    from sklearn.metrics import (
        roc_auc_score, precision_recall_curve, auc
    )

    metrics = {}

    metrics['auc'] = float(roc_auc_score(y_true, anomaly_scores))

    precision_curve, recall_curve, thresholds = precision_recall_curve(
        y_true, anomaly_scores
    )
    metrics['pr_auc'] = float(auc(recall_curve, precision_curve))

    if threshold is None:
        f1_scores = 2 * (precision_curve * recall_curve) / (
            precision_curve + recall_curve + 1e-10
        )
        best_idx = np.argmax(f1_scores)
        threshold = float(thresholds[best_idx]) if best_idx < len(thresholds) else 0.5

    metrics['threshold'] = float(threshold)

    y_pred = (anomaly_scores >= threshold).astype(int)
    metrics['precision'] = float(precision_curve[0])
    metrics['recall'] = float(recall_curve[0])
    metrics['f1'] = float(
        2 * metrics['precision'] * metrics['recall'] /
        (metrics['precision'] + metrics['recall'] + 1e-10)
    )

    return metrics


def compute_psi(
    expected: np.ndarray,
    actual: np.ndarray,
    bins: int = 10
) -> float:
    """
    计算Population Stability Index (PSI)用于数据漂移检测。

    PSI < 0.1: 无漂移
    0.1 <= PSI < 0.25: 轻微漂移
    PSI >= 0.25: 显著漂移

    Args:
        expected: 参考分布数据
        actual: 当前分布数据
        bins: 分箱数量

    Returns:
        PSI值
    """
    expected_hist, bin_edges = np.histogram(expected, bins=bins, density=True)
    actual_hist, _ = np.histogram(actual, bins=bin_edges, density=True)

    expected_hist = np.clip(expected_hist, 1e-10, None)
    actual_hist = np.clip(actual_hist, 1e-10, None)

    psi = np.sum((actual_hist - expected_hist) * np.log(actual_hist / expected_hist))
    return float(psi)
