"""Camera wrapper around cv2.VideoCapture. NOT thread-safe — use only from main thread."""

import structlog
from typing import Optional

import cv2
import numpy as np

from src.config import CaptureConfig

logger = structlog.get_logger(__name__)


class CameraCapture:
    """Manages a single cv2.VideoCapture instance for one camera index."""

    def __init__(self, camera_index: int, cfg: CaptureConfig):
        self._index = camera_index
        self._cfg = cfg
        self._cap: Optional[cv2.VideoCapture] = None

    def open(self) -> bool:
        try:
            self._cap = cv2.VideoCapture(self._index)
            if not self._cap.isOpened():
                logger.warning("camera open failed", camera_index=self._index)
                return False
            logger.info("camera opened", camera_index=self._index)
            return True
        except Exception:
            logger.exception("camera open exception", camera_index=self._index)
            return False

    def release(self):
        if self._cap is not None:
            self._cap.release()
            self._cap = None

    def is_open(self) -> bool:
        return self._cap is not None and self._cap.isOpened()

    def read_frame(self, resolution: tuple[int, int]) -> Optional[np.ndarray]:
        """Capture and resize a frame. Returns (H, W, 3) BGR array or None."""
        if not self.is_open():
            logger.warning("camera not open on read", camera_index=self._index)
            return None
        ret, frame = self._cap.read()
        if not ret or frame is None:
            logger.warning("camera read returned empty frame",
                           camera_index=self._index)
            return None
        w, h = resolution
        if frame.shape[1] != w or frame.shape[0] != h:
            frame = cv2.resize(frame, (w, h))
        return frame

    def encode_jpeg(self, frame: np.ndarray, quality: int) -> bytes:
        params = [cv2.IMWRITE_JPEG_QUALITY, quality]
        success, buf = cv2.imencode(".jpg", frame, params)
        if not success:
            raise RuntimeError("cv2.imencode failed")
        return buf.tobytes()
