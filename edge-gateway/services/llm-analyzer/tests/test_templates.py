"""Test template loading and variable filling."""

import tempfile
from pathlib import Path

import pytest

from src.llm.templates import TemplateLoader, PromptTemplate


SAMPLE_TEMPLATE_YAML = """
name: test_template
description: A test template
system_prompt: |
  You are a test assistant.
user_template: |
  Device: {device_id}
  Severity: {severity}
  Score: {health_score}

  Context:
  {vibration_summary}
example:
  user: |
    Example input text.
  output: |
    Example output text.
"""


@pytest.fixture
def template_dir():
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "test_template.yaml"
        p.write_text(SAMPLE_TEMPLATE_YAML, encoding="utf-8")
        yield d


def test_load_template_from_dir(template_dir):
    loader = TemplateLoader(template_dir)
    tmpl = loader.load("test_template")
    assert tmpl.name == "test_template"
    assert "device_id" in tmpl.placeholders
    assert "severity" in tmpl.placeholders
    assert "health_score" in tmpl.placeholders
    assert "vibration_summary" in tmpl.placeholders


def test_load_nonexistent_template(template_dir):
    loader = TemplateLoader(template_dir)
    with pytest.raises(FileNotFoundError):
        loader.load("does_not_exist")


def test_cache_hit(template_dir):
    loader = TemplateLoader(template_dir)
    t1 = loader.load("test_template")
    t2 = loader.load("test_template")
    assert t1 is t2  # Cached


def test_fill_template(template_dir):
    loader = TemplateLoader(template_dir)
    tmpl = loader.load("test_template")
    result = tmpl.fill(
        device_id="de01",
        severity="CRITICAL",
        health_score=35.0,
        vibration_summary="- RMS: 8.5 mm/s",
    )
    assert "de01" in result
    assert "CRITICAL" in result
    assert "35.0" in result
    assert "RMS: 8.5 mm/s" in result


def test_fill_missing_variables(template_dir):
    """Missing placeholders should be filled with 'N/A'."""
    loader = TemplateLoader(template_dir)
    tmpl = loader.load("test_template")
    result = tmpl.fill(device_id="de01", severity="WARNING")
    # health_score and vibration_summary are missing
    assert "N/A" in result


def test_fill_none_value(template_dir):
    """None values are replaced with 'N/A'."""
    loader = TemplateLoader(template_dir)
    tmpl = loader.load("test_template")
    result = tmpl.fill(
        device_id="de01",
        severity="WARNING",
        health_score=None,
        vibration_summary="data",
    )
    assert "N/A" in result


def test_build_messages(template_dir):
    loader = TemplateLoader(template_dir)
    tmpl = loader.load("test_template")
    messages = tmpl.build_messages(
        device_id="de01", severity="CRITICAL",
        health_score=35, vibration_summary="test",
    )
    assert len(messages) == 4  # system + example_user + example_assistant + user
    assert messages[0]["role"] == "system"
    assert messages[1]["role"] == "user"
    assert messages[1]["content"].strip() == "Example input text."
    assert messages[2]["role"] == "assistant"
    assert messages[2]["content"].strip() == "Example output text."
    assert messages[3]["role"] == "user"
    assert "de01" in messages[3]["content"]


def test_list_templates(template_dir):
    loader = TemplateLoader(template_dir)
    names = loader.list_templates()
    assert "test_template" in names


def test_list_templates_empty_dir():
    with tempfile.TemporaryDirectory() as d:
        loader = TemplateLoader(d)
        assert loader.list_templates() == []


def test_list_templates_nonexistent_dir():
    loader = TemplateLoader("/nonexistent/path")
    assert loader.list_templates() == []


def test_placeholder_extraction():
    tmpl = PromptTemplate(
        name="test",
        user_template="Hello {name}, your score is {score}",
    )
    assert tmpl.placeholders == {"name", "score"}
