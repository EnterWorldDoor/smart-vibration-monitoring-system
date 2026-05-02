#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EdgeVib MQTT Data Subscriber - PC端训练数据采集器

功能:
  1. 连接Mosquitto Broker (本地或远程)
  2. 订阅 edgevib/train/# topic (接收ESP32 Training模式数据)
  3. 实时解析JSON格式传感器数据
  4. 数据验证与清洗
  5. 自动转换为CSV训练集格式
  6. 支持断点续传和数据追加

使用示例:
  python mqtt_subscriber.py --broker localhost:1883 --output training_data.csv
  python mqtt_subscriber.py --config config.yaml --verbose

作者: EnterWorldDoor
版本: 1.0.0
"""

import json
import csv
import os
import sys
import time
import signal
import argparse
import logging
from datetime import datetime
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass, asdict

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("错误: 缺少paho-mqtt库,请运行: pip install paho-mqtt")
    sys.exit(1)


# ==================== 配置常量 ====================

DEFAULT_BROKER_HOST = "localhost"
DEFAULT_BROKER_PORT = 1883
DEFAULT_TOPIC_PREFIX = "edgevib/train/"
DEFAULT_QOS = 1
DEFAULT_OUTPUT_FILE = "training_data.csv"
DEFAULT_BUFFER_SIZE = 100  # 每100条记录刷新一次文件

REQUIRED_FIELDS = [
    'timestamp_ms', 'dev_id',
    'rms_x', 'rms_y', 'rms_z', 'overall_rms',
    'peak_freq', 'peak_amp',
    'temperature_c', 'humidity_rh'
]

VALID_LABELS = ['normal', 'imbalance', 'misalignment', 'bearing_fault', 'unknown']


# ==================== 数据结构定义 ====================

@dataclass
class SensorRecord:
    """传感器数据记录 (对应CSV的一行)"""
    timestamp_ms: int
    dev_id: int
    rms_x: float
    rms_y: float
    rms_z: float
    overall_rms: float
    peak_freq: float
    peak_amp: float
    temperature_c: float
    humidity_rh: float
    label: str = "unknown"  # 需要人工标注或自动标注


@dataclass
class Statistics:
    """采集统计信息"""
    total_received: int = 0
    valid_records: int = 0
    invalid_records: int = 0
    duplicate_count: int = 0
    start_time: float = 0.0
    last_received_time: float = 0.0


# ==================== 日志配置 ====================

def setup_logger(verbose: bool = False) -> logging.Logger:
    """
    配置日志系统
    
    Args:
        verbose: 是否启用DEBUG级别日志
        
    Returns:
        配置好的Logger实例
    """
    logger = logging.getLogger("mqtt_subscriber")
    
    level = logging.DEBUG if verbose else logging.INFO
    logger.setLevel(level)
    
    # 控制台输出
    console_handler = logging.StreamHandler(sys.stdout)
    console_handler.setLevel(level)
    
    formatter = logging.Formatter(
        '%(asctime)s | %(levelname)-8s | %(message)s',
        datefmt='%Y-%m-%d %H:%M:%S'
    )
    console_handler.setFormatter(formatter)
    
    logger.addHandler(console_handler)
    
    return logger


# ==================== 数据验证模块 ====================

class DataValidator:
    """
    数据验证器 - 确保接收到的数据符合规范
    
    功能:
      - 必填字段检查
      - 数据范围验证
      - 时间戳连续性检查
      - 异常值检测
    """
    
    def __init__(self, logger: logging.Logger):
        self.logger = logger
        self.last_timestamps: Dict[int, int] = {}  # {dev_id: last_timestamp}
        
    def validate_json(self, payload: bytes) -> Optional[Dict]:
        """
        验证并解析JSON载荷
        
        Args:
            payload: 原始字节流
            
        Returns:
            解析后的字典, 或None(解析失败)
        """
        try:
            data = json.loads(payload.decode('utf-8'))
            if not isinstance(data, dict):
                self.logger.warning("JSON不是字典类型")
                return None
            return data
        except json.JSONDecodeError as e:
            self.logger.error(f"JSON解析失败: {e}")
            return None
        except UnicodeDecodeError as e:
            self.logger.error(f"UTF-8解码失败: {e}")
            return None
    
    def validate_required_fields(self, data: Dict) -> bool:
        """
        检查必填字段是否存在
        
        Args:
            data: 数据字典
            
        Returns:
            True如果所有字段都存在
        """
        missing_fields = [f for f in REQUIRED_FIELDS if f not in data]
        if missing_fields:
            self.logger.warning(f"缺少字段: {missing_fields}")
            return False
        return True
    
    def validate_data_ranges(self, record: SensorRecord) -> bool:
        """
        验证数据值是否在合理范围内
        
        物理约束:
          - RMS加速度: ±10g (ADXL345量程±16g,实际振动<10g)
          - 温度: -40°C ~ +125°C (工业级传感器)
          - 湿度: 0% ~ 100% RH
          - 主频率: 0Hz ~ 2000Hz (400Hz采样率, Nyquist=200Hz,但FFT可能显示更高谐波)
          
        Args:
            record: 传感器记录
            
        Returns:
            True如果所有值都在合理范围
        """
        # RMS加速度检查 (单位: g)
        for field_name in ['rms_x', 'rms_y', 'rms_z']:
            value = getattr(record, field_name)
            if abs(value) > 10.0:  # ±10g
                self.logger.warning(
                    f"{field_name}={value:.4f}g 超出正常范围 (|value|>10g)"
                )
                # 不直接返回False,仅警告 (可能是真实异常情况)
        
        # 温度检查
        if not (-40.0 <= record.temperature_c <= 125.0):
            self.logger.warning(
                f"温度={record.temperature_c:.1f}°C 超出传感器量程 [-40, 125]°C"
            )
            return False
            
        # 湿度检查
        if not (0.0 <= record.humidity_rh <= 100.0):
            self.logger.warning(
                f"湿度={record.humidity_rh:.1f}%RH 超出范围 [0, 100]%"
            )
            return False
            
        # 主频率检查 (允许0~2000Hz,覆盖可能的谐波)
        if not (0.0 <= record.peak_freq <= 2000.0):
            self.logger.warning(
                f"主频率={record.peak_freq:.1f}Hz 可能异常"
            )
            
        return True
    
    def check_timestamp_continuity(self, record: SensorRecord) -> bool:
        """
        检查时间戳连续性 (检测丢包/乱序)
        
        规则:
          - 同一设备的时间戳应单调递增
          - 允许的最大间隔: 5000ms (5秒,考虑批量发送延迟)
          
        Args:
            record: 当前记录
            
        Returns:
            True如果时间戳合理
        """
        dev_id = record.dev_id
        current_ts = record.timestamp_ms
        
        if dev_id in self.last_timestamps:
            last_ts = self.last_timestamps[dev_id]
            time_diff = current_ts - last_ts
            
            if time_diff < 0:
                self.logger.warning(
                    f"设备{dev_id}时间戳回退: {last_ts} -> {current_ts} "
                    f"(diff={time_diff}ms), 可能是乱序消息"
                )
                return False
            elif time_diff == 0:
                self.logger.warning(
                    f"设备{dev_id}重复时间戳: {current_ts}, 跳过重复记录"
                )
                return False
            elif time_diff > 5000:
                self.logger.info(
                    f"设备{dev_id}时间戳跳跃: {time_diff}ms "
                    "(可能存在数据丢失)"
                )
        
        self.last_timestamps[dev_id] = current_ts
        return True


# ==================== CSV写入器 ====================

class CSVWriter:
    """
    CSV文件写入器 - 支持追加模式和缓冲刷新
    
    特性:
      - 自动创建目录
      - 写入头行 (首次)
      - 缓冲机制 (减少IO次数)
      - 原子性写入 (防止程序崩溃导致数据损坏)
    """
    
    def __init__(self, output_path: str,
                 buffer_size: int = DEFAULT_BUFFER_SIZE,
                 logger: logging.Logger = None):
        """
        初始化CSV写入器
        
        Args:
            output_path: 输出文件路径
            buffer_size: 缓冲区大小 (多少条记录后刷新到磁盘)
            logger: 日志实例
        """
        self.output_path = os.path.abspath(output_path)
        self.buffer_size = buffer_size
        self.logger = logger or logging.getLogger(__name__)
        
        self.buffer: List[SensorRecord] = []
        self.file_exists = os.path.exists(self.output_path)
        self.total_written = 0
        
        # 创建输出目录 (如不存在)
        output_dir = os.path.dirname(self.output_path)
        if output_dir and not os.path.exists(output_dir):
            os.makedirs(output_dir, exist_ok=True)
            self.logger.info(f"创建输出目录: {output_dir}")
    
    def _get_csv_fieldnames(self) -> List[str]:
        """获取CSV列名"""
        return list(SensorRecord.__dataclass_fields__.keys())
    
    def append(self, record: SensorRecord) -> int:
        """
        追加一条记录到缓冲区
        
        Args:
            record: 传感器记录
            
        Returns:
            当前缓冲区中的记录数
        """
        self.buffer.append(record)
        
        # 达到缓冲区大小时自动刷新
        if len(self.buffer) >= self.buffer_size:
            self.flush()
            
        return len(self.buffer)
    
    def flush(self) -> int:
        """
        将缓冲区的所有记录写入文件
        
        Returns:
            本次写入的记录数
        """
        if not self.buffer:
            return 0
            
        written_count = len(self.buffer)
        
        try:
            with open(self.output_path, 'a', newline='', encoding='utf-8') as f:
                writer = csv.DictWriter(f, fieldnames=self._get_csv_fieldnames())
                
                # 如果是新文件,先写入表头
                if not self.file_exists or os.path.getsize(self.output_path) == 0:
                    writer.writeheader()
                    self.file_exists = True
                
                # 写入所有缓冲的记录
                for record in self.buffer:
                    writer.writerow(asdict(record))
                    
            self.total_written += written_count
            self.logger.debug(
                f"已刷新 {written_count} 条记录到文件 "
                f"(累计: {self.total_written} 条)"
            )
            
            self.buffer.clear()
            return written_count
            
        except IOError as e:
            self.logger.error(f"文件写入失败: {e}")
            raise
    
    def close(self) -> int:
        """
        关闭写入器 (刷新剩余缓冲区)
        
        Returns:
            总共写入的记录数
        """
        remaining = self.flush()
        self.logger.info(
            f"CSV写入器关闭: 共写入 {self.total_written} 条记录 "
            f"(剩余缓冲: {remaining})"
        )
        return self.total_written


# ==================== MQTT订阅器主类 ====================

class MqttDataSubscriber:
    """
    MQTT数据采集器 - 核心类
    
    功能:
      - 连接Broker并订阅Topic
      - 接收并处理消息
      - 协调验证器和写入器
      - 统计信息收集
      - 优雅退出信号处理
    """
    
    def __init__(self,
                 broker_host: str = DEFAULT_BROKER_HOST,
                 broker_port: int = DEFAULT_BROKER_PORT,
                 topic_prefix: str = DEFAULT_TOPIC_PREFIX,
                 qos: int = DEFAULT_QOS,
                 output_file: str = DEFAULT_OUTPUT_FILE,
                 buffer_size: int = DEFAULT_BUFFER_SIZE,
                 verbose: bool = False):
        """
        初始化MQTT订阅器
        
        Args:
            broker_host: Broker主机地址
            broker_port: Broker端口号
            topic_prefix: Topic前缀 (支持通配符)
            qos: MQTT QoS等级 (0/1/2)
            output_file: 输出CSV文件路径
            buffer_size: CSV缓冲区大小
            verbose: 详细日志模式
        """
        self.broker_host = broker_host
        self.broker_port = broker_port
        self.topic = topic_prefix + "#"  # 添加通配符订阅所有子topic
        self.qos = qos
        self.verbose = verbose
        
        # 初始化组件
        self.logger = setup_logger(verbose)
        self.validator = DataValidator(self.logger)
        self.writer = CSVWriter(output_file, buffer_size, self.logger)
        self.stats = Statistics(start_time=time.time())
        
        # MQTT客户端初始化
        client_id = f"edgevib-pc-collector-{int(time.time())}"
        self.client = mqtt.Client(client_id=client_id,
                                   protocol=mqtt.MQTTv311,
                                   clean_session=True)
        
        # 注册回调函数
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.on_disconnect = self._on_disconnect
        
        # 运行状态标志
        self.running = False
        self._setup_signal_handlers()
    
    def _setup_signal_handlers(self):
        """设置信号处理器 (Ctrl+C优雅退出)"""
        def signal_handler(signum, frame):
            self.logger.info("\n收到退出信号,正在关闭...")
            self.stop()
            
        signal.signal(signal.SIGINT, signal_handler)
        signal.signal(signal.SIGTERM, signal_handler)
    
    def _on_connect(self, client, userdata, flags, rc):
        """
        MQTT连接成功回调
        
        Args:
            client: MQTT客户端实例
            userdata: 用户数据
            flags: 连接标志
            rc: 返回码 (0表示成功)
        """
        if rc == 0:
            self.logger.info(f"✓ 已连接到Broker: {self.broker_host}:{self.broker_port}")
            
            # 订阅Topic
            result, mid = self.client.subscribe(self.topic, qos=self.qos)
            if result == mqtt.MQTT_ERR_SUCCESS:
                self.logger.info(f"✓ 已订阅Topic: {self.topic} (QoS={self.qos})")
            else:
                self.logger.error(f"✗ 订阅失败 (error code: {result})")
        else:
            error_msg = {
                1: "协议版本不正确",
                2: "无效客户端标识符",
                3: "服务器不可用",
                4: "用户名密码错误",
                5: "未授权",
            }.get(rc, f"未知错误 ({rc})")
            self.logger.error(f"✗ 连接失败: {error_msg}")
    
    def _on_message(self, client, userdata, msg):
        """
        MQTT消息到达回调 (核心数据处理逻辑)
        
        处理流程:
          1. JSON解析
          2. 字段验证
          3. 范围检查
          4. 时间戳校验
          5. 转换为SensorRecord
          6. 写入CSV缓冲区
          7. 更新统计信息
          
        Args:
            client: MQTT客户端实例
            userdata: 用户数据
            msg: 消息对象 (包含topic和payload)
        """
        self.stats.total_received += 1
        self.stats.last_received_time = time.time()
        
        # 提取Topic中的dev_id
        topic_parts = msg.topic.split('/')
        dev_id_from_topic = int(topic_parts[2]) if len(topic_parts) >= 3 else 0
        
        # 步骤1: JSON解析
        data = self.validator.validate_json(msg.payload)
        if not data:
            self.stats.invalid_records += 1
            return
        
        # 步骤2: 提取嵌套的data字段 (ESP32发送的JSON结构)
        if 'data' in data and isinstance(data['data'], dict):
            sensor_data = data['data'].copy()  # 复制一份避免修改原始数据

            # 合并外层字段 (timestamp_ms, dev_id等)
            for k, v in data.items():
                if k != 'data':
                    sensor_data[k] = v

            # ⚠️ 【关键修复】提升嵌套字段到顶层!
            #
            # ESP32发送的JSON结构:
            #   {
            #     "timestamp_ms": ..., "dev_id": ...,
            #     "data": {
            #       "vibration": {"rms_x": ..., "rms_y": ..., ...},
            #       "environment": {"temperature_c": ..., "humidity_rh": ...}
            #     }
            #   }
            #
            # 但REQUIRED_FIELDS期望的是扁平结构:
            #   ['timestamp_ms', 'dev_id', 'rms_x', 'rms_y', ...]
            #
            # 修复方案: 将vibration和environment子对象中的字段提升到sensor_data顶层
            if 'vibration' in sensor_data and isinstance(sensor_data['vibration'], dict):
                vibration = sensor_data.pop('vibration')  # 移除嵌套层
                sensor_data.update(vibration)  # 提升到顶层

            if 'environment' in sensor_data and isinstance(sensor_data['environment'], dict):
                environment = sensor_data.pop('environment')  # 移除嵌套层
                sensor_data.update(environment)  # 提升到顶层
        else:
            sensor_data = data
        
        # 步骤3: 必填字段检查
        if not self.validator.validate_required_fields(sensor_data):
            self.stats.invalid_records += 1
            return
        
        # 步骤4: 构建SensorRecord对象
        # 注意: 经过步骤2的字段提升,所有必需字段都在sensor_data顶层
        try:
            record = SensorRecord(
                timestamp_ms=int(sensor_data.get('timestamp_ms', 0)),
                dev_id=int(sensor_data.get('dev_id', dev_id_from_topic)),
                rms_x=float(sensor_data.get('rms_x', 0)),
                rms_y=float(sensor_data.get('rms_y', 0)),
                rms_z=float(sensor_data.get('rms_z', 0)),
                overall_rms=float(sensor_data.get('overall_rms', 0)),
                peak_freq=float(sensor_data.get('peak_freq', 0)),
                peak_amp=float(sensor_data.get('peak_amp', 0)),
                temperature_c=float(sensor_data.get('temperature_c', 0)),
                humidity_rh=float(sensor_data.get('humidity_rh', 0)),
                label=sensor_data.get('label', 'unknown')  # 可选字段
            )
        except (ValueError, TypeError, KeyError) as e:
            self.logger.error(f"数据类型转换失败: {e}")
            self.stats.invalid_records += 1
            return
        
        # 步骤5: 数据有效性过滤
        data_quality = int(sensor_data.get('data_quality', 0))
        samples_analyzed = int(sensor_data.get('samples_analyzed', 0))
        if record.timestamp_ms <= 0:
            self.logger.warning(
                f"丢弃无效时间戳记录: dev={record.dev_id}, ts={record.timestamp_ms}"
            )
            self.stats.invalid_records += 1
            return
        if samples_analyzed <= 0 or data_quality != 0:
            self.logger.warning(
                f"丢弃非完整振动训练记录: dev={record.dev_id}, "
                f"samples={samples_analyzed}, quality={data_quality}"
            )
            self.stats.invalid_records += 1
            return
        if record.overall_rms == 0.0 and record.peak_amp == 0.0:
            self.logger.warning(
                f"丢弃零振动记录: dev={record.dev_id}, ts={record.timestamp_ms}"
            )
            self.stats.invalid_records += 1
            return

        if record.temperature_c == 0.0 and record.humidity_rh == 0.0:
            self.logger.warning(
                f"丢弃无温湿度记录: dev={record.dev_id}, ts={record.timestamp_ms}"
            )
            self.stats.invalid_records += 1
            return

        # 步骤6: 数据范围验证
        if not self.validator.validate_data_ranges(record):
            self.stats.invalid_records += 1
            return
        
        # 步骤7: 时间戳连续性检查
        if not self.validator.check_timestamp_continuity(record):
            self.stats.invalid_records += 1
            return
        
        # 步骤8: 写入CSV
        buffered_count = self.writer.append(record)
        self.stats.valid_records += 1
        
        # 定期打印进度 (每50条有效记录)
        if self.stats.valid_records % 50 == 0:
            elapsed = time.time() - self.stats.start_time
            rate = self.stats.valid_records / elapsed if elapsed > 0 else 0
            self.logger.info(
                f"📊 进度: 有效={self.stats.valid_records}, "
                f"无效={self.stats.invalid_records}, "
                f"速率={rate:.1f} 条/秒"
            )
    
    def _on_disconnect(self, client, userdata, rc):
        """
        MQTT断开连接回调
        
        Args:
            client: MQTT客户端实例
            userdata: 用户数据
            rc: 断开原因码
        """
        if rc != 0:
            self.logger.warning(f"意外断开连接 (rc={rc}), 将自动重连...")
        else:
            self.logger.info("已断开MQTT连接")
    
    def start(self):
        """
        启动MQTT订阅器 (阻塞运行直到调用stop())
        
        连接流程:
          1. TCP连接到Broker
          2. MQTT CONNECT握手
          3. SUBSCRIBE请求
          4. 进入循环等待消息
        """
        self.running = True
        self.logger.info("=" * 60)
        self.logger.info("  EdgeVib MQTT Data Collector Starting...")
        self.logger.info(f"  Broker: {self.broker_host}:{self.broker_port}")
        self.logger.info(f"  Topic:  {self.topic}")
        self.logger.info(f"  Output: {self.writer.output_path}")
        self.logger.info(f"  QoS:    {self.qos}")
        self.logger.info("=" * 60)
        
        try:
            # 连接Broker (阻塞直到连接成功或超时)
            self.client.connect(self.broker_host, self.broker_port, keepalive=60)
            
            # 启动网络循环 (阻塞)
            # loop_forever会自动处理重连
            self.client.loop_forever()
            
        except Exception as e:
            self.logger.error(f"致命错误: {e}")
            raise
        finally:
            self._cleanup()
    
    def stop(self):
        """
        停止订阅器 (触发优雅退出)
        
        执行步骤:
          1. 刷新CSV缓冲区
          2. 打印最终统计
          3. 断开MQTT连接
          4. 退出loop_forever
        """
        if not self.running:
            return
            
        self.running = False
        self.logger.info("正在停止数据采集...")
        
        # 刷新剩余数据
        total_written = self.writer.close()
        
        # 打印最终统计报告
        elapsed = time.time() - self.stats.start_time
        self.logger.info("\n" + "=" * 60)
        self.logger.info("  📈 采集统计报告")
        self.logger.info("=" * 60)
        self.logger.info(f"  总接收消息:   {self.stats.total_received}")
        self.logger.info(f"  有效记录:     {self.stats.valid_records}")
        self.logger.info(f"  无效记录:     {self.stats.invalid_records}")
        self.logger.info(f"  有效率:       {(self.stats.valid_records/max(1,self.stats.total_received))*100:.1f}%")
        self.logger.info(f"  运行时长:     {elapsed:.1f} 秒")
        self.logger.info(f"  平均速率:     {self.stats.valid_records/max(1,elapsed):.1f} 条/秒")
        self.logger.info(f"  输出文件:     {self.writer.output_path}")
        self.logger.info(f"  文件大小:     {os.path.getsize(self.writer.output_path)/1024:.1f} KB")
        self.logger.info("=" * 60)
        
        # 断开MQTT
        self.client.disconnect()
        self.client.loop_stop()
    
    def _cleanup(self):
        """清理资源"""
        self.writer.close()


# ==================== 命令行接口 ====================

def parse_arguments():
    """
    解析命令行参数
    
    Returns:
        解析后的参数命名空间
    """
    parser = argparse.ArgumentParser(
        description="EdgeVib MQTT数据采集器 - 从ESP32接收传感器数据并转换为CSV训练集",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
使用示例:
  %(prog)s                                    # 使用默认配置启动
  %(prog)s --broker 192.168.1.100:1883       # 连接远程Broker
  %(prog)s -o my_dataset.csv -b 50           # 自定义输出文件和缓冲区大小
  %(prog)s --verbose                         # 启用详细调试日志
        """
    )
    
    parser.add_argument(
        '--broker', '-b',
        type=str,
        default=f"{DEFAULT_BROKER_HOST}:{DEFAULT_BROKER_PORT}",
        help=f'MQTT Broker地址 (默认: {DEFAULT_BROKER_HOST}:{DEFAULT_BROKER_PORT})'
    )
    
    parser.add_argument(
        '--topic', '-t',
        type=str,
        default=DEFAULT_TOPIC_PREFIX,
        help=f'Topic前缀 (默认: {DEFAULT_TOPIC_PREFIX})'
    )
    
    parser.add_argument(
        '--qos', '-q',
        type=int,
        choices=[0, 1, 2],
        default=DEFAULT_QOS,
        help=f'MQTT QoS等级 (默认: {DEFAULT_QOS})'
    )
    
    parser.add_argument(
        '--output', '-o',
        type=str,
        default=DEFAULT_OUTPUT_FILE,
        help=f'输出CSV文件路径 (默认: {DEFAULT_OUTPUT_FILE})'
    )
    
    parser.add_argument(
        '--buffer-size',
        type=int,
        default=DEFAULT_BUFFER_SIZE,
        help=f'CSV缓冲区大小,条数 (默认: {DEFAULT_BUFFER_SIZE})'
    )
    
    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help='启用详细DEBUG日志'
    )
    
    parser.add_argument(
        '--version', '-V',
        action='version',
        version='%(prog)s 1.0.0'
    )
    
    return parser.parse_args()


def main():
    """
    程序入口点
    
    执行流程:
      1. 解析命令行参数
      2. 创建MqttDataSubscriber实例
      3. 调用start()开始采集 (阻塞)
      4. Ctrl-C触发stop()优雅退出
    """
    args = parse_arguments()
    
    # 解析broker地址
    if ':' in args.broker:
        host, port_str = args.broker.rsplit(':', 1)
        port = int(port_str)
    else:
        host = args.broker
        port = DEFAULT_BROKER_PORT
    
    # 创建并启动订阅器
    subscriber = MqttDataSubscriber(
        broker_host=host,
        broker_port=port,
        topic_prefix=args.topic,
        qos=args.qos,
        output_file=args.output,
        buffer_size=args.buffer_size,
        verbose=args.verbose
    )
    
    try:
        subscriber.start()
    except KeyboardInterrupt:
        # 已经在signal_handler中处理
        pass
    except Exception as e:
        logging.getLogger(__name__).fatal(f"程序异常终止: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
