#!/usr/bin/env python3
"""Interactive Meteoris TOML configuration wizard.

The canonical config/meteoris.toml file defines parameter order, compiled-style
sample values and explanatory comments.  An existing configuration, when
present, supplies the proposed values while missing parameters fall back to the
canonical template.
"""

from __future__ import annotations

import argparse
import ast
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

try:
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - Python < 3.11
    tomllib = None


VERSION = "1.1"
METEORIS_VERSION_FALLBACK = "0.6.0"
DEFAULT_INPUT = Path("meteoris.toml")
DEFAULT_OUTPUT = Path("meteoris.toml.new")

# Smart mode deliberately asks only values that normally need site/operator
# decisions.  All other settings are still emitted, using current/canonical
# defaults, so the generated file remains complete and self-documenting.
SMART_ESSENTIAL = {
    "sdr.driver",
    "sdr.sample_rate",
    "sdr.center_frequency",
    "sdr.gain",
    "dsp.shift_hz",
    "dsp.bandwidth_hz",
    "runtime.daemon",
    "logging.level",
    "logging.file_enabled",
    "logging.file",
    "agc.enabled",
    "recording.mode",
    "recording.segment_seconds",
    "recording.segment_count",
    "detector.plugin",
    "detector.peak_threshold_db",
    "detector.pre_context_s",
    "detector.post_context_s",
    "detector.max_event_seconds",
    "output.directory",
    "output.file_prefix",
    "output.hdf5_compression",
    "output.swmr",
    "output.daily_rotate_time",
    "output.max_growth_mb_per_min",
    "output.min_free_space_gb",
}

EXPLANATION_OVERRIDES = {
    "sdr.driver": "SoapySDR driver name used to open the receiver.",
    "sdr.sample_rate": "Input SDR sample rate in samples per second.",
    "dsp.decimation_stage1": "First DSP decimation factor.",
    "dsp.decimation_stage2": "Second DSP decimation factor.",
    "dsp.bandwidth_hz": "Processed output bandwidth after frequency shift and decimation, in Hz.",
    "psd.fft_size": "Number of samples/bins used by each PSD FFT.",
    "logging.file": "Base path/name for the text log file.",
    "logging.daily_rotate_time": "UTC time used as the daily text-log rotation boundary (HH:MM).",
    "agc.max_gain_db": "Maximum gain the AGC may request; -1 uses the SDR-reported limit.",
    "detector.post_context_s": "Seconds of PSD context retained after an event becomes inactive.",
    "detector.stationary.enabled": "Enable stationary/near-zero-drift track detection.",
    "detector.stationary.max_hz": "Highest frequency offset eligible for stationary detection.",
    "detector.stationary.activation_fraction": "Minimum occupied fraction of the activation interval for a stationary track.",
    "detector.chirp.enabled": "Enable drifting/chirp track detection.",
    "detector.chirp.max_hz": "Highest frequency offset eligible for chirp detection.",
    "detector.chirp.activation_time_s": "Minimum age before a tentative chirp track can activate.",
    "detector.chirp.activation_fraction": "Minimum occupied fraction of the activation interval for a chirp track.",
    "detector.chirp.max_drift_hz_s": "Maximum average drift rate accepted as a chirp, in Hz/s.",
    "detector.echoes.detection_width_hz": "Width of the Echoes detection interval; <= 0 uses the full band.",
    "detector.echoes.absolute_upper_db_hz": "Upper absolute-mode trigger threshold in PSD-density dB/Hz.",
    "detector.echoes.differential_upper_db": "Upper differential-mode trigger threshold for signal-minus-noise in dB.",
    "detector.echoes.automatic_upper_delta_db": "Automatic-mode amount added above the lower threshold to form the upper threshold.",
    "detector.echoes.automatic_warmup_s": "Baseline-learning time before automatic threshold detection starts.",
    "detector.echoes.automatic_baseline_time_constant_s": "Time constant used to adapt the automatic idle baseline.",
    "detector.echoes.automatic_stddev_window_s": "Window duration used for automatic-mode standard-deviation estimation.",
    "detector.echoes.automatic_end_stddev_factor": "Standard-deviation multiplier used by the automatic event-end condition.",
    "output.file_prefix": "Prefix used when naming daily Meteoris HDF5 files.",
}

