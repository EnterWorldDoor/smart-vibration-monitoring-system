"""ONNX Runtime Autoencoder — anomaly detection via reconstruction error."""

import json
from pathlib import Path
from typing import Optional

import numpy as np
import structlog

logger = structlog.get_logger(__name__)


class Autoencoder:
    def __init__(self, model_path: str, metadata_path: str):
        self.model_path = Path(model_path)
        self.metadata = self._load_metadata(metadata_path)
        self._session = None
        self._threshold: float = self.metadata.get("anomaly_threshold", 0.15)
        logger.info("autoencoder config", threshold=self._threshold,
                    input_dim=self.metadata.get("input_dim", 24))

    @staticmethod
    def _load_metadata(path: str) -> dict:
        with open(path) as f:
            return json.load(f)

    def load(self):
        import onnxruntime as ort
        if not self.model_path.exists():
            raise FileNotFoundError(f"ONNX model not found: {self.model_path}")
        self._session = ort.InferenceSession(
            str(self.model_path),
            providers=["CPUExecutionProvider"],
        )
        logger.info("onnx session loaded", model=str(self.model_path.name))

    def is_loaded(self) -> bool:
        return self._session is not None

    def infer(self, X: np.ndarray) -> np.ndarray:
        """Run inference: X (N, 24) → reconstructed (N, 24)."""
        if self._session is None:
            raise RuntimeError("Model not loaded")
        input_name = self._session.get_inputs()[0].name
        return self._session.run(None, {input_name: X})[0]

    def anomaly_scores(self, X: np.ndarray) -> np.ndarray:
        """Compute reconstruction MSE for each sample."""
        X_recon = self.infer(X)
        return np.mean(np.square(X - X_recon), axis=1)

    def detect(self, X: np.ndarray, threshold: Optional[float] = None) -> np.ndarray:
        """Detect anomalies: returns boolean array (True = anomaly)."""
        scores = self.anomaly_scores(X)
        return scores > (threshold or self._threshold)

    def get_threshold(self) -> float:
        return self._threshold
