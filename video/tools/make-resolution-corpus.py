#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Make bounded H.264 geometry probes and out-of-bounds rejection controls.

All encoding is CPU-only. generation.json pins commands, hashes and packet
bounds. Generation does not establish hardware support for any case.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    ap.add_argument('--input', type=Path, default=Path(__file__).resolve().parents[1] /
                    'demo/big-buck-bunny-720p-safe-h264.mp4')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = [(16,16), (160,96), (426,240), (640,360), (854,480), (960,540),
             (1024,576), (360,640), (1278,718), (1280,720), (1282,720), (1280,722), (1440,1080), (1918,1078), (1920,1080),
             (1920,1088), (1922,1080), (1936,1080), (1920,1104)]
    receipt = dict(input_sha256=hashlib.sha256(args.input.read_bytes()).hexdigest(), cases={})
    for width, height in cases:
        name = f'{width}x{height}'
        negative = ((width + 15) & ~15) > 1920 or ((height + 15) & ~15) > 1088
        frames = 2 if negative else 75
        output = args.output / (name + '.mp4')
        cmd = ['ffmpeg', '-nostdin', '-v', 'error', '-i', str(args.input), '-vf', f'scale={width}:{height}',
               '-frames:v', str(frames), '-an', '-c:v', 'libx264', '-threads', '2', '-preset', 'fast',
               '-crf', '32', '-maxrate', '700k', '-bufsize', '320k', '-x264-params',
               'ref=3:bframes=3:open-gop=1:keyint=25:scenecut=0', str(output)]
        subprocess.run(cmd, check=True, timeout=120)
        packets = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
            '-show_entries', 'packet=size', '-of', 'json', str(output)]))['packets']
        largest = max(int(packet['size']) for packet in packets)
        receipt['cases'][name] = dict(command=cmd, sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                                     frames=frames, max_packet_bytes=largest, expect_rejection=negative)
        (args.output / 'generation.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if largest > 65535:
            raise SystemExit(f'{name}: slice transport limit exceeded; geometry control is confounded')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
