"""
EdgeVib Edge-AI - 数据采集模块

本模块提供从MQTT Broker接收ESP32传感器数据并转换为CSV格式的功能。

主要组件:
  - MqttDataSubscriber: MQTT订阅器 (核心类)
  - DataValidator: 数据验证器
  - CSVWriter: CSV写入器

使用示例:
  from data_collection.mqtt_subscriber import MqttDataSubscriber
  
  sub = MqttDataSubscriber(
      broker_host="192.168.1.100",
      output_file="training_data.csv"
  )
  sub.start()  # 阻塞运行, Ctrl-C退出
"""

__version__ = "1.0.0"
__author__ = "EnterWorldDoor"

from .mqtt_subscriber import MqttDataSubscriber, SensorRecord, Statistics

__all__ = [
    'MqttDataSubscriber',
    'SensorRecord', 
    'Statistics',
]
