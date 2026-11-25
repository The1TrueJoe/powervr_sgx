#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check CPU-written MSVDX probe imports and idle GPU retirement.

Run as root with no decoder client and with the intended Mesa/libdrm environment.
Requires the hardware plane sampler. Each probe has separate writable storage;
no decoder work, module installation or power-management setting is requested.
"""
import argparse
import fcntl
import json
import mmap
import os
from pathlib import Path
import struct
import subprocess
import time

PARAMS = Path('/sys/module/gma500_gfx/parameters')
GET_CAPS = 0x80404301
EXPORT_BLANK = 0xc0404305
DMA_BUF_SYNC = 0x40086200
WRITABLE_EXPORT = 1 << 20
SYNC_WRITE = 2
SYNC_END = 4


def counter(name):
    return (PARAMS / ('msvdx_' + name)).read_text().strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--probe', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--rounds', type=int, default=16)
    ap.add_argument('--retire-timeout', type=float, default=3)
    args = ap.parse_args()
    if os.geteuid() != 0:
        ap.error('requires root access to /dev/cedar-msvdx')
    if not 1 <= args.rounds <= 128 or not 0 < args.retire_timeout <= 30:
        ap.error('rounds must be 1..128 and retire timeout must be 0..30 seconds')
    args.probe = args.probe.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    result = dict(passed=False, rounds=[])
    try:
        for index in range(args.rounds):
            if counter('quarantined') != 'N' or counter('session_active') != '0':
                raise RuntimeError('decoder must be idle and not quarantined')
            if counter('surface_objects') != '0':
                raise RuntimeError('preexisting surface storage has not retired')
            device = image = -1
            try:
                device = os.open('/dev/cedar-msvdx', os.O_RDWR | os.O_CLOEXEC)
                caps = bytearray(64)
                fcntl.ioctl(device, GET_CAPS, caps, True)
                if not struct.unpack_from('<I', caps, 52)[0] & WRITABLE_EXPORT:
                    raise RuntimeError('kernel does not advertise writable exports')
                request = bytearray(64)
                struct.pack_into('<IIi', request, 0, 1, 0, -1)
                struct.pack_into('<II', request, 24, 64, 32)
                fcntl.ioctl(device, EXPORT_BLANK, request, True)
                image = struct.unpack_from('<i', request, 8)[0]
                width, height, pitch, uv = struct.unpack_from('<IIII', request, 24)
                size = struct.unpack_from('<I', request, 44)[0]
                if not 0 < size <= 4 * 1024 * 1024 or uv + 2 > size:
                    raise RuntimeError('invalid exported mapping layout')
                with mmap.mmap(image, size, flags=mmap.MAP_SHARED,
                               prot=mmap.PROT_READ | mmap.PROT_WRITE) as pixels:
                    fcntl.ioctl(image, DMA_BUF_SYNC, struct.pack('<Q', SYNC_WRITE))
                    pixels[0] = 33 + index
                    pixels[uv] = 35 + index
                    pixels[uv + 1] = 200 - index
                    fcntl.ioctl(image, DMA_BUF_SYNC, struct.pack('<Q', SYNC_END | SYNC_WRITE))
                with (args.output / f'round-{index:03}.log').open('w') as log:
                    run = subprocess.run([str(args.probe), str(image), str(width),
                                          str(height), str(pitch), str(uv)],
                                         pass_fds=(image,), stdout=log,
                                         stderr=subprocess.STDOUT, timeout=45)
                if run.returncode:
                    raise RuntimeError(f'GPU sampling failed in round {index}')
            finally:
                if image >= 0:
                    os.close(image)
                if device >= 0:
                    os.close(device)
            start = time.monotonic()
            while counter('surface_objects') != '0':
                if counter('quarantined') != 'N':
                    raise RuntimeError('decoder quarantined')
                if time.monotonic() - start >= args.retire_timeout:
                    raise RuntimeError(f'GPU import storage did not retire in round {index}')
                time.sleep(.01)
            row = dict(round=index, retirement_seconds=round(time.monotonic() - start, 3))
            result['rounds'].append(row)
            print(json.dumps(row), flush=True)
        result['passed'] = True
    except Exception as error:
        result['error'] = str(error)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if not result['passed']:
        raise SystemExit(result['error'])


if __name__ == '__main__':
    main()
