#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare a bounded H.264 stream using per-frame hashes, optionally across sleep.

No installation or decoder fallback. Normal checks run as the broker UID.
--suspend-after explicitly requests real system suspend and requires root plus
--uid. Compares normalized elementary-stream timestamps, not container PTS.
Raw captures stay in the selected output directory, outside committed evidence.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import selectors
import subprocess
import time

SPEC = importlib.util.spec_from_file_location('verify_decode', Path(__file__).with_name('verify-decode.py'))
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)
MAX_INPUT = 64 * 1024 * 1024


def first_mb(nal):
    """Read the first unsigned Exp-Golomb value, without parsing the whole slice."""
    rbsp = nal[1:10].replace(b'\x00\x00\x03', b'\x00\x00')
    bits = ''.join(f'{byte:08b}' for byte in rbsp)
    try:
        zeros = bits.index('1')
    except ValueError as error:
        raise ValueError('truncated first_mb_in_slice') from error
    if zeros > 31 or len(bits) < zeros * 2 + 1:
        raise ValueError('invalid first_mb_in_slice')
    return int(bits[zeros:zeros * 2 + 1], 2) - 1


def picture_layout(data):
    starts = list(re.finditer(b'\x00\x00(?:\x00)?\x01', data))
    offsets, counts = [], []
    for i, start in enumerate(starts):
        end = starts[i+1].start() if i+1 < len(starts) else len(data)
        nal = data[start.end():end]
        if not nal or nal[0] & 31 not in (1, 5):
            continue
        if first_mb(nal) == 0:
            offsets.append(start.start())
            counts.append(0)
        if not counts:
            raise ValueError('slice before the first picture')
        counts[-1] += 1
    return offsets, counts


def hash_rows(path):
    rows = []
    for line in path.read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        row = tuple(part.strip() for part in line.split(','))
        if len(row) != 6 or not re.fullmatch('[0-9a-fA-F]{64}', row[5]):
            raise ValueError('invalid SHA256 framehash record')
        rows.append(row)
    return rows


def matching_frames(sw, hw, frames, image_bytes):
    return (len(sw) == len(hw) == frames and sw == hw and
            all(int(row[4]) == image_bytes for row in hw))


