#!/usr/bin/env python3
"""Regression test for the unified build.sh -> install.sh workflow."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(cmd, *, cwd, env, input_text=None):
    return subprocess.run(
        cmd,
        cwd=cwd,
        env=env,
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=True,
    )


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        work = Path(td) / "repo"
        shutil.copytree(ROOT, work, ignore=shutil.ignore_patterns(".git", "build-*", "build"))
        fakebin = Path(td) / "bin"
        fakebin.mkdir()
        log = Path(td) / "cmake.log"

        fake_cmake = fakebin / "cmake"
        fake_cmake.write_text(
            """#!/usr/bin/env bash
set -eu
printf '%s\\n' \"$*\" >> \"$FAKE_CMAKE_LOG\"
if [[ \" ${*} \" == *\" -S \"* && \" ${*} \" == *\" -B \"* ]]; then
  b=\"\"
  plot=ON
  sim=ON
  prev=\"\"
  for a in \"$@\"; do
    if [[ \"$prev\" == \"-B\" ]]; then b=\"$a\"; fi
    case \"$a\" in
      -DMETEORIS_INSTALL_PLOT=OFF) plot=OFF ;;
      -DMETEORIS_BUILD_SIM=OFF) sim=OFF ;;
    esac
    prev=\"$a\"
  done
  mkdir -p \"$b\"
  printf 'METEORIS_INSTALL_PLOT:BOOL=%s\\nMETEORIS_BUILD_SIM:BOOL=%s\\n' \"$plot\" \"$sim\" > \"$b/CMakeCache.txt\"
fi
"""
        )
        fake_cmake.chmod(0o755)

        env = os.environ.copy()
        env["PATH"] = f"{fakebin}:{env['PATH']}"
        env["FAKE_CMAKE_LOG"] = str(log)

        out = run(["./build.sh", "--recorder-only", "--native"], cwd=work, env=env).stdout
        assert "Build mode:  recorder-only" in out
        marker = work / ".meteoris-last-build"
        assert marker.is_file()
        build_dir = Path(marker.read_text().strip())
        assert build_dir.name == "build-recorder-only"

        before = log.read_text().splitlines()
        out = run(
            ["./install.sh", "--system", "--prefix", str(Path(td) / "prefix")],
            cwd=work,
            env=env,
        ).stdout
        after = log.read_text().splitlines()
        new = after[len(before):]
        assert "Installing existing build without rebuilding" in out
        assert len(new) == 1, new
        assert new[0].startswith("--install "), new
        assert not any("--build" in line for line in new)
        assert not any(" -S " in f" {line} " for line in new)

        # Interactive menu defaults to full on an empty answer.
        marker.unlink()
        out = run(["./build.sh", "--native"], cwd=work, env=env, input_text="\n").stdout
        assert "Build mode:  full" in out
        assert Path((work / ".meteoris-last-build").read_text().strip()).name == "build-full"

    print("build/install workflow test: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
