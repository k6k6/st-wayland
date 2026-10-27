Run from the repository root, inside a dedicated terminal:

```sh
./st -e tools/bench.sh --frames 300 --repeat 3 --warmup 1 --output /path/to/result.json
# Controlled 4 ms pacing, explicitly included in wall time:
./st -e tools/bench.sh --scene fullscreen --interval 0.004 --output /path/to/paced.json
# Headless verification:
tools/blink_bounds_test.sh
python3 tools/bench_test.py
```

Only Python 3's standard library is needed by the benchmark. `N=300` remains
supported as the default frame count. Without `--output`, each invocation creates
a unique `st-bench-*.json` in the system temporary directory and prints its path.
An explicit output path is overwritten. Use different paths for comparisons.

The runner reads actual rows/columns using ioctl after a short settling delay.
Each frame contains exactly that many full-width rows: mixed SGR attributes,
explicit CR/LF scrolling, or Unicode (CJK wide characters and Greek). Odd widths use a space for the final cell when necessary. Every frame
alternates content/styles. Autowrap is disabled during the run, so writing the
bottom-right cell does not accidentally scroll fullscreen scenes. Resizing at a
sample boundary invalidates the run. Keep geometry fixed throughout.

Payloads are generated before timing. Warmup means complete unmeasured runs per
scene. Each measured run starts with a cleared screen and an input drain. The
end drain uses tcdrain plus a cursor-position query (DSR), with a bounded timeout;
it confirms st has parsed preceding bytes, **not** that the compositor has
presented the final frame. Wall seconds cover writes, PTY backpressure, optional
per-frame sleep and the final drain. The default interval is zero. Use paced runs
to study periodic refresh, but do not call their wall time st CPU time. Unpaced
runs may coalesce many updates into a single display frame; these are throughput
workloads rather than FPS measurements. CPU results can include rendering that
happens before the fence; rendering after it is outside the sample.

JSON records every repeat separately, including actual geometry, exact workload
input bytes (excluding setup and the separately recorded four-byte final query),
wall seconds and st main-thread CPU nanoseconds from `/proc/PID/schedstat`.
The shell wrapper uses exec so a direct `./st -e tools/bench.sh` launch preserves
st as the parent. Automatic CPU detection requires the parent's executable name
to be `st`; use `--st-pid PID` for another launch arrangement or renamed binary.
The CPU counter excludes Python/shell and child processes. It measures the st
main thread, not possible library worker threads. Missing procfs information is
JSON null. Memory snapshots use smaps_rollup RSS/PSS/private-dirty KiB when
available; they are snapshots, not peak usage. Report metadata identifies the
parent executable and selected PID. Errors produce an `error` field and exit 1.

Compare identical geometry, fonts, patches, compositor settings, build flags and
workload arguments on an otherwise quiet machine. Keep all repeats and compare
their distribution, not a single best value. These tools do not launch or alter
the desktop compositor. The runner uses an alternate screen, hides the cursor,
and restores termios on normal completion/error; use a dedicated terminal since
it restores autowrap and cursor visibility to their normal enabled state.

The boundary regression links the actual st.o, discarding unrelated sections,
and exercises both REFLOW configurations without Wayland. It checks every cell
of 1x1 and 2x2 grids, matching/unrelated attributes, attribute removal and exact
dirty rows. Build products live in a disposable directory. Normal st build
headers, pkg-config and a C compiler are required. CFLAGS/LDFLAGS are accepted.
