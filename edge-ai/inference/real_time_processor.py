import numpy as np
import time
import json
from typing import Callable, Dict, Optional, Tuple

from .inference_engine import InferenceEngine
from utils.logger import get_logger

logger = get_logger(__name__)


class RealTimeProcessor:
    """
    企业级实时推理处理器 (PC端训练数据采集验证用)。

    功能:
    - 流式数据窗口缓冲区管理
    - 定时推理调度 (PC端验证)
    - MQTT消息接收与结果发布
    - 推理结果回调分发

    注意:
    - 本模块运行在PC端 Anaconda 环境，用于模型训练阶段的数据采集
    - ESP32端的实时推理由 ai_service.c + TFLite Micro 实现
    - Raspberry Pi端的实时推理为未来规划
    """

    def __init__(
        self,
        engine: InferenceEngine,
        window_size: int = 256,
        step_size: int = 128,
        input_channels: int = 6
    ):
        self.engine = engine
        self.window_size = window_size
        self.step_size = step_size
        self.input_channels = input_channels

        self._buffer: list = []
        self._callbacks: list = []
        self._running: bool = False
        self._last_inference_time: float = 0.0
        self._inference_count: int = 0
        self._processor_stats = {
            'total_samples_received': 0,
            'total_inferences': 0,
            'dropped_samples': 0,
            'max_buffer_size': window_size * 2
        }

    def ingest(self, data: np.ndarray, feature_dict: Optional[Dict] = None):
        """
        接收新数据点。

        Args:
            data: 单帧数据 (input_channels,) 或 (batch, input_channels)
            feature_dict: 对应的特征字典
        """
        if data.ndim == 1:
            data = np.expand_dims(data, axis=0)

        for i in range(len(data)):
            self._buffer.append(data[i].copy())
            self._processor_stats['total_samples_received'] += 1

        while len(self._buffer) >= self.window_size:
            window = np.array(self._buffer[:self.window_size], dtype=np.float32)
            self._buffer = self._buffer[self.step_size:]

            try:
                result = self.engine.infer(window, feature_dict)
                self._inference_count += 1
                self._processor_stats['total_inferences'] += 1
                self._last_inference_time = time.time()

                for callback in self._callbacks:
                    try:
                        callback(result)
                    except Exception as e:
                        logger.error("Callback error: %s", e)

            except Exception as e:
                logger.error("Inference error: %s", e)

        overflow = len(self._buffer) - self._processor_stats['max_buffer_size']
        if overflow > 0:
            self._buffer = self._buffer[-self.window_size:]
            self._processor_stats['dropped_samples'] += overflow

    def register_callback(self, callback: Callable):
        """
        注册推理结果回调。

        Args:
            callback: 接收 CascadeResult 的回调函数
        """
        self._callbacks.append(callback)

    def clear_callbacks(self):
        """清除所有回调"""
        self._callbacks.clear()

    def get_stats(self) -> Dict:
        """获取处理统计"""
        return {
            **self._processor_stats,
            'buffer_size': len(self._buffer),
            'buffer_capacity_pct': len(self._buffer) / self.window_size * 100,
            'inference_count': self._inference_count,
            'seconds_since_last_inference':
                time.time() - self._last_inference_time if self._last_inference_time > 0 else -1
        }

    def reset(self):
        """重置处理器"""
        self._buffer.clear()
        self._inference_count = 0
        self._last_inference_time = 0.0
        for k in self._processor_stats:
            if k not in ('max_buffer_size',):
                self._processor_stats[k] = 0


class MQTTInferenceBridge:
    """
    MQTT训练数据采集桥接器 (PC端 Anaconda 环境)。

    连接 MQTT Broker，实现:
    - 从 edgevib/train/# 接收 ESP32 训练模式数据
    - 推送到推理引擎进行PC端验证
    - 将结果发布到 edgevib/inference/{dev_id}/result
    - 同时用于 csv 训练集采集 (mqtt_subscriber.py)

    注意: ESP32端推理不经过此桥接器。
    """

    def __init__(
        self,
        engine: InferenceEngine,
        broker_host: str = 'localhost',
        broker_port: int = 1883,
        input_topic: str = 'edgevib/train/#',
        output_topic_prefix: str = 'edgevib/inference'
    ):
        self.engine = engine
        self.broker_host = broker_host
        self.broker_port = broker_port
        self.input_topic = input_topic
        self.output_topic_prefix = output_topic_prefix

        self.processor = RealTimeProcessor(engine)
        self.processor.register_callback(self._on_inference_result)

        self._client = None
        logger.info(
            "MQTTInferenceBridge initialized: %s:%d → %s",
            broker_host, broker_port, input_topic
        )

    def start(self):
        """启动MQTT监听和推理桥接"""
        try:
            import paho.mqtt.client as mqtt

            self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
            self._client.on_connect = self._on_mqtt_connect
            self._client.on_message = self._on_mqtt_message
            self._client.on_disconnect = self._on_mqtt_disconnect

            self._client.connect_async(self.broker_host, self.broker_port)
            self._client.loop_start()
            logger.info("MQTT bridge started")
        except Exception as e:
            logger.error("Failed to start MQTT bridge: %s", e)
            raise

    def stop(self):
        """停止MQTT桥接"""
        if self._client:
            self._client.loop_stop()
            self._client.disconnect()
            logger.info("MQTT bridge stopped")

    def _on_mqtt_connect(self, client, userdata, flags, reason_code, properties):
        logger.info("MQTT connected to %s:%d (reason=%d)", self.broker_host, self.broker_port, reason_code)
        client.subscribe(self.input_topic, qos=1)

    def _on_mqtt_message(self, client, userdata, msg):
        try:
            data = json.loads(msg.payload.decode('utf-8'))

            sensor_cols = ['rms_x', 'rms_y', 'rms_z',
                          'overall_rms', 'peak_freq', 'peak_amp']
            feature_dict = {
                k: float(data.get(k, 0))
                for k in ['overall_rms', 'peak_freq',
                          'temperature_c', 'humidity_rh']
            }

            sensor_vec = np.array([float(data.get(c, 0)) for c in sensor_cols],
                                 dtype=np.float32)
            self.processor.ingest(sensor_vec, feature_dict)

        except Exception as e:
            logger.error("MQTT message processing error: %s", e)

    def _on_mqtt_disconnect(self, client, userdata, flags, reason_code, properties):
        logger.warning("MQTT disconnected (reason=%d)", reason_code)

    def _on_inference_result(self, result):
        if not self._client or not self._client.is_connected():
            return

        payload = json.dumps({
            'timestamp': int(time.time() * 1000),
            'prediction': result.prediction,
            'predicted_class': result.predicted_class,
            'confidence': result.confidence,
            'model_used': result.model_used,
            'inference_time_ms': result.inference_time_ms,
            'diagnosis': result.diagnosis
        })

        topic = f"{self.output_topic_prefix}/result"
        self._client.publish(topic, payload, qos=1)
