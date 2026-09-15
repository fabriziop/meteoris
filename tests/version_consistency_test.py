#!/usr/bin/env python3
"""Checks that package versioning has one source of truth."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class VersionConsistencyTest(unittest.TestCase):
    def test_version_file_is_semver(self) -> None:
        version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        self.assertRegex(version, r"^[0-9]+\.[0-9]+\.[0-9]+$")

    def test_current_version_not_duplicated_in_sources_or_docs(self) -> None:
        version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        roots = [ROOT / "CMakeLists.txt", ROOT / "src", ROOT / "tools", ROOT / "doc", ROOT / "README.md"]
        offenders = []
        for root in roots:
            paths = [root] if root.is_file() else root.rglob("*")
            for path in paths:
                if not path.is_file() or path.suffix in {".png", ".pyc"}:
                    continue
                try:
                    text = path.read_text(encoding="utf-8")
                except UnicodeDecodeError:
                    continue
                if version in text:
                    offenders.append(str(path.relative_to(ROOT)))
        self.assertEqual(offenders, [], "current VERSION is hard-coded in: " + ", ".join(offenders))

    def test_cmake_reads_version_file(self) -> None:
        text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('${CMAKE_SOURCE_DIR}/VERSION', text)
        self.assertIn('VERSION ${METEORIS_VERSION}', text)
        self.assertIn('src/version.hpp.in', text)

    def test_cpp_uses_generated_version_header(self) -> None:
        text = (ROOT / "src" / "meteoris.cpp").read_text(encoding="utf-8")
        self.assertIn('#include "version.hpp"', text)
        self.assertNotRegex(text, r'METEORIS_VERSION\s*=\s*"[0-9]')


if __name__ == "__main__":
    unittest.main()
