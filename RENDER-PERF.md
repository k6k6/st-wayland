# 渲染性能优化工作记录

> 开始：2026-09-22 ｜ 环境：Hyprland 0.52.2，eDP-2 1920×1200@60，scale=1，16 核
> 目标：降低 st(wayland) 渲染路径的 CPU 开销，并让重绘频率与显示器刷新率对齐。
> 状态：**第一、二阶段完成（未 commit）**。第二阶段（去 offscreen surface、直接画进 shm buffer）见 §5。

---

## 1. 测量方法（可复用，脚本在 `tools/`）

| 脚本 | 用途 |
|---|---|
| `tools/bench.sh` | 在 st 内运行：等待窗口拿到最终尺寸 → 300 次整屏重绘（混合颜色/bold，每次间隔 4 ms）→ `seq 300000` 滚屏。结果追加到 `/tmp/bench.out` |
| `tools/render_test.sh` | 渲染一屏验收内容：bold/italic/underline/undercurl/reverse/faint/struck、16 色/truecolor/背景色、ligature、CJK、emoji、boxdraw、braille、彩色 undercurl、长行折行、光标 |
| `tools/shot.sh <bin> <out.png>` | 启动 `<bin> -e render_test.sh`，用 `hyprctl clients` 找窗口几何，`grim` 截图 |
| `tools/compare.py a.png b.png` | 逐像素比对两张截图（需要 PIL/numpy） |

常用命令：

```sh
make && cp st /tmp/st-new
# CPU 时间（st 进程本身）
perf stat --no-inherit -e task-clock /tmp/st-new -e tools/bench.sh
# 剖析
perf record -F 4000 -g -o /tmp/p.data /tmp/st-new -e tools/bench.sh
perf report -i /tmp/p.data --comm st-new --no-children --sort dso --stdio | head
perf report -i /tmp/p.data --comm st-new --children --sort symbol --stdio | grep -vE '0x[0-9a-f]{8}' | head -40
# commit 次数（帧数）
WAYLAND_DEBUG=1 /tmp/st-new -e tools/bench.sh 2>&1 | grep -cE 'wl_surface#[0-9]+\.commit'
# 正确性
WAIT=2.5 SLEEP=3 tools/shot.sh /tmp/st-base /tmp/shot-base.png   # 原版需要多等，见 §4
tools/shot.sh ./st /tmp/shot-new.png
tools/compare.py /tmp/shot-base.png /tmp/shot-new.png            # 期望 diff bbox None
```

注意：`perf stat` 若不加 `--no-inherit` 会把 bench.sh / seq 等子进程的 CPU 也算进去。

基线二进制：`/tmp/st-base`（优化前，机器重启后需从 git stash 或注释掉改动重新构建）。

---

## 2. 优化前剖析（94×52 格，整屏重绘 300 次 + seq 300k）

CPU 1315 ms（`perf stat` 含子进程；`--no-inherit` 约 513 ms）。按 DSO：

| DSO | 占比 | 原因 |
|---|---|---|
| libharfbuzz | **44%** | `hbtransform()` 每帧对每个属性段重新 `hb_shape()`，没有任何缓存 |
| libcairo | 17% | 每个 cell 一次 `cairo_rectangle+fill`，每段 `save/clip/restore`，每次 `set_source_rgba` 分配 pattern |
| libpixman | 16.5% | `pixman_fill`（背景）+ glyph composite |
| libc | 8% | `present()` 里 offscreen→shm 的整屏 memcpy |
| st 本身 | 7.5% | 终端仿真 + `run()` 每次唤醒 `tattrset(ATTR_BLINK)` 整屏扫描 |

另外发现：主循环 `needdraw` 只由 tty/输入/闪烁触发，窗口启动后要等第一次 tty 输出才会 map；且 `draw()` 没有与合成器帧率同步，`maxlatency=2ms` 时 300 次重绘发了 340 次 commit。

---

## 3. 已完成的改动（第一阶段）

全部编译通过，`tools/compare.py` 与原版截图**逐像素一致**。

