#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded real decode test: no-fallback VA output must match software pixels.

Run as the broker's permitted uid, after installation. Does not install,
unload modules, restart the desktop, or reboot. Uses the selected VA backend
and installed broker. Output uses about 180 MB with the default 64 frames.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import time


def sha(p):
    h = hashlib.sha256()
    with p.open('rb') as f:
        for b in iter(lambda: f.read(1024 * 1024), b''):
            h.update(b)
    return h.hexdigest()


def counters():
    base = Path('/sys/module/gma500_gfx/parameters')
    names = ['completed_frames', 'completed_slices', 'firmware_boots', 'quarantined', 'session_active']
    names += [n for n in ['large_slices', 'max_slice_bytes_seen', 'bitstream_pages',
                          'engine_contexts', 'surface_objects', 'surface_pages', 'forced_resets',
                          'forced_reset_failures', 'firmware_ready_waits', 'firmware_ready_timeouts',
                                'startup_retries', 'startup_retry_failures',
                                'sleep_checkpoints', 'resume_restarts', 'resume_failures']
              if (base / ('msvdx_' + n)).is_file()]
    return {n: (base / ('msvdx_' + n)).read_text().strip() for n in names}


def resources_retired(before, after):
    return after['session_active'] == '0' and all(
        after.get(n) == before.get(n) for n in
        ['bitstream_pages', 'engine_contexts', 'surface_objects', 'surface_pages'])


def retired_counters(before):
    deadline = time.monotonic() + 2
    while True:
        after = counters()
        if resources_retired(before, after) or after['quarantined'] != 'N' or time.monotonic() >= deadline:
            return after
        time.sleep(.01)


def validate_outputs(sw, hw, frames, before, after, width=1280, height=720):
    expected = frames * width * height * 3 // 2
    delta = int(after['completed_frames']) - int(before['completed_frames'])
    return (sw.stat().st_size == hw.stat().st_size == expected
            and sha(sw) == sha(hw) and delta >= frames
            and after['quarantined'] == 'N'
            and resources_retired(before, after))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('input', type=Path)
    ap.add_argument('--frames', type=int, default=64)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--driver-dir', type=Path, default=Path('/opt/cedar-msvdx'))
    ap.add_argument('--socket', default='/run/cedar-decoder/decoder.sock')
    ap.add_argument('--seek', type=float, default=0,
                    help='seek to this many seconds before decoding (tests random access)')
    ap.add_argument('--expect-rejection', action='store_true',
                    help='negative control: require hardware failure before any frame completes')
    args = ap.parse_args()
    if not 1 <= args.frames <= 128:
        ap.error('use 1..128 frames')
    if not 0 <= args.seek < float('inf'):
        ap.error('--seek must be finite and nonnegative')
    args.output = args.output or Path('build/validation-' + time.strftime('%Y%m%d-%H%M%S'))
    args.output.mkdir(parents=True, exist_ok=False)
    metadata = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-select_streams',
        'v:0', '-show_entries', 'stream=codec_name,width,height,profile', '-of', 'json',
        str(args.input)]))['streams'][0]
    width, height = metadata['width'], metadata['height']
    if metadata['codec_name'] != 'h264' or width < 2 or height < 2 or (width | height) & 1:
        raise SystemExit('use H.264 with even NV12 dimensions')
    required_bytes = 2 * args.frames * width * height * 3 // 2
    if shutil.disk_usage(args.output).free < required_bytes + 16 * 1024 * 1024:
        raise SystemExit(f'insufficient output space: need {required_bytes} bytes plus 16 MiB headroom')
    before = counters()
    if before['quarantined'] != 'N':
        raise SystemExit('decoder quarantined; collect logs and recover before testing')
    if before['session_active'] != '0':
        raise SystemExit('decoder session already active; stop competing clients before testing')
    for hardware in [False, True]:
        name = 'hardware' if hardware else 'software'
        cmd = ['ffmpeg', '-nostdin', '-v', 'warning', '-xerror']
        if hardware:
            cmd = ['env', 'LIBVA_DRIVER_NAME=cedar',
                   'LIBVA_DRIVERS_PATH=' + str(args.driver_dir.resolve()),
                   'CEDAR_DECODER_SOCKET=' + args.socket, 'CEDAR_VA_DMABUF=1',
                   *cmd, '-hwaccel', 'vaapi', '-vaapi_device', '/dev/dri/card0',
                   '-hwaccel_output_format', 'vaapi']
        else:
            cmd += ['-hwaccel', 'none']
        cmd += ['-ss', str(args.seek), '-i', str(args.input), '-frames:v', str(args.frames), '-an', '-vf',
                'hwdownload,format=nv12' if hardware else 'format=nv12',
                '-f', 'rawvideo', str(args.output / (name + '.nv12'))]
        with (args.output / (name + '.log')).open('wb') as log:
            try:
                subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                after = retired_counters(before)
                rejected = (args.expect_rejection and hardware and isinstance(error, subprocess.CalledProcessError)
                            and after['completed_frames'] == before['completed_frames']
                            and after['quarantined'] == 'N' and resources_retired(before, after))
                report = dict(hardware_validated=False, failed_stage=name,
                              rejection_validated=rejected, metadata=metadata, input_sha256=sha(args.input),
                              error=str(error), command=cmd, before=before, after=after,
                              requested_frames=args.frames, seek_seconds=args.seek)
                (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
                if rejected:
                    print(json.dumps(report, indent=2))
                    return
                raise SystemExit(f'{name} decode failed; see {args.output}') from error
    sw, hw = args.output / 'software.nv12', args.output / 'hardware.nv12'
    after = retired_counters(before)
    delta = int(after['completed_frames']) - int(before['completed_frames'])
    valid = validate_outputs(sw, hw, args.frames, before, after, width, height)
    report = dict(hardware_validated=valid, metadata=metadata, input_sha256=sha(args.input),
                  software_sha256=sha(sw), hardware_sha256=sha(hw),
                  requested_frames=args.frames, seek_seconds=args.seek,
                  output_bytes=hw.stat().st_size, pixel_frames=hw.stat().st_size // (width * height * 3 // 2),
                  hardware_frame_delta=delta, before=before, after=after)
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not valid or args.expect_rejection:
        raise SystemExit('hardware qualification FAILED')


if __name__ == '__main__':
    main()
