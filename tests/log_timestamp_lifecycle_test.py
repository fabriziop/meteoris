"""Regression checks for UTC log timestamps and lifecycle markers."""
from pathlib import Path
import re
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'src' / 'meteoris.cpp'

class LoggingFormatTest(unittest.TestCase):
    def test_utc_timestamp_both_sinks(self):
        text = SOURCE.read_text()
        self.assertIn('%m%d%H%M%S.%e [%l] %v', text)
        self.assertEqual(text.count('spdlog::pattern_time_type::utc'), 2)

    def test_lifecycle_on_success_and_error(self):
        text = SOURCE.read_text()
        self.assertIn('logLifecycle("START", "OS")', text)
        self.assertIn('logLifecycle("STOP", "OS")', text)
        self.assertIn('logLifecycle("STOP", "ERROR")', text)
        self.assertIn('%a %b %d %H:%M:%S %Y UTC', text)

if __name__ == '__main__':
    unittest.main()
