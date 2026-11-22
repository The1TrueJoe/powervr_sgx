#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fetch/check the content-pinned public DE2 reference files; never execute them."""
import hashlib
import argparse
import json
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--lock', type=Path, default=ROOT/'de2-sources.lock.json')
    args = ap.parse_args()
    record = json.loads(args.lock.read_text())
    for entry in record['files']:
        relative = Path(entry['path'])
        if relative.is_absolute() or '..' in relative.parts:
            raise SystemExit('invalid locked source path')
        output = ROOT/'sources/psb-video-de2'/relative
        if not output.exists():
            with urllib.request.urlopen(entry['url'], timeout=30) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != entry['sha256']:
                raise SystemExit('download SHA256 mismatch: ' + entry['path'])
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(data)
        if hashlib.sha256(output.read_bytes()).hexdigest() != entry['sha256']:
            raise SystemExit('local SHA256 mismatch: ' + entry['path'])
        print(entry['path'] + ': SHA256 OK')


if __name__ == '__main__':
    main()
