#!/usr/bin/env python3
"""Regression tests for meteoris_recover_hdf5 destination selection."""

from __future__ import annotations

import builtins
import importlib.util
import sys
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "meteoris_recover_hdf5.py"

spec = importlib.util.spec_from_file_location("meteoris_recover_hdf5", TOOL)
assert spec and spec.loader
recover = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = recover
spec.loader.exec_module(recover)


def with_input(values, func, *args):
    answers = iter(values)
    with mock.patch.object(builtins, "input", side_effect=lambda _prompt="": next(answers)):
        return func(*args)


def main() -> int:
    source = Path("/tmp/example.h5")

    output, in_place = with_input(["", ""], recover._choose_destination, source)
    assert output == Path("/tmp/example.h5.recovery")
    assert in_place is False

    output, in_place = with_input(["1", "/tmp/custom.h5"], recover._choose_destination, source)
    assert output == Path("/tmp/custom.h5")
    assert in_place is False

    output, in_place = with_input(["2", "yes"], recover._choose_destination, source)
    assert output == source
    assert in_place is True

    try:
        with_input(["2", ""], recover._choose_destination, source)
    except recover.RecoveryError as exc:
        assert "cancelled" in str(exc).lower()
    else:
        raise AssertionError("In-place recovery must require explicit confirmation")

    parser = recover.build_parser()
    args = parser.parse_args(["--output", "copy.h5", "input.h5"])
    assert args.output == Path("copy.h5") and not args.in_place
    args = parser.parse_args(["--in-place", "input.h5"])
    assert args.in_place and args.output is None

    try:
        parser.parse_args(["--in-place", "--output", "copy.h5", "input.h5"])
    except SystemExit as exc:
        assert exc.code != 0
    else:
        raise AssertionError("--in-place and --output must be mutually exclusive")

    print("meteoris_recover_hdf5 CLI tests: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
