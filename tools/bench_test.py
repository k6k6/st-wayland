#!/usr/bin/env python3
"""Headless semantic and PTY runner checks; no compositor needed."""
import errno
import fcntl
import json
import os
import pty
import re
import select
import struct
import subprocess
import tempfile
import termios
import time
import unicodedata
from pathlib import Path
import sys
sys.dont_write_bytecode = True
import bench


def width(text):
    return sum(0 if unicodedata.combining(c) else
               2 if unicodedata.east_asian_width(c) in ('W', 'F') else 1 for c in text)


assert bench.cpu_sample(2**30) is None
assert all(value is None for value in bench.process_sample(2**30).values())

for rows in (1, 2, 7):
    for cols in (1, 2, 3, 17, 80):
        for scene in ('fullscreen', 'scroll', 'unicode'):
            for phase in (0, 1):
                data = bench.payload(scene, rows, cols, phase).decode()
                text = re.sub(r'\x1b\[[0-9;]*m', '', data)
                if scene == 'scroll':
                    lines = text.split('\r\n')[:-1]
                else:
                    lines = re.split(r'\x1b\[\d+;1H', text)[1:]
                assert len(lines) == rows
                assert all(width(line) == cols for line in lines), (rows, cols, scene)
                assert '\x1b' not in ''.join(lines)

with tempfile.TemporaryDirectory(prefix='st-bench-test-') as work:
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 2, 3, 0, 0))
    original = termios.tcgetattr(slave)
    output = Path(work) / 'report.json'
    proc = subprocess.Popen([str(Path(__file__).with_name('bench.sh')), '--frames', '2',
                             '--repeat', '2', '--warmup', '1', '--output', str(output)],
                            stdin=slave, stdout=slave, stderr=slave)
    pending, captured, queries = b'', b'', 0
    deadline = time.monotonic() + 15
    while proc.poll() is None and time.monotonic() < deadline:
        if select.select([master], [], [], 0.1)[0]:
            try:
                chunk = os.read(master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    break
                raise
            captured += chunk
            pending += chunk
            while b'\x1b[6n' in pending:
                _, pending = pending.split(b'\x1b[6n', 1)
                os.write(master, b'\x1b[1;1R')
                queries += 1
    if proc.poll() is None:
        proc.kill()
        raise AssertionError('PTY runner timed out')
    assert proc.wait() == 0, captured
    assert termios.tcgetattr(slave) == original
    report = json.loads(output.read_text())
    assert 'error' not in report, report
    assert len(report['samples']) == 6
    assert queries == 18, queries  # 3 scenes * (warmup + 2 repeats) * 2 fences
    for sample in report['samples']:
        expected = sum(len(bench.payload(sample['scene'], 2, 3, phase)) for phase in (0, 1))
        assert sample['input_bytes'] == expected
        assert sample['st_cpu_ns'] is None  # Test parent is Python, not st.
        assert (sample['rows'], sample['cols']) == (2, 3)
        assert sample['wall_seconds'] > 0
    os.close(master)
    os.close(slave)
print('bench: exact visible widths, UTF-8/ANSI integrity, PTY drain, repeats, bytes, restoration PASS')
