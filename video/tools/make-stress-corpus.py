#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate large-slice and multislice H.264 probes; encoding makes no support claim."""
import argparse
import hashlib
import json
import re
from pathlib import Path
import subprocess


def slice_counts(path):
    data = subprocess.check_output(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(path),
        '-c:v', 'copy', '-bsf:v', 'h264_mp4toannexb', '-f', 'h264', '-'])
    starts = list(re.finditer(b'\x00\x00(?:\x00)?\x01', data))
    counts = []
    for i, start in enumerate(starts):
        end = starts[i+1].start() if i+1 < len(starts) else len(data)
        nal = data[start.end():end]
        if not nal or nal[0] & 31 not in (1, 5):
            continue
        rbsp = nal[1:].replace(b'\x00\x00\x03', b'\x00\x00')
        bits = ''.join(f'{v:08b}' for v in rbsp)
        zeros = bits.index('1')
        first_mb = int(bits[zeros:2*zeros+1], 2) - 1
        if not first_mb:
            counts.append(0)
        assert counts, 'missing first slice'
        counts[-1] += 1
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = {
        'noisy-high': ('1280x720', 32, 'ref=3:bframes=3', True),
        'baseline-slices4': ('1280x720', 50, 'ref=2:bframes=0:weightp=0:cabac=0:8x8dct=0:slices=4', False),
        'main-slices4': ('1280x720', 50, 'ref=2:bframes=2:b-pyramid=none:8x8dct=0:slices=4', False),
        'high-slices4': ('1280x720', 50, 'ref=3:bframes=3:open-gop=1:slices=4', False),
        'high-slices16': ('1280x720', 50, 'ref=3:bframes=3:slices=16', False),
        'wrap-slices64': ('1280x720', 1030, 'ref=3:bframes=3:slice-max-mbs=57', False),
        'high-slices64': ('1280x720', 25, 'ref=3:bframes=3:slice-max-mbs=57', False),
        'reject-slices65': ('1280x720', 25, 'ref=3:bframes=3:slice-max-mbs=56', False),
        'cropped-slices4': ('854x480', 50, 'ref=3:bframes=3:slices=4', False),
    }
    report = {'ffmpeg_version': subprocess.check_output(['ffmpeg', '-version'], text=True).splitlines()[0], 'cases': {}}
    for name, (size, frames, controls, noisy) in cases.items():
        source = 'testsrc2=size=' + size + ':rate=25'
        if noisy:
            source += ',noise=alls=20:allf=t+u:all_seed=1234'
        output = args.output / (name + '.mp4')
        cmd = ['ffmpeg', '-nostdin', '-v', 'error', '-f', 'lavfi', '-i', source,
               '-frames:v', str(frames), '-an', '-c:v', 'libx264', '-threads', '2',
               '-preset', 'fast', '-qp' if noisy else '-crf', '18' if noisy else '32',
               '-x264-params', controls + (':keyint=4096:scenecut=0' if name.startswith('wrap-') else ':keyint=25:scenecut=0'), str(output)]
        subprocess.run(cmd, check=True, timeout=180)
        packets = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
            '-show_entries', 'packet=size', '-of', 'json', str(output)]))['packets']
        largest = max(int(p['size']) for p in packets)
        counts = slice_counts(output)
        expected = 1 if noisy else 65 if name == 'reject-slices65' else 64 if name.endswith('slices64') else 16 if name == 'high-slices16' else 4
        if len(counts) != frames or set(counts) != {expected}:
            raise SystemExit(f'{name}: unexpected slice counts {sorted(set(counts))}')
        report['cases'][name] = dict(command=cmd, frames=len(packets), slices_per_picture=expected, expect_rejection=expected > 64,
            max_packet_bytes=largest, sha256=hashlib.sha256(output.read_bytes()).hexdigest())
        (args.output / 'generation.json').write_text(json.dumps(report, indent=2) + '\n')
        if len(packets) != frames or (noisy and not 65535 < largest <= 1024*1024):
            raise SystemExit(f'{name}: failed intended frame/packet bounds')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