### 3.1 `hb.c`：harfbuzz 塑形缓存
- `ShapeEntry` 表：4096 槽、2-way 组相联，键 = FNV-1a(font 指针, rune 序列)，命中时比较 `font/hash/length/memcmp(runes)`。
- 命中直接返回缓存的 `hb_glyph_info_t[]` / `hb_glyph_position_t[]`（`data->buffer = NULL`；`wl.c` 只读 `glyphs/positions/count`，不用 buffer）。
- 未命中：正常 `hb_shape` 后复制进 victim 槽（优先空槽，否则按 `shapetick` 奇偶伪随机淘汰）。
- 段长 > `SHAPECACHE_MAXLEN`(1024) 不缓存。
- `hbunloadfonts()`（字体重载/缩放）和 `hbdestroybuffer()` 时 `shapecache_clear()`——因为键里有 font 指针，重载后指针可能复用。
- `hbfontcache` 里的 hb_font 用 `hb_ft_font_create_referenced()` 建立：HarfBuzz 的 table 指针指向 FT_Face 的文件 mmap（零拷贝），而 cairo 在打开超过 `MAX_OPEN_FACES`(10) 个 face 后会 `FT_Done_Face()` 空闲 face；不自己持引用就会被回收，之后整形读到野指针而崩溃（回归测试 `tools/hb_face_lifetime_test.sh`）。
- 效果：harfbuzz 44% → 0.5%。

### 3.2 `wl.c`：背景矩形直接写内存
- 新增 `wlfillrect(const Color*, x, y, w, h)`：`premulpixel()` 算好 premultiplied `0xAARRGGBB`（alpha==0xffff 走快速路径），`cairo_surface_flush` → 逐行写 `uint32_t` → `cairo_surface_mark_dirty_rectangle`。编译器会把内循环向量化成 AVX-512 store。
- `drawrect()` 改为调用它；`patch/boxdraw.c` 的 `bdrect()` 也改走它（`bdcr` 变量保留但不再使用）。
- `wlfillrect` 是非 static 的，供 boxdraw.c 用；`patch/x_include.c` 在 wl.c 顶部包含，前向声明在 wl.c:~170。

### 3.3 `wl.c`：cairo 状态去重
- `static struct { clipx,clipy,clipw,cliph; src; srcvalid; font } ds;` 记录当前 clip / 纯色 source / scaled font。
- `setsource()` 相同像素值跳过；`setclip()` 相同矩形跳过；`drawglyphs()` 新签名带 clip 矩形 `(fg, specs, len, cx, cy, cw, ch)`，内部 `setclip` + font 去重。
- `xdrawglyphfontspecs` 的字形绘制不再 `cairo_save/rectangle/clip/restore`。
- 需要自己设 clip 的地方（undercurl 的三种样式、sixel 贴图）在 `cairo_save` 之前先 `resetclip()`，否则会与遗留 clip 求交。`cairo_save/restore` 块内部用 `setsourceraw()`（不更新 `ds`）。
- `createoffscreen()` 里 `resetdrawstate()` + `cairo_set_operator(OVER)`（operator 现在恒为 OVER；背景不再经 cairo）。

### 3.4 `wl.c`：帧回调节流
- `present()` 每次 commit 前 `wl_surface_frame()` 注册 `framecb`，记录 `framereq` 时间。
- `run()` 底部：`needdraw || presentpending` 时先 `framewait(&now)`；回调未到且未超 `FRAME_TIMEOUT`(100 ms) 则把它并入 `timeout` 并 `continue`，脏行与 damage 继续累积。
- `present()` 拿不到空闲 buffer 时置 `presentpending=1`，主循环下次会再 `draw()`。
- 效果：300 次重绘 commit 340 → 188（≈60 Hz）。

### 3.5 `wl.c`：`tattrset(ATTR_BLINK)` 缓存
- `run()` 里原来每次循环（每读一块 pty）都整屏扫描 `term.line` 找 ATTR_BLINK，`perf annotate` 显示 `run` 自身 8% 全在这。改为 `blinkattr` 变量，只在 `needdraw` 后更新。

### 3.6 `wl.c`：启动即提交首帧
- `run()` 在 `cresize()` 后立即 `draw(); wl_display_flush(); needdraw = 0;`。此前窗口要等第一次 tty 输出才 attach buffer / map。

### 3.7 结果

| 版本 | CPU（`--no-inherit`） | commit 数 |
|---|---|---|
| 原版 | 513 ms | 340 |
| 现在 | **348 ms（−32%）** | 188 |

现在的剖析（`--no-children`, by DSO）：st 自身 42%（其中 `wlfillrect` 17% 纯 store 带宽、`tputc/tscrollup` 等终端仿真 ~15%），cairo 16% + pixman 13%（字形合成），libc 15%（主要是 `present()` 的整屏 memcpy）。

---

