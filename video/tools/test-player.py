#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded mpv seek/reload stress; run as the broker's desktop user.

This checks decoder selection, API progress and resource retirement, not pixels
or subjective presentation quality. Use verify-decode.py for exact pixels.
"""
import argparse
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time


def counters():
    root = Path('/sys/module/gma500_gfx/parameters')
    names = ['completed_frames', 'firmware_boots', 'quarantined', 'session_active',
             'surface_objects', 'surface_pages']
    names += [n for n in ('bitstream_pages', 'engine_contexts', 'forced_resets', 'forced_reset_failures',
                                'firmware_ready_waits', 'firmware_ready_timeouts',
                                'startup_retries', 'startup_retry_failures',
                                'sleep_checkpoints', 'resume_restarts', 'resume_failures') if (root / ('msvdx_' + n)).is_file()]
    return {name: (root / ('msvdx_' + name)).read_text().strip() for name in names}


class IPC:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(15)
        self.sock.connect(str(path))
        self.stream = self.sock.makefile('rwb', buffering=0)
        self.sequence = 0
        self.loaded = 0

    def command(self, *args):
        self.sequence += 1
        self.stream.write((json.dumps(dict(command=args, request_id=self.sequence)) + '\n').encode())
        while True:
            line = self.stream.readline()
            if not line:
                raise RuntimeError('mpv closed IPC')
            response = json.loads(line)
            if response.get('event') == 'file-loaded':
                self.loaded += 1
            if response.get('request_id') == self.sequence:
                if response.get('error') == 'property unavailable' and args[0] == 'get_property':
                    return None
                if response.get('error') != 'success':
                    raise RuntimeError(f'{args}: {response}')
                return response.get('data')

    def close(self):
        self.stream.close()
        self.sock.close()


def wait_for(check, process, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f'mpv exited early: {process.returncode}')
        if check():
            return
        time.sleep(.05)
    raise TimeoutError('mpv failed to complete operation')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--rounds', type=int, default=20,
                    help='seek/reload rounds; 0 starts paced playback directly without warm-up')
    ap.add_argument('--launcher', help='explicit presentation launcher; default follows --session')
    ap.add_argument('--hwdec', choices=['vaapi-copy', 'vaapi'], default='vaapi-copy',
                    help='require this hardware path throughout; vaapi tests direct GPU import')
    ap.add_argument('--vo', choices=['null', 'gpu', 'x11'], default='null')
    ap.add_argument('--session', choices=['x11', 'wayland'], default='x11',
                    help='select the matching installed presentation launcher')
    ap.add_argument('--fullscreen', action='store_true')
    ap.add_argument('--gpu-log', action='store_true', help='record GPU renderer identity and diagnostics')
    ap.add_argument('--terminal', action='store_true',
                    help='inherit the calling terminal and retain normal mpv status output')
    ap.add_argument('--play-seconds', type=int, default=0,
                    help='then loop normal paced playback for this many seconds (up to 3600)')
    ap.add_argument('--play-start', type=float, default=0,
                    help='start sustained playback at this source time in seconds')
    ap.add_argument('--audio', action='store_true', help='decode/play audio, still muted by cedar-mpv')
    ap.add_argument('--fast', action='store_true', help='select mpv fast rendering profile')
    ap.add_argument('--alternate', type=Path, nargs='+', default=[],
                    help='cycle these inputs on reload to test resolution/profile changes')
    args = ap.parse_args()
    if not 0 <= args.rounds <= 100:
        ap.error('--rounds must be 0..100')
    if not 0 <= args.play_seconds <= 3600:
        ap.error('--play-seconds must be 0..3600')
    if not args.rounds and not args.play_seconds:
        ap.error('--rounds=0 requires --play-seconds for a cold-start playback check')
    if not 0 <= args.play_start < float('inf') or (args.play_start and not args.play_seconds):
        ap.error('--play-start must be finite, nonnegative and used with --play-seconds')
    if args.session == 'wayland' and args.vo == 'x11':
        ap.error('native Wayland tests require --vo=gpu or --vo=null')
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(passed=False, vo=args.vo, session=args.session, hwdec=args.hwdec,
                  fullscreen=args.fullscreen, terminal=args.terminal,
                  play_start=args.play_start, cold_start=not args.rounds, rounds=[], before=counters())
    if report['before']['session_active'] != '0' or report['before']['quarantined'] != 'N':
        raise SystemExit('stop competing decoder clients and recover quarantine first')
    with tempfile.TemporaryDirectory(prefix='cedar-player-') as temporary:
        endpoint = Path(temporary) / 'mpv.sock'
        launcher = args.launcher or ('cedar-mpv-wayland-gpu' if args.session == 'wayland' else 'cedar-mpv')
        cmd = [launcher, '--hwdec=' + args.hwdec, '--idle=yes', '--pause', '--vo=' + args.vo,
               '--input-ipc-server=' + str(endpoint),
               str(args.input.resolve())]
        if args.gpu_log:
            cmd.insert(1, '--msg-level=all=warn,vo/gpu=debug')
        elif not args.terminal:
            cmd.insert(1, '--msg-level=all=warn')
        if not args.rounds:
            cmd.remove('--pause')
            if args.play_start:
                cmd.insert(1, '--start=' + str(args.play_start))
        if args.fullscreen:
            cmd.insert(1, '--fullscreen')
        if not args.audio:
            cmd.insert(1, '--no-audio')
        if args.fast:
            cmd.insert(1, '--profile=fast')
        if args.play_seconds:
            cmd.insert(1, '--loop-file=inf')
        if args.terminal:
            cmd.insert(1, '--terminal=yes')
            cmd.insert(1, '--log-file=' + str(args.output / 'mpv.log'))
        report['command'] = cmd
        ipc = None
        with (args.output / 'mpv.log').open('wb') as log:
            process = subprocess.Popen(cmd, stdout=None if args.terminal else log,
                                       stderr=None if args.terminal else subprocess.STDOUT)
            launch_started = time.monotonic()
            try:
                wait_for(endpoint.exists, process, seconds=60)
                ipc = IPC(endpoint)
                # A real cold start on this Atom can spend over 15 seconds in
                # display, audio and libva initialization before serving IPC.
                # Keep runtime commands bounded more tightly after selection.
                ipc.sock.settimeout(60)
                wait_for(lambda: ipc.command('get_property', 'idle-active') is False, process, seconds=60)
                wait_for(lambda: ipc.command('get_property', 'hwdec-current') == args.hwdec, process, seconds=60)
                report['hardware_selection_seconds'] = time.monotonic() - launch_started
                ipc.sock.settimeout(15)
                duration = ipc.command('get_property', 'duration')
                inputs = [args.input, *args.alternate]
                current_input = args.input
                after = report['before']
                for iteration in range(args.rounds):
                    # Alternate directions and cross IDRs; reload every fifth operation.
                    before = counters()
                    if iteration % 5 == 4:
                        loaded = ipc.loaded
                        current_input = inputs[(iteration//5 + 1) % len(inputs)]
                        ipc.command('loadfile', str(current_input.resolve()), 'replace')
                        wait_for(lambda: ipc.command('get_property', 'hwdec-current') == args.hwdec and
                                 ipc.loaded > loaded, process)
                        duration = ipc.command('get_property', 'duration')
                    position = duration * [.1, .7, .25, .85, .4][iteration % 5]
                    ipc.command('seek', position, 'absolute+exact')
                    wait_for(lambda: not ipc.command('get_property', 'seeking') and
                             abs(ipc.command('get_property', 'time-pos') - position) < .15, process)
                    selected = ipc.command('get_property', 'hwdec-current')
                    after = counters()
                    if selected != args.hwdec or after['quarantined'] != 'N':
                        raise RuntimeError(f'decoder changed or quarantined: {selected}, {after}')
                    if int(after['completed_frames']) <= int(before['completed_frames']):
                        raise RuntimeError('seek produced no hardware completions')
                    report['rounds'].append(dict(position=position, before=before, after=after,
                                                input=str(current_input), hwdec=selected,
                                                video=ipc.command('get_property', 'video-params')))
                if args.play_seconds:
                    report['playback_samples'] = []
                    if args.play_start and (duration is None or args.play_start >= duration):
                        raise ValueError('--play-start is outside the final input duration')
                    if args.rounds:
                        ipc.command('seek', args.play_start, 'absolute+exact')
                        ipc.command('set_property', 'pause', False)
                    started = time.monotonic()
                    last_progress = started
                    completed = int(counters()['completed_frames'])
                    while time.monotonic() - started < args.play_seconds:
                        time.sleep(1)
                        if process.poll() is not None:
                            raise RuntimeError('mpv exited during sustained playback')
                        sample = dict(seconds=time.monotonic()-started, counters=counters())
                        for prop in ('hwdec-current', 'time-pos', 'frame-drop-count',
                                     'decoder-frame-drop-count', 'vo-delayed-frame-count',
                                     'mistimed-frame-count', 'display-sync-active',
                                     'estimated-display-fps', 'avsync', 'audio-pts'):
                            sample[prop] = ipc.command('get_property', prop)
                        report['playback_samples'].append(sample)
                        now_completed = int(sample['counters']['completed_frames'])
                        if now_completed > completed:
                            last_progress = time.monotonic()
                            completed = now_completed
                        if time.monotonic() - last_progress > 10:
                            raise RuntimeError('hardware decoding stalled for ten seconds')
                        # During a loop transition the properties may briefly be
                        # unavailable. Explicit software decoder selection fails.
                        if sample['hwdec-current'] not in (None, args.hwdec) or sample['counters']['quarantined'] != 'N':
                            raise RuntimeError('hardware decoder lost during sustained playback')
                    if int(counters()['completed_frames']) <= int(after['completed_frames']):
                        raise RuntimeError('sustained playback produced no hardware completions')
                ipc.command('quit')
                process.wait(timeout=15)
                if process.returncode:
                    raise RuntimeError(f'mpv exited with {process.returncode}')
                report['passed'] = True
            except Exception as error:
                report['error'] = str(error)
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
                # Broker releases the session asynchronously after socket EOF.
                for _ in range(100):
                    report['after'] = counters()
                    if report['after']['session_active'] == '0':
                        break
                    time.sleep(.05)
                for name in ('session_active', 'surface_objects', 'surface_pages', 'bitstream_pages', 'engine_contexts'):
                    if report['after'].get(name) != report['before'].get(name):
                        report['passed'] = False
                        report['retirement_error'] = name
                if report['after']['quarantined'] != 'N':
                    report['passed'] = False
    report['decode_errors'] = [line for line in (args.output / 'mpv.log').read_text(errors='replace').splitlines()
        if re.search(r'Failed to end picture|hardware accelerator failed|picture submission failed|'
                     r'unsupported picture|Error while decoding|Error decoding', line, re.I)]
    if report['decode_errors']:
        report['passed'] = False
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise SystemExit('player stress FAILED')


if __name__ == '__main__':
    main()
