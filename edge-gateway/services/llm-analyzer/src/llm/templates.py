"""YAML prompt template loader with variable substitution."""

import re
import structlog
from dataclasses import dataclass, field
from pathlib import Path

import yaml

logger = structlog.get_logger(__name__)


@dataclass
class PromptTemplate:
    name: str
    description: str = ""
    system_prompt: str = ""
    user_template: str = ""
    example_user: str = ""
    example_output: str = ""
    _placeholders: set = field(default_factory=set, repr=False)

    def __post_init__(self):
        # Extract all {placeholder} variables from user_template
        pattern = re.findall(r'\{(\w+)\}', self.user_template)
        self._placeholders = set(pattern)

    @property
    def placeholders(self) -> set:
        return self._placeholders

    def fill(self, **variables) -> str:
        """Fill user_template with provided variables, building the full prompt.

        Missing variables are replaced with 'N/A'.
        """
        safe_vars = {p: "N/A" for p in self._placeholders}
        for k, v in variables.items():
            safe_vars[k] = str(v) if v is not None else "N/A"
        prompt = self.user_template.format(**safe_vars)
        return prompt

    def build_messages(self, **variables) -> list[dict]:
        """Build chat completion messages with system prompt, 1-shot example, and user prompt."""
        filled_user = self.fill(**variables)
        messages = [{"role": "system", "content": self.system_prompt}]

        if self.example_user and self.example_output:
            messages.append({"role": "user", "content": self.example_user})
            messages.append({"role": "assistant", "content": self.example_output})

        messages.append({"role": "user", "content": filled_user})
        return messages

    def build_prompt_text(self, **variables) -> str:
        """Build a single text prompt string (for non-chat models)."""
        parts = []
        if self.system_prompt:
            parts.append(f"<|system|>\n{self.system_prompt}</s>")
        if self.example_user and self.example_output:
            parts.append(f"<|user|>\n{self.example_user}</s>")
            parts.append(f"<|assistant|>\n{self.example_output}</s>")
        filled_user = self.fill(**variables)
        parts.append(f"<|user|>\n{filled_user}</s>")
        parts.append("<|assistant|>")
        return "\n".join(parts)


class TemplateLoader:
    """Load and cache YAML prompt templates from a directory."""

    def __init__(self, prompts_dir: str = "prompts"):
        self.prompts_dir = Path(prompts_dir)
        self._cache: dict[str, PromptTemplate] = {}

    def load(self, name: str) -> PromptTemplate:
        """Load a template by name (without .yaml extension)."""
        if name in self._cache:
            return self._cache[name]

        file_path = self.prompts_dir / f"{name}.yaml"
        if not file_path.exists():
            raise FileNotFoundError(f"Template not found: {file_path}")

        with open(file_path, encoding="utf-8") as f:
            data = yaml.safe_load(f)

        template = PromptTemplate(
            name=data.get("name", name),
            description=data.get("description", ""),
            system_prompt=data.get("system_prompt", ""),
            user_template=data.get("user_template", ""),
            example_user=data.get("example", {}).get("user", ""),
            example_output=data.get("example", {}).get("output", ""),
        )
        self._cache[name] = template
        logger.debug("template loaded", name=name,
                     placeholders=list(template.placeholders))
        return template

    def list_templates(self) -> list[str]:
        """List available template names."""
        if not self.prompts_dir.exists():
            return []
        return [p.stem for p in self.prompts_dir.glob("*.yaml")]