SECTION_RE = re.compile(r"^\s*\[([^\]]+)\]\s*(?:#.*)?$")
ASSIGN_RE = re.compile(r"^\s*([A-Za-z0-9_-]+)\s*=\s*(.*?)\s*(?:#.*)?$")


@dataclass
class Parameter:
    section: str
    key: str
    canonical_value: Any
    explanation: str
    section_number: int
    parameter_number: int

    @property
    def dotted(self) -> str:
        return f"{self.section}.{self.key}"

    @property
    def number(self) -> str:
        return f"{self.section_number}.{self.parameter_number}"


@dataclass
class Section:
    name: str
    number: int
    parameters: list[Parameter]


def _nested_get(data: dict[str, Any], dotted: str) -> tuple[bool, Any]:
    node: Any = data
    for part in dotted.split("."):
        if not isinstance(node, dict) or part not in node:
            return False, None
        node = node[part]
    return True, node


def _nested_set(data: dict[str, Any], dotted: str, value: Any) -> None:
    parts = dotted.split(".")
    node = data
    for part in parts[:-1]:
        child = node.get(part)
        if not isinstance(child, dict):
            child = {}
            node[part] = child
        node = child
    node[parts[-1]] = value


def _canonical_path() -> Path:
    # Source-tree use: tools/../config/meteoris.toml
    source = Path(__file__).resolve().parent.parent / "config" / "meteoris.toml"
    if source.is_file():
        return source

    # Installed tool: look in common share locations relative to executable.
    exe = Path(sys.argv[0]).resolve()
    candidates = [
        exe.parent.parent / "share" / "meteoris" / "meteoris.toml",
        Path("/usr/local/share/meteoris/meteoris.toml"),
        Path("/usr/share/meteoris/meteoris.toml"),
    ]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(
        "cannot locate canonical config/meteoris.toml; run from the source tree "
        "or install the Meteoris configuration templates"
    )


def _brief_comment(comment_lines: list[str], dotted: str) -> str:
    if not comment_lines:
        return EXPLANATION_OVERRIDES.get(dotted, f"Meteoris setting {dotted}.")
    text = " ".join(line.strip().lstrip("#").strip() for line in comment_lines)
    text = re.sub(r"\s+", " ", text).strip()
    if not text:
        return f"Meteoris setting {dotted}."
    # Prefer the first sentence, but retain a short second clause when the first
    # sentence is tiny.  Keep prompts readable on terminals.
    sentence = re.split(r"(?<=[.!?])\s+", text, maxsplit=1)[0]
    if len(sentence) < 45 and len(text) <= 170:
        sentence = text
    if len(sentence) > 180:
        sentence = sentence[:177].rstrip() + "..."
    return sentence


def load_schema(path: Path) -> tuple[list[Section], dict[str, Any]]:
    if tomllib is None:
        raise RuntimeError("meteoris_config requires Python 3.11+ (tomllib)")
    text = path.read_text(encoding="utf-8")
    canonical = tomllib.loads(text)

    sections: list[Section] = []
    current: Section | None = None
    pending_comments: list[str] = []

    for line in text.splitlines():
        stripped = line.strip()
        section_match = SECTION_RE.match(line)
        if section_match:
            current = Section(section_match.group(1), len(sections) + 1, [])
            sections.append(current)
            pending_comments = []
            continue
        if stripped.startswith("#"):
            pending_comments.append(stripped)
            continue
        if not stripped:
            pending_comments = []
            continue
        assign_match = ASSIGN_RE.match(line)
        if assign_match and current is not None:
            key = assign_match.group(1)
            dotted = f"{current.name}.{key}"
            found, value = _nested_get(canonical, dotted)
            if not found:
                raise RuntimeError(f"canonical template parser lost parameter {dotted}")
            current.parameters.append(
                Parameter(
                    section=current.name,
                    key=key,
                    canonical_value=value,
                    explanation=_brief_comment(pending_comments, dotted),
                    section_number=current.number,
                    parameter_number=len(current.parameters) + 1,
                )
            )
            pending_comments = []
        else:
            pending_comments = []

    return sections, canonical



