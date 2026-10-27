#!/usr/bin/env python3
"""Small in-terminal workload; see bench.md for measurement boundaries."""
import argparse
import fcntl
import json
import os
import re
import select
import struct
import sys
import tempfile
import termios
import time
import tty
from pathlib import Path

ESC = b'\x1b['


def write_all(data):
    view = memoryview(data)
    while view:
        view = view[os.write(1, view):]


def dimensions():
    rows, cols, _, _ = struct.unpack('HHHH', fcntl.ioctl(1, termios.TIOCGWINSZ, bytes(8)))
    if not rows or not cols:
        raise RuntimeError('PTY has no usable dimensions')
    return rows, cols


def line(cols, unicode=False, phase=0):
    # Each cluster has a known terminal cell width. No slicing of ANSI or UTF-8.
    tokens = [('界', 2), ('文', 2), ('λ', 1)] if unicode else [('a', 1), ('>', 1), ('7', 1)]
    styles = ['31', '1;34', '0', '4;32']
    parts, used, i = [], 0, phase
    while used < cols:
        text, width = tokens[i % len(tokens)]
        if width > cols - used:
            text, width = ' ', 1
        parts.append('\x1b[' + styles[i % len(styles)] + 'm' + text)
        used += width
        i += 1
    return (''.join(parts) + '\x1b[0m').encode('utf-8')


def payload(scene, rows, cols, phase):
    text = line(cols, scene == 'unicode', phase)
    if scene == 'scroll':
        return (text + b'\r\n') * rows
    return b''.join(ESC + f'{r};1H'.encode() + text for r in range(1, rows + 1))


def drain(timeout):
    # tcdrain alone only drains the producer's tty queue. DSR is a parser fence.
    termios.tcdrain(1)
    write_all(ESC + b'6n')
    end, reply = time.monotonic() + timeout, b''
    while time.monotonic() < end:
        if select.select([0], [], [], max(0, end - time.monotonic()))[0]:
            data = os.read(0, 4096)
            if not data:
                break
            reply += data
            if re.search(rb'\x1b\[\d+;\d+R', reply):
                return
    raise RuntimeError('terminal DSR drain timed out; sample is invalid')


def cpu_sample(pid):
    if pid is not None:
        try:
            return int(Path(f'/proc/{pid}/schedstat').read_text().split()[0])
        except (OSError, ValueError, IndexError):
            pass
    return None


def process_sample(pid):
    result = {'rss_kib': None, 'pss_kib': None, 'private_dirty_kib': None}
    if pid is None:
        return result
    try:
        fields = dict(re.findall(r'^(Rss|Pss|Private_Dirty):\s+(\d+) kB$',
                                 Path(f'/proc/{pid}/smaps_rollup').read_text(), re.M))
        for source, target in [('Rss', 'rss_kib'), ('Pss', 'pss_kib'), ('Private_Dirty', 'private_dirty_kib')]:
            result[target] = int(fields[source]) if source in fields else None
    except (OSError, ValueError):
        pass
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames', type=int, default=int(os.environ.get('N', '300')))
    parser.add_argument('--repeat', type=int, default=3)
    parser.add_argument('--warmup', type=int, default=1, help='unmeasured full runs per scene')
    parser.add_argument('--interval', type=float, default=0, help='sleep seconds after each frame; included in wall time')
    parser.add_argument('--scene', choices=['all', 'fullscreen', 'scroll', 'unicode'], default='all')
    parser.add_argument('--drain-timeout', type=float, default=5)
    parser.add_argument('--st-pid', type=int, help='explicit terminal process PID if not direct child of st')
    parser.add_argument('--output', help='JSON path (default: unique file in temporary directory)')
    args = parser.parse_args()
    if args.frames < 1 or args.repeat < 1 or args.warmup < 0 or args.interval < 0 or args.drain_timeout <= 0:
        parser.error('frames/repeat/timeout must be positive; warmup/interval nonnegative')
    if not os.isatty(0) or not os.isatty(1):
        parser.error('run inside st: ./st -e tools/bench.sh [options]')
    pid = args.st_pid
    try:
        executable = os.readlink(f'/proc/{os.getppid()}/exe')
    except OSError:
        executable = None
    if pid is None and executable and Path(executable).name == 'st':
        pid = os.getppid()
    if args.output:
        output = Path(args.output)
    else:
        fd, name = tempfile.mkstemp(prefix='st-bench-', suffix='.json')
        os.close(fd)
        output = Path(name)
    report = {'version': 1, 'settings': vars(args), 'st_pid': pid,
              'parent_executable': executable, 'cpu_scope': 'st main thread /proc/PID/schedstat; excludes children',
              'drain': 'tcdrain + DSR parser fence; compositor presentation not guaranteed',
              'wall_includes_sleep': args.interval > 0, 'samples': []}
    saved = termios.tcgetattr(0)
    try:
        tty.setraw(0)
        write_all(ESC + b'?1049h' + ESC + b'?25l' + ESC + b'?7l')
        time.sleep(0.2)  # Initial geometry settling, outside all measurements.
        rows, cols = dimensions()
        report['rows'], report['cols'] = rows, cols
        scenes = ['fullscreen', 'scroll', 'unicode'] if args.scene == 'all' else [args.scene]
        for scene in scenes:
            frames = [payload(scene, rows, cols, phase) for phase in range(2)]
            for run in range(-args.warmup, args.repeat):
                write_all(ESC + b'0m' + ESC + b'2J' + ESC + b'H')
                drain(args.drain_timeout)
                before_cpu = cpu_sample(pid)
                start = time.perf_counter()
                count = 0
                for frame in range(args.frames):
                    data = frames[frame % 2]
                    write_all(data)
                    count += len(data)
                    if args.interval:
                        time.sleep(args.interval)
                drain(args.drain_timeout)
                wall = time.perf_counter() - start
                after_cpu = cpu_sample(pid)
                after = process_sample(pid)
                end_rows, end_cols = dimensions()
                if (rows, cols) != (end_rows, end_cols):
                    raise RuntimeError('PTY resized during benchmark; rerun at fixed geometry')
                if run >= 0:
                    cpu = None if before_cpu is None or after_cpu is None else after_cpu - before_cpu
                    report['samples'].append({'scene': scene, 'repeat': run, 'rows': rows, 'cols': cols,
                        'input_bytes': count, 'drain_query_bytes': 4, 'wall_seconds': wall,
                        'st_cpu_ns': cpu, 'memory_after': after})
    except Exception as error:
        report['error'] = str(error)
    finally:
        write_all(ESC + b'0m' + ESC + b'?7h' + ESC + b'?25h' + ESC + b'?1049l')
        termios.tcsetattr(0, termios.TCSANOW, saved)
    output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'report': str(output), 'samples': len(report['samples']), 'error': report.get('error')}))
    return int('error' in report)


if __name__ == '__main__':
    sys.exit(main())
