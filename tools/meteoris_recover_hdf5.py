#!/usr/bin/env python3
"""
.+
.context    : Meteoris triggered PSD recorder
.title      : Conservative recovery helper for uncleanly closed HDF5 files
.kind       : Python maintenance program
.author     : Meteoris contributors
.license    : MIT (see LICENSE file)
.description
Recover a Meteoris HDF5 file after a crash or forced shutdown by automating the
safe subset of doc/RECOVER_HDF5.md. The original file is never modified.
.-
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

__version__ = "0.5.1"


class RecoveryError(RuntimeError):
    pass


@dataclass
class CommandResult:
    args: list[str]
    returncode: int
    stdout: str
    stderr: str


def run_command(args: Sequence[str]) -> CommandResult:
    proc = subprocess.run(
        list(args),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    return CommandResult(list(args), proc.returncode, proc.stdout, proc.stderr)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_tools(names: Sequence[str]) -> None:
    missing = [name for name in names if shutil.which(name) is None]
    if missing:
        raise RecoveryError(
            "Missing required HDF5 command-line tool(s): " + ", ".join(missing) + "\n"
            "Install the HDF5 tools package for your distribution (for example, "
            "hdf5-tools on Debian/Ubuntu)."
        )


def active_writer_evidence(path: Path) -> list[str]:
    evidence: list[str] = []

    lsof = shutil.which("lsof")
    if lsof is not None:
        result = run_command([lsof, "--", str(path)])
        if result.returncode == 0:
            output = (result.stdout + result.stderr).strip()
            if output:
                evidence.append(f"lsof: {output}")

    fuser = shutil.which("fuser")
    if fuser is not None:
        # psmisc fuser does not accept the conventional `--` separator.
        result = run_command([fuser, str(path)])
        if result.returncode == 0:
            output = (result.stdout + result.stderr).strip()
            if output:
                evidence.append(f"fuser: {output}")

    return evidence


def stale_writer_error(text: str) -> bool:
    lowered = text.lower()
    markers = (
        "already open for write",
        "open for write/swmr write",
        "file consistency flags",
        "may use <h5clear file>",
    )
    return any(marker in lowered for marker in markers)


def corruption_error(text: str) -> bool:
    lowered = text.lower()
    markers = (
        "object header",
        "b-tree",
        "btree",
        "heap",
        "checksum",
        "bad address",
        "address overflow",
        "file signature not found",
        "not an hdf5 file",
        "truncated file",
    )
    return any(marker in lowered for marker in markers)


def parse_filesize_output(text: str) -> tuple[int | None, int | None]:
    values: dict[str, int] = {}
    for name in ("EOA", "EOF"):
        match = re.search(rf"\b{name}\b\s*[:=]\s*(\d+)", text, re.IGNORECASE)
        if match:
            values[name] = int(match.group(1))
    return values.get("EOA"), values.get("EOF")


def test_readable(path: Path, *, error_stack: bool = False) -> CommandResult:
    args = ["h5dump"]
    if error_stack:
        args.append("--enable-error-stack=2")
    args.extend(["-H", "--", str(path)])
    return run_command(args)


def inventory(path: Path, inventory_path: Path) -> CommandResult:
    result = run_command(["h5ls", "-r", "--", str(path)])
    if result.returncode == 0:
        inventory_path.write_text(result.stdout, encoding="utf-8")
    return result


def write_diagnostics(directory: Path, phase: str, result: CommandResult) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f"{phase}.stdout.txt").write_text(result.stdout, encoding="utf-8")
    (directory / f"{phase}.stderr.txt").write_text(result.stderr, encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Recover an HDF5 file left unclean after a crash. The input is never "
            "modified; recovery is performed on a copy using h5clear."
        )
    )
    parser.add_argument("file", type=Path, help="HDF5 file to recover")
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="recovery-copy path (default: FILE.recovery)",
    )
    parser.add_argument(
        "--no-increment",
        action="store_true",
        help="do not repair a confirmed EOA/EOF mismatch with h5clear --increment",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="replace an existing recovery output (never replaces the input file)",
    )
    return parser


def recover(source: Path, output: Path, *, allow_increment: bool, force: bool) -> int:
    source = source.expanduser().resolve()
    output = output.expanduser().resolve()

    if not source.exists():
        raise RecoveryError(f"Input file does not exist: {source}")
    if not source.is_file():
        raise RecoveryError(f"Input path is not a regular file: {source}")
    if source == output:
        raise RecoveryError("Recovery output must differ from the input file.")
    if output.exists() and not force:
        raise RecoveryError(
            f"Recovery output already exists: {output}\n"
            "Choose another path or use --force to replace that recovery copy."
        )

    require_tools(("h5clear", "h5dump", "h5ls"))

    evidence = active_writer_evidence(source)
    if evidence:
        raise RecoveryError(
            "Refusing recovery because the input may still be open by a process:\n  "
            + "\n  ".join(evidence)
        )

    before = source.stat()
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    shutil.copy2(source, output)
    after = source.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        output.unlink(missing_ok=True)
        raise RecoveryError("Input changed while it was being copied; refusing recovery.")

    source_hash = sha256(source)
    copy_hash = sha256(output)
    if source_hash != copy_hash:
        output.unlink(missing_ok=True)
        raise RecoveryError("Recovery copy checksum does not match the input.")

    diagnostics = output.parent / f"{output.name}.diagnostics"
    inventory_path = diagnostics / "inventory.txt"
    print(f"Input:    {source}")
    print(f"Recovery: {output}")
    print(f"SHA-256:  {source_hash}")

    initial = test_readable(output)
    write_diagnostics(diagnostics, "h5dump-initial", initial)
    if initial.returncode == 0:
        inv = inventory(output, inventory_path)
        write_diagnostics(diagnostics, "h5ls-final", inv)
        if inv.returncode != 0:
            raise RecoveryError("File opens, but h5ls inventory failed; see diagnostics.")
        print("Result: file was already structurally readable; no HDF5 metadata was changed.")
        print(f"Inventory: {inventory_path}")
        return 0

    # The default h5dump error is often only "unable to open file".  Ask HDF5
    # for its full error stack before deciding whether mutation is justified.
    detailed = test_readable(output, error_stack=True)
    write_diagnostics(diagnostics, "h5dump-error-stack", detailed)
    diagnostic_error = (
        initial.stderr
        + "\n"
        + initial.stdout
        + "\n"
        + detailed.stderr
        + "\n"
        + detailed.stdout
    )
    if corruption_error(diagnostic_error) and not stale_writer_error(diagnostic_error):
        raise RecoveryError(
            "The failure looks like corruption rather than a stale writer flag. "
            f"No h5clear mutation was attempted. See {diagnostics}."
        )
    if not stale_writer_error(diagnostic_error):
        raise RecoveryError(
            "The HDF5 error stack does not identify a stale write/SWMR consistency flag. "
            f"No automated mutation was attempted. See {diagnostics}."
        )

    print("Detected a stale HDF5 write/SWMR consistency flag; clearing it on the recovery copy.")
    cleared = run_command(["h5clear", "--status", str(output)])
    write_diagnostics(diagnostics, "h5clear-status", cleared)
    if cleared.returncode != 0:
        raise RecoveryError(f"h5clear --status failed; see {diagnostics}.")

    after_status = test_readable(output)
    write_diagnostics(diagnostics, "h5dump-after-status", after_status)
    if after_status.returncode == 0:
        inv = inventory(output, inventory_path)
        write_diagnostics(diagnostics, "h5ls-final", inv)
        if inv.returncode != 0:
            raise RecoveryError("Recovered metadata opens, but h5ls inventory failed; see diagnostics.")
        print("Result: recovered by clearing the stale writer flag.")
        print(f"Inventory: {inventory_path}")
        return 0

    post_status_error = after_status.stderr + "\n" + after_status.stdout
    if corruption_error(post_status_error):
        raise RecoveryError(
            "After clearing the stale writer flag, HDF5 reports metadata corruption. "
            f"Stopping without further mutation. See {diagnostics}."
        )

    filesize = run_command(["h5clear", "--filesize", str(output)])
    write_diagnostics(diagnostics, "h5clear-filesize", filesize)
    eoa, eof = parse_filesize_output(filesize.stdout + "\n" + filesize.stderr)
    if filesize.returncode != 0 or eoa is None or eof is None:
        raise RecoveryError(
            "File is still unreadable and EOA/EOF could not be confirmed. "
            f"Stopping without --increment. See {diagnostics}."
        )
    print(f"HDF5 address check: EOA={eoa}, EOF={eof}")
    if eoa == eof:
        raise RecoveryError(
            "File is still unreadable but EOA and EOF match; --increment is not justified. "
            f"See {diagnostics}."
        )
    if not allow_increment:
        raise RecoveryError(
            "Confirmed an EOA/EOF mismatch, but --no-increment was requested. "
            f"Recovery copy is preserved at {output}."
        )

    before_increment = output.with_name(output.name + ".before-increment")
    if before_increment.exists():
        if force:
            before_increment.unlink()
        else:
            raise RecoveryError(
                f"Safety copy already exists: {before_increment}. Remove it or use --force."
            )
    shutil.copy2(output, before_increment)
    print(f"Preserved pre-increment copy: {before_increment}")

    incremented = run_command(["h5clear", "--increment", str(output)])
    write_diagnostics(diagnostics, "h5clear-increment", incremented)
    if incremented.returncode != 0:
        raise RecoveryError(f"h5clear --increment failed; see {diagnostics}.")

    final = test_readable(output)
    write_diagnostics(diagnostics, "h5dump-final", final)
    if final.returncode != 0:
        raise RecoveryError(
            "The recovery copy is still unreadable after the confirmed EOA/EOF repair. "
            f"Preserve all copies and inspect {diagnostics}."
        )

    inv = inventory(output, inventory_path)
    write_diagnostics(diagnostics, "h5ls-final", inv)
    if inv.returncode != 0:
        raise RecoveryError("Recovered metadata opens, but h5ls inventory failed; see diagnostics.")
    print("Result: recovered stale writer state and repaired the EOA/EOF mismatch.")
    print(f"Inventory: {inventory_path}")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    source: Path = args.file
    output = args.output if args.output is not None else Path(str(source) + ".recovery")
    try:
        return recover(
            source,
            output,
            allow_increment=not args.no_increment,
            force=args.force,
        )
    except RecoveryError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
