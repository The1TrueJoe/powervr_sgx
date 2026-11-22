#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare mpv GPU-rendered RGB output from direct VA import and VA copy.

Run in the active Wayland session with the intended graphics stack and player
on PATH. Screenshots use grim to capture the compositor output. This avoids mpv's
software screenshot fallback on GLES2. Hide unrelated windows on other outputs
or select one output with --output-name.
"""
import argparse
import importlib.util
import json
import re
from pathlib import Path
import subprocess
import tempfile
import time

spec = importlib.util.spec_from_file_location('cedar_player', Path(__file__).with_name('test-player.py'))
player = importlib.util.module_from_spec(spec)
spec.loader.exec_module(player)


def capture(args, mode):
    log = args.output / (mode + '.log')
    png = args.output / (mode + '.png')
    report = dict(mode=mode, before=player.counters())
    with tempfile.TemporaryDirectory(prefix='cedar-colors-') as temporary:
        endpoint = Path(temporary) / 'mpv.sock'
        command = [args.launcher, '--hwdec=' + mode, '--audio=no',
                   '--pause', '--keep-open=yes', '--idle=yes', '--fullscreen', '--osd-level=0',
                   '--cursor-autohide=always', '--vd-queue-enable=no',
                   '--start=' + str(args.seek), '--dither-depth=no',
                   '--msg-level=all=warn,vo/gpu=debug', '--log-file=' + str(log),
                   '--input-ipc-server=' + str(endpoint), str(args.input.resolve())]
        report['command'] = command
        terminal = (args.output / (mode + '.terminal.log')).open('wb')
        process = subprocess.Popen(command, stdout=terminal, stderr=subprocess.STDOUT)
        ipc = None
        try:
            player.wait_for(endpoint.exists, process, seconds=60)
            ipc = player.IPC(endpoint)
            ipc.sock.settimeout(60)
            player.wait_for(lambda: ipc.command('get_property', 'hwdec-current') == mode and
                            ipc.command('get_property', 'vo-configured'), process, seconds=60)
            player.wait_for(lambda: ipc.command('get_property', 'time-pos') is not None,
                            process, seconds=60)
            report['time_pos'] = ipc.command('get_property', 'time-pos')
            # Allow the paused frame's Wayland commit to reach the compositor.
            time.sleep(.5)
            command = ['grim'] + (['-o', args.output_name] if args.output_name else []) + [str(png)]
            subprocess.run(command, check=True, timeout=30)
            player.wait_for(png.exists, process, seconds=30)
            if ipc.command('get_property', 'hwdec-current') != mode:
                raise RuntimeError('hardware path changed during screenshot')
            ipc.command('quit')
            process.wait(timeout=30)
        finally:
            if ipc:
                ipc.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            terminal.close()
            for _ in range(100):
                report['after'] = player.counters()
                if report['after']['session_active'] == '0':
                    break
                time.sleep(.05)
    for name in ('session_active', 'surface_objects', 'surface_pages', 'bitstream_pages', 'engine_contexts'):
        if report['after'].get(name) != report['before'].get(name):
            raise RuntimeError('resource retirement failed: ' + name)
    if report['after']['quarantined'] != 'N':
        raise RuntimeError('decoder quarantined')
    summaries = (args.output / (mode + '.terminal.log')).read_text(errors='replace')
    downloads = [int(n) for n in re.findall(r'explicit_CPU_downloads=(\d+)', summaries)]
    report['decoder_cpu_downloads'] = sum(downloads) if downloads else None
    if mode == 'vaapi' and (not downloads or any(downloads)):
        raise RuntimeError('direct import did not report zero decoder CPU downloads')
    pixels = subprocess.check_output(['ffmpeg', '-nostdin', '-v', 'error', '-i', str(png),
                                      '-frames:v', '1', '-pix_fmt', 'rgb24', '-f', 'rawvideo', '-'])
    return report, pixels


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--seek', type=float, default=30)
    ap.add_argument('--launcher', default='cedar-mpv-wayland-gpu')
    ap.add_argument('--output-name', help='Wayland output to capture (for example LVDS-1)')
    args = ap.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    result = dict(passed=False)
    try:
        if player.counters()['session_active'] != '0':
            raise RuntimeError('decoder already active')
        direct, a = capture(args, 'vaapi')
        copied, b = capture(args, 'vaapi-copy')
        result.update(direct=direct, copied=copied)
        if abs(direct['time_pos'] - copied['time_pos']) > .0001:
            raise RuntimeError('screenshots selected different source frames')
        if not a or len(a) != len(b):
            raise RuntimeError('RGB screenshot sizes differ or are empty')
        differences = [abs(x-y) for x, y in zip(a, b)]
        result.update(compared_components=len(a), differing_components=sum(v != 0 for v in differences),
                      max_error=max(differences), mean_error=sum(differences) / len(a))
        result['passed'] = not any(differences)
    except Exception as error:
        result['error'] = str(error)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    if not result['passed']:
        raise SystemExit('GPU-rendered color comparison FAILED')


if __name__ == '__main__':
    main()