def feed(process, data, timeout):
    """Bound pipe writes too: a stopped decoder must not hang the test harness."""
    fd = process.stdin.fileno()
    os.set_blocking(fd, False)
    deadline = time.monotonic() + timeout
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(fd, selectors.EVENT_WRITE)
            offset = 0
            view = memoryview(data)
            while offset < len(view):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(process.args, timeout)
                if selector.select(min(remaining, 1)):
                    try:
                        offset += os.write(fd, view[offset:offset+65536])
                    except BlockingIOError:
                        continue
    finally:
        os.set_blocking(fd, True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--frames', type=int, default=75)
    ap.add_argument('--expected-slices', type=int)
    ap.add_argument('--driver-dir', type=Path, default=Path('/opt/cedar-msvdx'))
    ap.add_argument('--socket', default='/run/cedar-decoder/decoder.sock')
    ap.add_argument('--uid', type=int, help='broker user; required when running as root')
    ap.add_argument('--suspend-after', type=int, help='explicitly suspend after feeding this many pictures')
    ap.add_argument('--suspend-seconds', type=int, default=20)
    ap.add_argument('--timeout', type=int, default=600)
    args = ap.parse_args()
    if not 1 <= args.frames <= 4096 or not 1 <= args.timeout <= 3600:
        ap.error('use 1..4096 frames and a 1..3600 second timeout')
    if args.expected_slices is not None and not 1 <= args.expected_slices <= 64:
        ap.error('--expected-slices must be 1..64')
    if os.geteuid() == 0 and args.uid is None:
        ap.error('root must explicitly select --uid for the broker client')
    uid = os.geteuid() if args.uid is None else args.uid
    if os.geteuid() != 0 and uid != os.geteuid():
        ap.error('changing the client uid requires root')
    if args.suspend_after is not None:
        if os.geteuid() != 0 or not 8 <= args.suspend_after <= args.frames - 8:
            ap.error('suspend requires root and at least eight pictures on both sides')
        if not 5 <= args.suspend_seconds <= 120:
            ap.error('--suspend-seconds must be 5..120')
    username = pwd.getpwuid(uid).pw_name
    as_user = ['runuser', '-u', username, '--'] if os.geteuid() == 0 else []
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    if os.geteuid() == 0:
        os.chown(args.output, uid, -1)
    report = {'passed': False, 'requested_frames': args.frames, 'client_uid': uid,
              'timestamp_method': 'Same raw Annex-B stream and normalized input frame rate for both decoders.'}
    process = None
    armed = False
    try:
        report['before'] = VERIFY.counters()
        if report['before']['quarantined'] != 'N' or report['before']['session_active'] != '0':
            raise RuntimeError('recover quarantine and stop competing decoder clients first')
        metadata = json.loads(subprocess.check_output(as_user + ['ffprobe', '-v', 'error', '-select_streams',
            'v:0', '-show_entries', 'stream=codec_name,width,height,avg_frame_rate', '-of', 'json',
            str(args.input.resolve())], timeout=args.timeout))['streams'][0]
        if metadata['codec_name'] != 'h264' or (metadata['width'] | metadata['height']) & 1:
            raise ValueError('use progressive H.264 with even NV12 dimensions')
        fps = metadata['avg_frame_rate']
        n, d = map(int, fps.split('/'))
        if n <= 0 or d <= 0:
            raise ValueError('input must declare a positive frame rate')
        report['metadata'] = metadata
        report['input_sha256'] = VERIFY.sha(args.input)
        raw = args.output / 'input.h264'
        extract = ['ffmpeg', '-nostdin', '-v', 'error', '-i', str(args.input.resolve()),
                   '-map', '0:v:0', '-frames:v', str(args.frames), '-an', '-c:v', 'copy',
                   '-bsf:v', 'h264_mp4toannexb', '-fs', str(MAX_INPUT), '-f', 'h264', str(raw)]
        with (args.output / 'extract.log').open('wb') as log:
            subprocess.run(as_user + extract, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=args.timeout)
        if raw.stat().st_size > MAX_INPUT:
            raise ValueError('compressed test input exceeds 64 MiB')
        data = raw.read_bytes()
        offsets, counts = picture_layout(data)
        if len(counts) != args.frames or (args.expected_slices and set(counts) != {args.expected_slices}):
            raise ValueError(f'wrong picture/slice counts: pictures={len(counts)}, slices={sorted(set(counts))}')
        report['slices_per_picture'] = sorted(set(counts))
        report['elementary_stream_sha256'] = VERIFY.sha(raw)
        common = ['ffmpeg', '-nostdin', '-v', 'warning', '-xerror']
        def tail(source, hardware):
            return ['-probesize', '4096', '-analyzeduration', '1', '-f', 'h264', '-r', fps,
                    '-i', source, '-frames:v', str(args.frames), '-an', '-vf',
                    'hwdownload,format=nv12' if hardware else 'format=nv12', '-f', 'framehash',
                    '-hash', 'sha256', str(args.output / ('hardware.framehash' if hardware else 'software.framehash'))]
        sw = as_user + common + ['-hwaccel', 'none'] + tail(str(raw), False)
        report['software_command'] = sw
        with (args.output / 'software.log').open('wb') as log:
            subprocess.run(sw, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=args.timeout)
        hw = ['env', 'LIBVA_DRIVER_NAME=cedar', 'LIBVA_DRIVERS_PATH='+str(args.driver_dir.resolve()),
              'CEDAR_DECODER_SOCKET='+args.socket, 'CEDAR_VA_DMABUF=1', *common,
              '-hwaccel', 'vaapi', '-vaapi_device', '/dev/dri/card0', '-hwaccel_output_format', 'vaapi',
              *tail('pipe:0', True)]
        hw = as_user + hw
        report['hardware_command'] = hw
        with (args.output / 'hardware.log').open('wb') as log:
            process = subprocess.Popen(hw, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
            if args.suspend_after is not None:
                cut = offsets[args.suspend_after]
                if cut < 4096:
                    raise ValueError('pre-suspend prefix must reach the probe size; feed more pictures')
                feed(process, data[:cut], args.timeout)
                deadline = time.monotonic() + 30
                while True:
                    parked = VERIFY.counters()
                    if parked['session_active'] == '1' and int(parked['completed_frames']) > int(report['before']['completed_frames']):
                        break
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('decoder did not establish a live reference chain')
                    time.sleep(.1)
                time.sleep(.5)
                report['parked'] = VERIFY.counters()
                report['mem_sleep'] = Path('/sys/power/mem_sleep').read_text().strip()
                boot = Path('/proc/sys/kernel/random/boot_id').read_text()
                armed = True
                with (args.output / 'suspend.log').open('wb') as wake:
                    subprocess.run(['rtcwake', '-m', 'mem', '-s', str(args.suspend_seconds)],
                                   stdout=wake, stderr=subprocess.STDOUT, check=True, timeout=180)
                if Path('/proc/sys/kernel/random/boot_id').read_text() != boot:
                    raise RuntimeError('boot identity changed across suspend')
                report['resumed'] = VERIFY.counters()
                feed(process, data[cut:], args.timeout)
            else:
                feed(process, data, args.timeout)
            process.stdin.close()
            process.wait(timeout=args.timeout)
            if process.returncode:
                raise RuntimeError(f'hardware decoder exited {process.returncode}')
        report['after'] = VERIFY.retired_counters(report['before'])
        sw_rows = hash_rows(args.output / 'software.framehash')
        hw_rows = hash_rows(args.output / 'hardware.framehash')
        if not matching_frames(sw_rows, hw_rows, args.frames, metadata['width'] * metadata['height'] * 3 // 2):
            raise RuntimeError('frame count, timestamps, NV12 size or pixel hashes differ')
        after, before = report['after'], report['before']
        if after['quarantined'] != 'N' or not VERIFY.resources_retired(before, after):
            raise RuntimeError('decoder quarantined or resources did not retire')
        if int(after['completed_frames']) - int(before['completed_frames']) < args.frames:
            raise RuntimeError('insufficient hardware completions')
        if args.expected_slices and int(after['completed_slices']) - int(before['completed_slices']) != args.frames * args.expected_slices:
            raise RuntimeError('wrong hardware slice completion count')
        if args.suspend_after is not None:
            for key in ['sleep_checkpoints', 'resume_restarts']:
                if int(after.get(key, 0)) <= int(before.get(key, 0)):
                    raise RuntimeError('no verified decoder '+key)
            if after.get('resume_failures') != before.get('resume_failures'):
                raise RuntimeError('decoder resume failure recorded')
        report['framehash_file_sha256'] = {n:VERIFY.sha(args.output / (n+'.framehash')) for n in ['software','hardware']}
        report['passed'] = True
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    finally:
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
        if process and process.stdin and not process.stdin.closed:
            try:
                process.stdin.close()
            except OSError:
                pass
        try:
            if armed:
                subprocess.run(['rtcwake', '-m', 'disable'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            if 'before' in report:
                report['after'] = VERIFY.retired_counters(report['before'])
                if report['after']['quarantined'] != 'N' or not VERIFY.resources_retired(report['before'], report['after']):
                    raise RuntimeError('decoder did not retire cleanly')
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            report['passed'] = False
            report['cleanup_error'] = str(error)
        (args.output / 'result.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    if not report['passed']:
        raise SystemExit('stream qualification FAILED; see '+str(args.output))


if __name__ == '__main__':
    main()
