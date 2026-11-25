#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check that stream qualification cannot accept missing/corrupted frames."""
import importlib.util
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest

spec = importlib.util.spec_from_file_location('stream', Path(__file__).resolve().parents[1] / 'tools/test-h264-stream.py')
stream = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stream)


class StreamTests(unittest.TestCase):
    def test_real_slice_boundaries_and_emulation_prevention(self):
        data = b'\x00\x00\x00\x01\x67\xff'  # SPS, not a picture
        data += b'\x00\x00\x01\x65\x80'     # first_mb = 0
        data += b'\x00\x00\x01\x41\x40'     # first_mb = 1, same picture
        data += b'\x00\x00\x00\x01\x41\x80'  # next picture
        offsets, counts = stream.picture_layout(data)
        self.assertEqual(counts, [2, 1])
        self.assertEqual(len(offsets), 2)
        self.assertEqual(stream.first_mb(b'\x41\x00\x00\x03\x80\x00\x00'), 65535)
        with self.assertRaises(ValueError):
            stream.picture_layout(b'\x00\x00\x01\x41\x40')
        with self.assertRaises(ValueError):
            stream.first_mb(b'\x41\x00')

    def test_pipe_stall_has_a_deadline(self):
        process = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(10)'],
                                   stdin=subprocess.PIPE)
        try:
            with self.assertRaises(subprocess.TimeoutExpired):
                stream.feed(process, bytes(1024*1024), .05)
        finally:
            process.kill()
            process.wait()
            process.stdin.close()

    def test_full_frame_count_timestamp_and_hash_required(self):
        a = [('0','0','0','1','384','a'*64)]
        self.assertTrue(stream.matching_frames(a, a, 1, 384))
        self.assertFalse(stream.matching_frames(a, a, 2, 384))
        self.assertFalse(stream.matching_frames(a, a, 1, 768))
        for row in [('0','1','1','1','384','a'*64), ('0','0','0','1','384','b'*64)]:
            self.assertFalse(stream.matching_frames(a, [row], 1, 384))
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)/'frames'
            p.write_text('# header\n0, 0, 0, 1, 384, not-a-hash\n')
            with self.assertRaises(ValueError):
                stream.hash_rows(p)


if __name__ == '__main__':
    unittest.main()
