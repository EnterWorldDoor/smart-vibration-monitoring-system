"""Test LLM output parsing and report builder data assembly."""

import pytest

from src.report_builder import parse_report_sections


class TestParseReportSections:
    def test_standard_four_section_format(self):
        text = (
            "【标题】motor de01 驱动端轴承故障恶化\n"
            "【当前状态】\n设备运行在危险区间，RMS达到8.5 mm/s。\n"
            "【异常分析】\n1. 振动总量超标\n2. 轴承故障特征\n"
            "【维护建议】\n1. 立即安排停机检查\n2. 重点检查驱动端轴承\n"
        )
        result = parse_report_sections(text)
        assert "motor de01" in result["title"]
        assert "危险区间" in result["summary"]
        assert "振动总量超标" in result["analysis"]
        assert "停机检查" in result["advice"]

    def test_missing_sections_handled_gracefully(self):
        text = "Some unstructured response without sections."
        result = parse_report_sections(text)
        assert result["title"] == ""
        # Falls back to using full text as summary
        assert "unstructured" in result["summary"]

    def test_partial_sections(self):
        text = (
            "【标题】Test Title\n"
            "【当前状态】\nStatus text here.\n"
        )
        result = parse_report_sections(text)
        assert result["title"] == "Test Title"
        assert "Status text" in result["summary"]
        assert result["analysis"] == ""
        assert result["advice"] == ""

    def test_empty_text(self):
        result = parse_report_sections("")
        assert result["title"] == ""
        assert result["summary"] == ""
        assert result["analysis"] == ""
        assert result["advice"] == ""

    def test_title_only(self):
        text = "【标题】Single title line"
        result = parse_report_sections(text)
        assert result["title"] == "Single title line"
        assert result["summary"] == ""
        assert result["analysis"] == ""
        assert result["advice"] == ""

    def test_multiline_sections(self):
        text = (
            "【标题】Test\n"
            "【当前状态】\nLine 1\nLine 2\nLine 3\n"
            "【异常分析】\nAnalysis line 1\nAnalysis line 2\n"
            "【维护建议】\nAdvice 1\nAdvice 2\nAdvice 3"
        )
        result = parse_report_sections(text)
        assert "Line 1" in result["summary"]
        assert "Line 3" in result["summary"]
        assert "Analysis line 1" in result["analysis"]
        assert "Advice 3" in result["advice"]

    def test_chinese_brackets_only(self):
        """Only 【】 brackets should be parsed, not [[]] or other variants."""
        text = "标题: Some title\n[当前状态]\nStatus here\n"
        result = parse_report_sections(text)
        # [当前状态] should NOT be matched — only 【当前状态】
        assert result["title"] == ""