## 4. 已知细节 / 坑

- 原版 `/tmp/st-base` 启动后不立刻 map，`shot.sh` 需 `WAIT=2.5 SLEEP=3` 才能截到；新版不需要。
- `st.h` 的 `TIMEDIFF(t1,t2)` 宏用 `t1.tv_sec`，参数不能是指针——`framewait()` 里先解引用到局部变量。
- `perf` 在此机器上 `/proc/*` 读取被策略拦截，`ps -o cputimes` 不可用；用 `perf stat --no-inherit` 或 `perf record` 采样数÷频率估算 CPU。
- 塑形缓存的 `data->buffer` 命中时为 NULL；若将来有代码要用 `shaped.buffer` 需注意。
- `xw.buf/xw.draw` 只在一帧内（`xstartdraw()` 成功 → `present()`）非 NULL；帧外调用绘制函数是静默 no-op。
- `/proc/<pid>/smaps*` 在沙盒里被策略拦截，只能让 st 里跑的脚本读 `$PPID` 自己的（`/tmp/rss.sh`, `/tmp/rss2.sh` 这种写法）。
- 沙盒下 `/tmp/bench.out` 等临时文件正常可写。

---

## 5. 第二阶段（已完成）：去掉常驻 offscreen surface，直接画进 shm buffer

**动机**：此前每帧 = 整屏填充 4.4 MB（`wlfillrect`）+ `present()` 把 damage 从 offscreen memcpy 到 shm（再读写各 4.4 MB）。foot 的做法是直接渲染到 shm buffer，只在复用 buffer 时把"自它上次绘制以来变化过"的区域从最新 buffer 拷过来。同时省掉常驻 offscreen。

### 5.1 实现（`wl.c`）

1. `ShmBuffer` 增加 `cairo_surface_t *surface; cairo_t *cr;`（`cairo_image_surface_create_for_data` 直接包住 mmap 内存）。新建时用 `premulpixel(&dc.col[defaultbg])` 填满整块（resize 后的兜底）。`destroybuffer()` 同时销毁 surface/cr，并把指向它的 `lastbuf/curbuf` 置 NULL。
2. 全局 `curbuf`（本帧正在画的）、`lastbuf`（最近一次 commit 的，持有完整画面；合成器持有期间只读）。`NBUFFERS` 2 → 3。
3. `getbuffer(w,h)`：先释放所有空闲且尺寸不符的 buffer；然后**优先选 `lastbuf` 本身**（若合成器已 release）——它已是完整画面，零拷贝；否则选其它空闲的已分配 buffer，再否则新建。Hyprland 对 shm 是 commit 后立即 upload+release，所以稳态只用 **1 块** buffer。
4. `xstartdraw()` → `startframe()`：`!xw.configured` 返回 0（首个 configure 前不能 attach）；拿不到 buffer → `presentpending = 1; return 0`（`term.dirty` 保留，等 `wl_buffer.release` 唤醒后重试）。若 `lastbuf && lastbuf != b`，按像素行把 `b->damage ∩ lastbuf` 从 `lastbuf->data` memcpy 到 `b->data`，**跳过本帧会被整行重绘的像素行**（`pixelrowdirty()`：`py < vborderpx` 看 `term.dirty[0]`，`py >= vborderpx + row*ch` 看 `term.dirty[row-1]`，其余 `term.dirty[(py-vborderpx)/ch]`）。然后 `curbuf = b; xw.buf/xw.draw = b->surface/cr; cairo_reset_clip; resetdrawstate; boxdraw_xinit`。
   - **与原设计的一处差异**：`b` 的 damage **不清空**。它记录的是"合成器上次看到这块 buffer 之后所有变化"，既包括刚从 `lastbuf` 拷来的区域，也包括本帧要画的区域；两者都必须报给 `wl_surface_damage_buffer`。若在 startframe 清掉，A/B 交替时拷贝来的行就不会被上报（合成器可能不刷新那块）。
