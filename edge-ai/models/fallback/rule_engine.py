import numpy as np
import yaml
import os
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass, field, asdict

from utils.logger import get_logger

logger = get_logger(__name__)


@dataclass
class ISORule:
    """单条ISO 10816规则"""
    name: str
    condition: str
    severity: str
    message: str
    threshold: float = 0.0
    feature: str = ""


@dataclass
class RuleEngineConfig:
    """规则引擎配置"""
    iso_class: str = "class_II"
    alarm_threshold_mm_s: float = 7.1
    trip_threshold_mm_s: float = 11.0
    peak_freq_high_warn: float = 500.0
    crest_factor_warn: float = 6.0
    kurtosis_warn: float = 4.0
    temp_high_warn: float = 85.0
    custom_rules: List[Dict] = field(default_factory=list)


class RuleEngine:
    """
    企业级规则引擎 — 兜底模型的诊断逻辑。

    基于 ISO 10816-3 工业振动标准，实现:
    - 振动烈度评估 (A/B/C/D 区域)
    - 温度异常检测
    - 波峰因子/峰度异常检测
    - 自定义规则链
    - 人类可读的诊断结论生成

    ISO 10816-3 区域定义 (Class II 中型电机):
        Zone A (良好):    < 1.4 mm/s
        Zone B (可接受):  1.4 - 2.8 mm/s
        Zone C (报警):    2.8 - 7.1 mm/s
        Zone D (危险):    > 7.1 mm/s
    """

    ISO_ZONES = {
        'class_I': {'A': 0.71, 'B': 1.4, 'C': 4.5},      # 小型机器
        'class_II': {'A': 1.4, 'B': 2.8, 'C': 7.1},       # 中型机器
        'class_III': {'A': 2.8, 'B': 4.5, 'C': 11.0},      # 大型刚性基础
        'class_IV': {'A': 4.5, 'B': 7.1, 'C': 18.0}        # 大型柔性基础
    }

    def __init__(self, config: Optional[RuleEngineConfig] = None):
        self.config = config or RuleEngineConfig()
        self.rules: List[ISORule] = []
        self._build_default_rules()

    def _build_default_rules(self):
        """构建默认规则集"""
        zones = self.ISO_ZONES.get(self.config.iso_class,
                                    self.ISO_ZONES['class_II'])

        self.rules = [
            ISORule(
                name="iso_zone_d",
                condition="overall_rms > trip",
                severity="critical",
                message=f"振动烈度超过跳机阈值",
                threshold=self.config.trip_threshold_mm_s,
                feature="overall_rms"
            ),
            ISORule(
                name="iso_zone_c",
                condition="overall_rms > alarm",
                severity="warning",
                message=f"振动烈度超过报警阈值",
                threshold=self.config.alarm_threshold_mm_s,
                feature="overall_rms"
            ),
            ISORule(
                name="high_frequency",
                condition="peak_freq > threshold",
                severity="warning",
                message="高频振动分量异常，可能轴承故障",
                threshold=self.config.peak_freq_high_warn,
                feature="peak_freq"
            ),
            ISORule(
                name="high_crest_factor",
                condition="crest_factor > threshold",
                severity="warning",
                message="波峰因子过高，可能存在冲击性故障",
                threshold=self.config.crest_factor_warn,
                feature="crest_factor"
            ),
            ISORule(
                name="high_kurtosis",
                condition="kurtosis > threshold",
                severity="info",
                message="峰度升高，振动分布出现重尾",
                threshold=self.config.kurtosis_warn,
                feature="kurtosis"
            ),
            ISORule(
                name="high_temperature",
                condition="temperature_c > threshold",
                severity="warning",
                message="温度异常升高",
                threshold=self.config.temp_high_warn,
                feature="temperature_c"
            ),
        ]

    def diagnose(self, features: Dict[str, float]) -> Dict:
        """
        执行规则引擎诊断。

        Args:
            features: 特征字典。支持以下键:
                - overall_rms: 整体振动烈度 (mm/s 或 g)
                - peak_freq: 主峰值频率 (Hz)
                - crest_factor: 波峰因子
                - kurtosis: 峰度
                - temperature_c: 温度 (°C)
                - rms_x, rms_y, rms_z: 各轴RMS

        Returns:
            诊断结果字典:
            {
                'status': 'normal' | 'warning' | 'critical',
                'iso_zone': 'A' | 'B' | 'C' | 'D',
                'triggered_rules': [...],
                'confidence': 0.0-1.0,
                'diagnosis_summary': "人类可读的诊断结论"
            }
        """
        result = {
            'status': 'normal',
            'iso_zone': 'A',
            'triggered_rules': [],
            'confidence': 0.95,
            'diagnosis_summary': '',
            'feature_values': {k: round(v, 4) for k, v in features.items()}
        }

        overall_rms = features.get('overall_rms', 0)
        zones = self.ISO_ZONES.get(self.config.iso_class,
                                    self.ISO_ZONES['class_II'])

        if overall_rms >= zones['C']:
            result['iso_zone'] = 'D'
            result['status'] = 'critical'
        elif overall_rms >= zones['B']:
            result['iso_zone'] = 'C'
            result['status'] = 'warning'
        elif overall_rms >= zones['A']:
            result['iso_zone'] = 'B'
            result['status'] = 'info'
        else:
            result['iso_zone'] = 'A'
            result['status'] = 'normal'

        triggered = []
        for rule in self.rules:
            value = features.get(rule.feature, 0)
            if value > rule.threshold:
                triggered.append({
                    'rule': rule.name,
                    'severity': rule.severity,
                    'message': rule.message,
                    'value': round(value, 4),
                    'threshold': rule.threshold
                })

        if triggered:
            result['triggered_rules'] = triggered
            severities = [r['severity'] for r in triggered]
            if 'critical' in severities:
                result['status'] = 'critical'
            elif 'warning' in severities and result['status'] != 'critical':
                result['status'] = 'warning'

        result['confidence'] = self._calculate_confidence(result, features)

        result['diagnosis_summary'] = self._generate_summary(result)

        return result

    def _calculate_confidence(
        self, result: Dict, features: Dict[str, float]
    ) -> float:
        """计算规则引擎诊断置信度"""
        n_triggers = len(result['triggered_rules'])
        if n_triggers == 0:
            return 0.95

        severity_weights = {'info': 0.1, 'warning': 0.3, 'critical': 0.6}
        weighted = sum(severity_weights.get(r['severity'], 0) for r in result['triggered_rules'])
        confidence = min(1.0, 0.5 + weighted * 0.5)
        return round(confidence, 3)

    def _generate_summary(self, result: Dict) -> str:
        """生成人类可读的诊断结论"""
        iso_zone = result['iso_zone']
        status = result['status']

        summary_parts = []

        if iso_zone == 'A':
            summary_parts.append("设备振动状态良好，运行平稳")
        elif iso_zone == 'B':
            summary_parts.append("设备振动可接受，建议持续监测")
        elif iso_zone == 'C':
            summary_parts.append("设备振动超标，需要安排检修计划")
        elif iso_zone == 'D':
            summary_parts.append("设备振动严重超标，建议立即停机检查")

        for rule in result['triggered_rules']:
            summary_parts.append(f"[{rule['severity']}] {rule['message']}")

        if not result['triggered_rules']:
            summary_parts.append("所有监测指标正常")

        return " | ".join(summary_parts)

    def calibrate_from_data(
        self, df, rms_col: str = 'overall_rms',
        temp_col: str = 'temperature_c', percentile: float = 95.0
    ):
        """
        从历史数据自动标定阈值。

        Args:
            df: 历史数据DataFrame
            rms_col: RMS列名
            temp_col: 温度列名
            percentile: 阈值分位数
        """
        if rms_col in df.columns:
            rms_95 = float(np.percentile(df[rms_col].dropna(), percentile))
            rms_99 = float(np.percentile(df[rms_col].dropna(), 99))
            self.config.alarm_threshold_mm_s = rms_95
            self.config.trip_threshold_mm_s = rms_99
            logger.info("RMS thresholds calibrated: alarm=%.3f, trip=%.3f",
                        rms_95, rms_99)

        if temp_col in df.columns:
            temp_95 = float(np.percentile(df[temp_col].dropna(), percentile))
            self.config.temp_high_warn = temp_95
            logger.info("Temperature threshold calibrated: %.1f°C", temp_95)

        self._build_default_rules()

    def save_config(self, filepath: str):
        """保存规则引擎配置"""
        os.makedirs(os.path.dirname(filepath), exist_ok=True)
        with open(filepath, 'w', encoding='utf-8') as f:
            yaml.dump(asdict(self.config), f, default_flow_style=False)
        logger.info("Rule engine config saved: %s", filepath)

    def load_config(self, filepath: str):
        """加载规则引擎配置"""
        with open(filepath, 'r', encoding='utf-8') as f:
            data = yaml.safe_load(f)
        self.config = RuleEngineConfig(**data)
        self._build_default_rules()
        logger.info("Rule engine config loaded: %s", filepath)