def reorder_schema_for_current(sections: list[Section], current_path: Path) -> list[Section]:
    """Follow an existing file's section/key order, appending missing canonical items."""
    canonical_by_section = {s.name: s for s in sections}
    seen_section_order: list[str] = []
    seen_keys: dict[str, list[str]] = {}
    active: str | None = None

    for line in current_path.read_text(encoding="utf-8").splitlines():
        sm = SECTION_RE.match(line)
        if sm:
            active = sm.group(1)
            if active in canonical_by_section and active not in seen_section_order:
                seen_section_order.append(active)
                seen_keys[active] = []
            continue
        am = ASSIGN_RE.match(line)
        if am and active in canonical_by_section:
            key = am.group(1)
            valid = {p.key for p in canonical_by_section[active].parameters}
            if key in valid and key not in seen_keys[active]:
                seen_keys[active].append(key)

    section_names = seen_section_order + [s.name for s in sections if s.name not in seen_section_order]
    reordered: list[Section] = []
    for section_number, name in enumerate(section_names, 1):
        canonical_section = canonical_by_section[name]
        by_key = {p.key: p for p in canonical_section.parameters}
        keys = seen_keys.get(name, []) + [p.key for p in canonical_section.parameters if p.key not in seen_keys.get(name, [])]
        params: list[Parameter] = []
        for parameter_number, key in enumerate(keys, 1):
            p = by_key[key]
            params.append(Parameter(
                section=p.section, key=p.key, canonical_value=p.canonical_value,
                explanation=p.explanation, section_number=section_number,
                parameter_number=parameter_number))
        reordered.append(Section(name=name, number=section_number, parameters=params))
    return reordered

def load_toml(path: Path) -> dict[str, Any]:
    if tomllib is None:
        raise RuntimeError("meteoris_config requires Python 3.11+ (tomllib)")
    with path.open("rb") as handle:
        return tomllib.load(handle)