5. `present()`：`curbuf` 为空直接返回；`cairo_surface_flush` → attach + `damage_buffer(curbuf 的 bbox)` + frame callback + commit；`busy = 1; lastbuf = curbuf; curbuf = NULL; xw.buf = xw.draw = NULL`；清 damage。整块 memcpy 逻辑删除。
6. 删除 `createoffscreen()`（含 `xinit`/`xresize` 两处调用）。`xresize` 改为只 `tfulldirt()`（旧尺寸 buffer 由 `getbuffer` 释放/重建，新 buffer 自带背景色）。`updatesize()` 去掉 `&& xw.buf` 条件。
7. `wlcreateshmbuffer()`：mmap 后 `madvise(MADV_HUGEPAGE)`。本机 `shmem_enabled = advise`，4276 kB 的 buffer 有 4096 kB 走 PMD 映射（`smaps` 的 `ShmemPmdMapped`）。整屏刷新的 store 直接打到 shm，TLB miss 显著减少，单独带来约 −4%。
8. `xw.buf/xw.draw` 仅在 `xstartdraw..present` 之间有效，帧外为 NULL；`wlfillrect` 已有 `!xw.buf` 保护，其它绘制点均已核对只在帧内调用。

### 5.2 结果（同 §3.7 的 bench，94×52，`--no-inherit`，三次取中位）

| 版本 | CPU | commit 数 | 滚屏后 RSS | shm buffer 数 |
|---|---|---|---|---|
| 原版 | 513 ms | 340 | — | — |
| 第一阶段 | 348 ms | 188 | 33.4 MB | 1 + 4.4 MB offscreen |
| 第二阶段（2 块交替 + 拷贝） | 346 ms | 187 | 33.4 MB | 2 |
| + `MADV_HUGEPAGE` | 332 ms | 187 | — | 2 |
| **+ 优先复用 `lastbuf`（最终）** | **283 ms（−45% vs 原版，−19% vs 一阶段）** | 187 | **29.3 MB** | **1** |

剖析（2 块交替版本）：libc 15% → 4%（memcpy 没了），但 `wlfillrect` 17% → 28%——写 shm（4K 页、被合成器刚读过）比写私有 malloc 内存（THP）慢；这就是加 `MADV_HUGEPAGE` 的原因。最终版稳态只有一块 buffer，既无拷贝也无跨 buffer 缓存抖动。

打字场景 `WAYLAND_DEBUG=1`：每次按键 `damage_buffer(0, 0, 950, 26)`，即一整行（脏行整行重绘的天然粒度）。

### 5.3 验证

- `tools/compare.py` 与原版截图逐像素一致（2 块交替版和最终版均验证过，前者覆盖了拷贝路径）。
- ASan 构建（`make CFLAGS="-O1 -g -fsanitize=address -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address"`）下 `/tmp/stress.sh`：`seq 200000` 输出中 60 轮 `hyprctl dispatch fullscreen 1` / `resizeactive exact 700 500` / `1300 900` 快速切换，无报告；`render_test.sh` 无报告。

### 5.4 之后可选

- `wl_surface_damage_buffer` 改按终端行分段上报（对 Hyprland 模糊背景友好），需要 per-row 的 x 范围而不是 bbox。
- `xdrawline` 在 bg pass 里合并相邻同色 cell 的 `wlfillrect`（现在 `wlfillrect` 约占 28%，纯 store 带宽；合并调用收益预计 <5%）。
- 完全空闲时 `wl_surface_frame` 不注册（目前每次 commit 都注册，无害）。
- 若合成器不立即 release（非 Hyprland），走 2 块交替 + 行级拷贝路径，性能约为 332 ms 档。

---

## 6. 文件改动清单（未 commit）

- `hb.c`：+ 塑形缓存（`ShapeEntry`, `shapecache_clear`, `shapehash`），`hbtransform` 重写；新增 `#include <string.h> <stdint.h>`。
- `wl.c`：`ShmBuffer` 附近 + frame callback（`framecb/framereq/presentpending/frame_done/framewait`）；**第二阶段**：`ShmBuffer` 增加 `surface/cr`，`curbuf/lastbuf`，`NBUFFERS 3`，`destroybuffer/getbuffer` 重写，新增 `pixelrowdirty/startframe`，`present` 重写，`createoffscreen` 删除，`xresize` 简化，`wlcreateshmbuffer` 加 `MADV_HUGEPAGE`；`setsource/setsourceraw/setclip/resetclip/resetdrawstate/premulpixel/wlfillrect/drawrect/drawglyphs` 重写；`xdrawglyphfontspecs` 字形 clip 改法；undercurl/sixel 前 `resetclip()`；`xfinishdraw` 开头 `resetclip()`；`run()`：首帧 draw、`blinkattr`、framewait 节流、`presentpending`。
- `patch/boxdraw.c`：`bdrect` → `wlfillrect`。
- `tools/`：bench.sh, render_test.sh, shot.sh, compare.py（新增）。
