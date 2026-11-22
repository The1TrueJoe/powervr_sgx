#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression checks for qualification false positives; no hardware required."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    'verify_decode', Path(__file__).resolve().parents[1] / 'tools/verify-decode.py')
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


class QualificationTests(unittest.TestCase):
    def test_receipt_requires_complete_pixels_and_hardware(self):
        with tempfile.TemporaryDirectory() as directory:
            sw, hw = [Path(directory) / name for name in ('sw', 'hw')]
            pixels = bytes(1280 * 720 * 3 // 2)
            sw.write_bytes(pixels)
            hw.write_bytes(pixels)
            before = dict(completed_frames='10')
            after = dict(completed_frames='12', quarantined='N', session_active='0')
            self.assertTrue(verify.validate_outputs(sw, hw, 1, before, after))
            # Identical early EOF used to pass; even a partial frame must fail.
            self.assertFalse(verify.validate_outputs(sw, hw, 2, before, after))
            sw.write_bytes(pixels[:-1])
            hw.write_bytes(pixels[:-1])
            self.assertFalse(verify.validate_outputs(sw, hw, 1, before, after))
            sw.write_bytes(pixels)
            hw.write_bytes(pixels[:-1] + b'\x01')
            self.assertFalse(verify.validate_outputs(sw, hw, 1, before, after))
            hw.write_bytes(pixels)
            for change in [dict(completed_frames='10'), dict(quarantined='Y'),
                           dict(session_active='1')]:
                self.assertFalse(verify.validate_outputs(sw, hw, 1, before, after | change))
            sw.write_bytes(bytes(640*368*3//2))
            hw.write_bytes(sw.read_bytes())
            self.assertTrue(verify.validate_outputs(sw, hw, 1, before, after, 640, 368))
            self.assertFalse(verify.validate_outputs(sw, hw, 1, before, after))


if __name__ == '__main__':
    unittest.main()