def _format_value(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        escaped = value.replace("\\", "\\\\").replace('"', '\\"')
        return f'"{escaped}"'
    if isinstance(value, float):
        return repr(value)
    if isinstance(value, int):
        return str(value)
    if isinstance(value, list):
        return "[" + ", ".join(_format_value(item) for item in value) + "]"
    raise TypeError(f"unsupported TOML value type: {type(value).__name__}")


def _parse_value(raw: str, default: Any) -> Any:
    text = raw.strip()
    if isinstance(default, bool):
        lowered = text.lower()
        if lowered in {"true", "yes", "y", "1", "on"}:
            return True
        if lowered in {"false", "no", "n", "0", "off"}:
            return False
        raise ValueError("enter true/false (yes/no also accepted)")
    if isinstance(default, int) and not isinstance(default, bool):
        return int(text, 0)
    if isinstance(default, float):
        return float(text)
    if isinstance(default, str):
        # Quotes are optional at the prompt. If supplied, parse them naturally.
        if len(text) >= 2 and text[0] == text[-1] and text[0] in {'"', "'"}:
            parsed = ast.literal_eval(text)
            if not isinstance(parsed, str):
                raise ValueError("expected a string")
            return parsed
        return text
    if isinstance(default, list):
        parsed = ast.literal_eval(text)
        if not isinstance(parsed, list):
            raise ValueError("expected a list")
        return parsed
    raise ValueError(f"unsupported value type {type(default).__name__}")


def _yes_no(prompt: str, default: bool = True) -> bool:
    suffix = " [Y/n]: " if default else " [y/N]: "
    while True:
        reply = input(prompt + suffix).strip().lower()
        if not reply:
            return default
        if reply in {"y", "yes"}:
            return True
        if reply in {"n", "no"}:
            return False
        print("Please answer yes or no.")


def _choose_mode(explicit: str | None) -> str:
    if explicit:
        return explicit
    print("Configuration mode:")
    print("  1) smart  - ask only essential parameters")
    print("  2) expert - ask every parameter")
    while True:
        reply = input("Mode [1]: ").strip()
        if not reply or reply == "1":
            return "smart"
        if reply == "2":
            return "expert"
        print("Enter 1 for smart or 2 for expert.")


def _ordered_parameters(sections: list[Section], mode: str) -> list[Parameter]:
    params = [parameter for section in sections for parameter in section.parameters]
    if mode == "expert":
        return params
    return [parameter for parameter in params if parameter.dotted in SMART_ESSENTIAL]


def _prompt_parameter(parameter: Parameter, default: Any) -> Any:
    print(f"\n{parameter.number} {parameter.dotted}")
    print(f"    {parameter.explanation}")
    while True:
        reply = input(f"    Value [{_format_value(default)}]: ")
        if not reply.strip():
            return default
        try:
            return _parse_value(reply, default)
        except (ValueError, SyntaxError) as exc:
            print(f"    Invalid value: {exc}")


def _review(parameters: list[Parameter], values: dict[str, Any]) -> None:
    print("\nInput review")
    print("------------")
    for parameter in parameters:
        found, value = _nested_get(values, parameter.dotted)
        if found:
            print(f"{parameter.number:>5}  {parameter.dotted:<42} = {_format_value(value)}")



def _meteoris_version() -> str:
    """Return the Meteoris application version for the startup banner."""
    # In a source checkout, CMakeLists.txt is the authoritative project version.
    source_cmake = Path(__file__).resolve().parent.parent / "CMakeLists.txt"
    if source_cmake.is_file():
        try:
            text = source_cmake.read_text(encoding="utf-8")
            match = re.search(r"project\s*\(\s*Meteoris.*?VERSION\s+([0-9]+(?:\.[0-9]+)+)", text, re.S | re.I)
            if match:
                return match.group(1)
        except OSError:
            pass

    # Installed/build-tree use: prefer the meteoris executable beside the wizard.
    candidates = [
        Path(sys.argv[0]).resolve().parent / "meteoris",
        Path(__file__).resolve().parent / "meteoris",
    ]
    for executable in candidates:
        if not executable.is_file() or not os.access(executable, os.X_OK):
            continue
        try:
            result = subprocess.run(
                [str(executable), "--version"],
                check=False, capture_output=True, text=True, timeout=2.0
            )
            match = re.search(r"\bmeteoris\s+([0-9]+(?:\.[0-9]+)+)", result.stdout, re.I)
            if match:
                return match.group(1)
        except (OSError, subprocess.SubprocessError):
            pass

    return METEORIS_VERSION_FALLBACK


def _parameter_by_number(sections: list[Section]) -> dict[str, Parameter]:
    return {p.number: p for section in sections for p in section.parameters}


def _review_edit_loop(
    review_parameters: list[Parameter],
    sections: list[Section],
    values: dict[str, Any],
) -> list[Parameter]:
    """Review values and optionally edit parameters by their displayed number."""
    visible = list(review_parameters)
    visible_dotted = {p.dotted for p in visible}
    by_number = _parameter_by_number(sections)

    while True:
        _review(visible, values)
        if _yes_no("\nInput data OK?", default=True):
            return visible

        print("Enter a parameter number to modify. Press Enter when finished editing")
        print("to return to the review.")
        while True:
            number = input("Parameter number to modify [Enter=review]: ").strip()
            if not number:
                break
            parameter = by_number.get(number)
            if parameter is None:
                print(f"Unknown parameter number: {number}")
                continue
            _, default = _nested_get(values, parameter.dotted)
            value = _prompt_parameter(parameter, default)
            _nested_set(values, parameter.dotted, value)
            if parameter.dotted not in visible_dotted:
                visible.append(parameter)
                visible_dotted.add(parameter.dotted)
                visible.sort(key=lambda p: (p.section_number, p.parameter_number))


def _write_toml(path: Path, sections: list[Section], values: dict[str, Any], source: str) -> None:
    lines = [
        "# Meteoris configuration generated by meteoris_config.",
        f"# Proposed values were based on: {source}",
        "# Parameter numbers match the interactive wizard order.",
        "",
    ]
    for section in sections:
        lines.append(f"[{section.name}]")
        for parameter in section.parameters:
            found, value = _nested_get(values, parameter.dotted)
            if not found:
                value = parameter.canonical_value
            lines.append(f"# {parameter.number} {parameter.explanation}")
            lines.append(f"{parameter.key} = {_format_value(value)}")
        lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Interactively create a commented meteoris.toml configuration."
    )
    parser.add_argument(
        "config",
        nargs="?",
        type=Path,
        help="existing TOML used for proposed defaults (default: ./meteoris.toml when present)",
    )
    parser.add_argument("--mode", choices=("smart", "expert"), help="skip the mode question")
    parser.add_argument("--version", action="version", version=f"meteoris_config {VERSION}")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        schema_path = _canonical_path()
        sections, canonical = load_schema(schema_path)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    input_path: Path | None = args.config
    if input_path is None and DEFAULT_INPUT.is_file():
        input_path = DEFAULT_INPUT

    values: dict[str, Any] = canonical
    source = str(schema_path)
    if input_path is not None:
        if not input_path.is_file():
            print(f"ERROR: input configuration does not exist: {input_path}", file=sys.stderr)
            return 2
        try:
            current = load_toml(input_path)
            sections = reorder_schema_for_current(sections, input_path)
        except (OSError, ValueError) as exc:
            print(f"ERROR: cannot read {input_path}: {exc}", file=sys.stderr)
            return 2
        # Start with a fresh canonical load, then overlay known current values.
        values = load_toml(schema_path)
        for section in sections:
            for parameter in section.parameters:
                found, value = _nested_get(current, parameter.dotted)
                if found:
                    _nested_set(values, parameter.dotted, value)
        source = str(input_path)

    print("Meteoris configuration wizard")
    print(f"Meteoris version: {_meteoris_version()}")
    if input_path is not None:
        print(f"Input filename: {input_path}")
    else:
        print(f"Input filename: {DEFAULT_INPUT} (not found; using {schema_path})")

    mode = _choose_mode(args.mode)
    asked = _ordered_parameters(sections, mode)
    print(f"Mode: {mode} ({len(asked)} of {sum(len(s.parameters) for s in sections)} parameters will be asked)")

    for parameter in asked:
        _, default = _nested_get(values, parameter.dotted)
        value = _prompt_parameter(parameter, default)
        _nested_set(values, parameter.dotted, value)

    review_parameters = asked if mode == "smart" else [p for s in sections for p in s.parameters]
    _review_edit_loop(review_parameters, sections, values)

    reply = input(f"\nOutput TOML [{DEFAULT_OUTPUT}]: ").strip()
    output = Path(reply) if reply else DEFAULT_OUTPUT
    if output.exists() and not _yes_no(f"{output} already exists. Overwrite?", default=False):
        print("Not written.")
        return 1

    try:
        _write_toml(output, sections, values, source)
        # Parse our own output before claiming success.
        load_toml(output)
    except (OSError, ValueError, TypeError) as exc:
        print(f"ERROR: cannot write valid TOML: {exc}", file=sys.stderr)
        return 2

    print(f"Wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
