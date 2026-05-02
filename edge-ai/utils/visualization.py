import numpy as np
from typing import Dict, List, Optional
from pathlib import Path

from .logger import get_logger

logger = get_logger(__name__)


def plot_confusion_matrix(
    cm: np.ndarray,
    class_names: List[str],
    output_path: Optional[str] = None,
    title: str = "Confusion Matrix"
):
    """
    绘制混淆矩阵热力图。

    Args:
        cm: 混淆矩阵 (N, N)
        class_names: 类别名称列表
        output_path: 输出图片路径，None则显示
        title: 图表标题
    """
    import matplotlib.pyplot as plt
    import seaborn as sns

    fig, ax = plt.subplots(figsize=(8, 6))
    sns.heatmap(
        cm, annot=True, fmt='d', cmap='Blues',
        xticklabels=class_names, yticklabels=class_names,
        ax=ax
    )
    ax.set_xlabel('Predicted Label')
    ax.set_ylabel('True Label')
    ax.set_title(title)

    plt.tight_layout()

    if output_path:
        Path(output_path).parent.mkdir(parents=True, exist_ok=True)
        plt.savefig(output_path, dpi=150, bbox_inches='tight')
        logger.info("Confusion matrix saved to %s", output_path)
    else:
        plt.show()

    plt.close(fig)


def plot_training_history(
    history: Dict,
    output_path: Optional[str] = None
):
    """
    绘制训练历史曲线。

    Args:
        history: Keras History.history 字典
        output_path: 输出图片路径
    """
    import matplotlib.pyplot as plt

    metrics_to_plot = [k for k in history.keys() if not k.startswith('val_')]
    n_metrics = len(metrics_to_plot)

    if n_metrics == 0:
        return

    fig, axes = plt.subplots(1, n_metrics, figsize=(6 * n_metrics, 4))
    if n_metrics == 1:
        axes = [axes]

    for i, metric in enumerate(metrics_to_plot):
        axes[i].plot(history[metric], label=f'Train {metric}')
        val_key = f'val_{metric}'
        if val_key in history:
            axes[i].plot(history[val_key], label=f'Val {metric}')
        axes[i].set_xlabel('Epoch')
        axes[i].set_ylabel(metric)
        axes[i].set_title(f'Training {metric}')
        axes[i].legend()
        axes[i].grid(True, alpha=0.3)

    plt.tight_layout()

    if output_path:
        Path(output_path).parent.mkdir(parents=True, exist_ok=True)
        plt.savefig(output_path, dpi=150, bbox_inches='tight')
        logger.info("Training history saved to %s", output_path)
    else:
        plt.show()

    plt.close(fig)


def plot_anomaly_scores(
    scores: np.ndarray,
    threshold: float,
    labels: Optional[np.ndarray] = None,
    output_path: Optional[str] = None
):
    """
    绘制异常分数分布图。

    Args:
        scores: 异常分数数组
        threshold: 判定阈值
        labels: 真实标签 (可选)
        output_path: 输出路径
    """
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(figsize=(10, 5))

    if labels is not None:
        normal_scores = scores[labels == 0]
        anomaly_scores = scores[labels == 1]
        ax.hist(normal_scores, bins=50, alpha=0.6, label='Normal', color='green')
        ax.hist(anomaly_scores, bins=50, alpha=0.6, label='Anomaly', color='red')
    else:
        ax.hist(scores, bins=50, alpha=0.7, label='All samples')

    ax.axvline(x=threshold, color='red', linestyle='--',
               linewidth=2, label=f'Threshold={threshold:.4f}')
    ax.set_xlabel('Anomaly Score')
    ax.set_ylabel('Count')
    ax.set_title('Anomaly Score Distribution')
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()

    if output_path:
        Path(output_path).parent.mkdir(parents=True, exist_ok=True)
        plt.savefig(output_path, dpi=150, bbox_inches='tight')
        logger.info("Anomaly score plot saved to %s", output_path)
    else:
        plt.show()

    plt.close(fig)
