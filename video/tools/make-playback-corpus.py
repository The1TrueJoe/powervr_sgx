#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate short 720p H.264 compatibility probes, including unsupported syntax.

Encoding is CPU-only. A successfully generated clip makes no support claim.
generation.json records exact commands, input identity and packet bounds.
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
    cases = {
        'baseline': 'ref=2:bframes=0:weightp=0:cabac=0:8x8dct=0',
        'main-b2': 'ref=2:bframes=2:b-pyramid=none:weightp=0:8x8dct=0',
        'high-b3': 'ref=4:bframes=3:b-pyramid=none:weightp=0',
        'high-default': 'ref=3:bframes=3',
        'high-open-gop': 'ref=3:bframes=3:open-gop=1',
        'high-weighted': 'ref=3:bframes=0:weightp=2',
        'testsrc-b-pyramid': 'ref=3:bframes=3',
    }
    receipt = dict(input_sha256=hashlib.sha256(args.input.read_bytes()).hexdigest(),
                   ffmpeg_version=subprocess.check_output(['ffmpeg', '-version'], text=True).splitlines()[0],
                   cases={})
    for name, controls in cases.items():
        output = args.output / (name + '.mp4')
        source = ['-f', 'lavfi', '-i', 'testsrc2=size=1280x720:rate=25'] if name.startswith('testsrc') else ['-i', str(args.input)]
        cmd = ['ffmpeg', '-nostdin', '-v', 'error', *source, '-t', '5', '-an',
               '-c:v', 'libx264', '-threads', '2', '-preset', 'fast', '-crf',
               '32' if name.startswith('testsrc') else '30', '-maxrate', '700k',
               '-bufsize', '320k', '-x264-params', controls + ':keyint=50:scenecut=0', str(output)]
        subprocess.run(cmd, check=True, timeout=120)
        packets = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams',
            'v:0', '-show_entries', 'packet=size', '-of', 'json', str(output)]))['packets']
        largest = max(int(packet['size']) for packet in packets)
        receipt['cases'][name] = dict(command=cmd, sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                                      packets=len(packets), max_packet_bytes=largest)
        (args.output / 'generation.json').write_text(json.dumps(receipt, indent=2) + '\n')
        if largest > 65535:
            raise SystemExit(f'{name}: exceeds slice transport limit; do not use as positive control')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
