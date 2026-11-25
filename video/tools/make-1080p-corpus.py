#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate CPU-only 1080p H.264 qualification and rejection controls."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = [
        ('baseline', '1920x1080', 'baseline', 'ref=2:bframes=0:weightp=0', 75),
        ('main', '1920x1080', 'main', 'ref=3:bframes=2:b-pyramid=none', 75),
        ('high', '1920x1080', 'high', 'ref=4:bframes=3:open-gop=1', 100),
        ('high-slices4', '1920x1080', 'high', 'ref=4:bframes=3:slices=4', 75),
        ('high-slices64', '1920x1080', 'high', 'ref=4:bframes=3:slice-max-mbs=128', 25),
        ('reject-slices65', '1920x1080', 'high', 'ref=4:bframes=3:slice-max-mbs=127', 25),
        ('high-coded1088', '1920x1088', 'high', 'ref=4:bframes=3', 75),
        ('high-cropped', '1918x1078', 'high', 'ref=4:bframes=3:slices=4', 75),
        ('high-1440', '1440x1080', 'high', 'ref=4:bframes=3', 75),
        ('regression-720', '1280x720', 'high', 'ref=4:bframes=3:slices=4', 75),
        ('reject-width', '1936x1080', 'high', 'ref=1:bframes=0', 2),
        ('reject-height', '1920x1104', 'high', 'ref=1:bframes=0', 2),
    ]
    receipt = {'hardware_qualified': False, 'cases': {}}
    slice_counts = runpy.run_path(str(Path(__file__).with_name('make-stress-corpus.py')))['slice_counts']
    for name, size, profile, params, frames in cases:
        output = args.output / (name + '.mp4')
        cmd = ['ffmpeg', '-v', 'error', '-nostdin', '-f', 'lavfi', '-i',
               f'testsrc2=size={size}:rate=25', '-frames:v', str(frames),
               '-an', '-c:v', 'libx264', '-threads', '8', '-preset', 'medium',
               '-profile:v', profile, '-crf', '23', '-x264-params',
               params + ':keyint=50:scenecut=0', str(output)]
        subprocess.run(cmd, check=True, timeout=180)
        expected = 65 if name == 'reject-slices65' else 64 if name == 'high-slices64' else 4 if 'slices=4' in params else 1
        counts = slice_counts(output)
        if len(counts) != frames or set(counts) != {expected}:
            raise SystemExit(f'{name}: unexpected picture/slice counts {len(counts)}, {set(counts)}')
        receipt['cases'][name] = {'command': cmd, 'frames': frames,
            'slices_per_picture': expected,
            'sha256': hashlib.sha256(output.read_bytes()).hexdigest(),
            'expect_rejection': name.startswith('reject-')}
        (args.output / 'generation.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
