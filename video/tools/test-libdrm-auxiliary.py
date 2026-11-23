#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""On-target discovery/identity regression for the private libdrm build.

Run with LD_LIBRARY_PATH=/opt/cedar-libdrm/lib. No rendering or modesetting.
Tests real nodes through independent fd/dev_t/list APIs, including legacy APIs.
"""
import ctypes as c
import json
import os
from pathlib import Path


class Device(c.Structure):
    _fields_ = [('nodes', c.POINTER(c.c_char_p)), ('available', c.c_int),
                ('bustype', c.c_int), ('businfo', c.c_void_p), ('deviceinfo', c.c_void_p)]


P = c.POINTER(Device)


def main():
    d = c.CDLL('libdrm.so.2')
    d.drmGetDevice2.argtypes = [c.c_int, c.c_uint32, c.POINTER(P)]
    d.drmGetDevice.argtypes = [c.c_int, c.POINTER(P)]
    d.drmGetDeviceFromDevId.argtypes = [c.c_ulong, c.c_uint32, c.POINTER(P)]
    d.drmDevicesEqual.argtypes = [P, P]
    d.drmFreeDevice.argtypes = [c.POINTER(P)]
    d.drmFreeDevices.argtypes = [c.POINTER(P), c.c_int]
    d.drmGetDevices2.argtypes = [c.c_uint32, c.POINTER(P), c.c_int]
    d.drmGetDevices.argtypes = [c.POINTER(P), c.c_int]
    loaded = sorted({line.split()[-1] for line in Path('/proc/self/maps').read_text().splitlines()
                     if '/libdrm.so.' in line})
    prefix = os.environ.get('CEDAR_LIBDRM_PREFIX', '/opt/cedar-libdrm').rstrip('/') + '/'
    assert loaded and all(p.startswith(prefix) for p in loaded), loaded
    expected = [{'/dev/dri/card0'}, {'/dev/dri/card1', '/dev/dri/renderD128'}]
    nodes = ['/dev/dri/card0', '/dev/dri/card1', '/dev/dri/renderD128']

    def describe(p):
        v = p.contents
        names = [v.nodes[i].decode() for i in range(3) if v.available & (1 << i)]
        result = dict(bustype=v.bustype, nodes=names)
        result['identity'] = str((Path('/sys/class/drm') / Path(names[0]).name / 'device').resolve())
        if v.bustype in (0, 5):
            ids = c.cast(v.deviceinfo, c.POINTER(c.c_uint16))
            result['pci_ids'] = [ids[0], ids[1]]
        return result

    rows = []
    open_fds_before = len(list(Path('/proc/self/fd').iterdir()))
    for _ in range(10):
        refs = []
        try:
            for i, node in enumerate(nodes):
                fd = os.open(node, os.O_RDWR | os.O_CLOEXEC)
                p = q = legacy = None
                p, q, legacy = P(), P(), P()
                try:
                    assert d.drmGetDevice2(fd, 0, c.byref(p)) == 0, node
                    assert d.drmGetDeviceFromDevId(os.fstat(fd).st_rdev, 0, c.byref(q)) == 0
                    assert d.drmGetDevice(fd, c.byref(legacy)) == 0
                    assert d.drmDevicesEqual(p, q) == 1
                    assert d.drmDevicesEqual(p, legacy) == 1
                    desc = describe(p)
                    assert set(desc['nodes']) == expected[0 if i == 0 else 1], desc
                    assert desc['bustype'] == (0 if i == 0 else 5), desc
                    assert desc['pci_ids'] == [0x8086, 0x0be2], desc
                    refs.append(p)
                    p = None
                    if len(rows) < 3:
                        rows.append(dict(requested=node, **desc))
                finally:
                    os.close(fd)
                    for owned in [p, q, legacy]:
                        if owned:
                            d.drmFreeDevice(c.byref(owned))
            assert d.drmDevicesEqual(refs[0], refs[1]) == 0
            assert d.drmDevicesEqual(refs[1], refs[2]) == 1
            assert describe(refs[1])['identity'] == str(Path('/sys/class/drm/renderD128/device').resolve())
            for legacy in [False, True]:
                arr = (P * 16)()
                count = d.drmGetDevices(arr, 16) if legacy else d.drmGetDevices2(0, arr, 16)
                try:
                    assert count == 2, count
                    groups = {frozenset(describe(arr[i])['nodes']) for i in range(count)}
                    assert groups == {frozenset(x) for x in expected}, groups
                finally:
                    if count > 0:
                        d.drmFreeDevices(arr, count)
        finally:
            for p in refs:
                d.drmFreeDevice(c.byref(p))
    assert len(list(Path('/proc/self/fd').iterdir())) == open_fds_before
    p = P()
    assert d.drmGetDevice2(-1, 0, c.byref(p)) < 0 and not p
    assert d.drmGetDevices2(0x80000000, None, 0) == -22
    assert d.drmGetDevices2(0, None, 0) == 2
    print(json.dumps(dict(status='passed', iterations=10, loaded=loaded, nodes=rows,
                         same_auxiliary_identity=True, display_kept_separate=True,
                         legacy_apis=True, no_fd_leaks=True), indent=2))


if __name__ == '__main__':
    main()
