# 2026-11-18 内存占用与渲染性能分析

接在 [RENDER-PERF.md](RENDER-PERF.md)（塑形缓存、直接写 shm、帧回调节流）和
[OPTIMIZATIONS.md](OPTIMIZATIONS.md)（damage 区域、SIXEL、字体回退）之后。本文先给出当前
代码的实测分布，再给出按优先级排序的优化项；实施结果见文末。

## 1. 测量条件

- 构建：与 `config.mk` 相同的 `-O2 -march=native -flto`（剖析时另加
  `-g -fno-omit-frame-pointer`），使用工作树里的 `config.h` / `patches.h`。
- 环境：私有 headless mango（`WLR_BACKENDS=headless WLR_RENDERER=gles2`），输出
  1920×1200@60，st 窗口 1892×1172 px、209×58 格。8 核 AVX-512，L2 1 MiB/核，L3 16 MiB。
  实机面板为 2560×1600@120，窗口缓冲约 8.4 MB。
- 负载按显示节拍逐帧写入（每 16.7 ms 一次，结束用 DSR 排空），不是 load-and-close：

  | 场景 | 内容 |
  |---|---|
  | page | 每帧整屏全新内容（`bat` 真彩色高亮的 `wl.c`/`st.c`；另有 CJK、纯文本） |
  | scroll1 | 主屏底部每帧新增一行（shell 输出、日志滚动） |
  | region1 | 备用屏 + DECSTBM，每帧滚一行（nvim / less 的滚动方式） |
  | type | 每 33 ms 输入一个字符 |
  | trail | 每 0.5 s 一次跨屏光标跳转 |
  | flood | 不限速输出 30–44 MB |

- 工具：每帧各阶段耗时用临时插桩（`draw()` 内 `clock_gettime` 累计，不入库）；
  CPU 用 `perf stat --no-inherit -e task-clock`；剖析 `perf record -e cycles:u`；
  堆用 heaptrack；映射用 `pmap -XX`。

## 2. 内存

### 2.1 实机实例（kB）

| 映射 | 运行 2 h 的实例 RSS | PSS | 运行 12 min 的实例 RSS | PSS |
|---|---:|---:|---:|---:|
| 字体文件 mmap（Maple Mono NF CN ×4） | 21060 | 15827 | 4884 | 1657 |
| shm buffer（memfd `st-shm`） | 16776（2 块） | 8388 | 8388（1 块） | 4194 |
| `[heap]` | 11324 | 11324 | 3516 | 3516 |
| 共享库 | 9824 | 1186 | 9676 | 1237 |
| fontconfig cache mmap | 6924 | 786 | 6920 | 785 |
| 其它匿名映射 / 栈 / 二进制 | 1488 | 944 | 1228 | 712 |
| **合计** | **67396** | **38455** | **34612** | **12101** |

### 2.2 heap 构成（heaptrack，209×58，cat 代码/CJK、nvim 翻页、git log、ls，峰值 15.62 MB）

| 项 | 大小 | 说明 |
|---|---:|---|
| scrollback 行 | 8.36 MB | 2000 行 × 209 列 × 20 B，整行宽度分配 |
| glyph 光栅 / cairo、pixman、FreeType 缓存 | 2.08 MB | 随用过的不同字形增长，由 cairo 限界 |
| HarfBuzz 塑形缓存 | 2.03 MB | 上限 4096 项 / 8 MiB，每字符 44 B + 3 次 malloc |
| 屏幕行、字体加载、fontconfig 等 | 余下 | |

105 列时 scrollback 为 4.20 MB（峰值 8.75 MB 的 48%）。

### 2.3 结论

1. **shm buffer 空闲时也常驻**。每块 = 宽 × 高 × 4（8.4–8.7 MB）。稳态只需要 1 块
   （合成器 commit 后约 1 ms 即 release，下一帧复用同一块），但第二块一旦因 release 迟到
   被分配就永不释放——运行 2 h 的实例正是 2 块。终端空闲时这 8.4–16.8 MB 完全没有用途。
2. **scrollback 是最大的私有内存**。历史行与屏幕行交换指针，所以按整行宽度保存；代码类
   输出平均行长约 25 字符，209 列下约 88% 是行尾空白。
3. **字体文件 mmap 是 RSS 最大的一项，但不是可优化项**。全部是 clean 的文件页：
   内核 fault-around 每次缺页映射 16 页（实测每次访问 +55 kB，`MADV_RANDOM` 无效），
   页本来就在 page cache 里，多实例共享，内存紧张时直接回收。空闲时 `MADV_DONTNEED`
   只能让 RSS 数字变小，系统可用内存不变，还要伸手进 FreeType 的 stream。
4. fontconfig cache 与共享库是共享映射，PSS 很小；glyph 缓存与塑形缓存都有上界。

## 3. 渲染

### 3.1 每帧耗时（300 帧，60 Hz 节拍）

| 场景 | 重绘行数/帧 | draw 耗时/帧 | 进程 CPU |
|---|---:|---:|---:|
| page（代码） | 57.7 | 2.35 ms | 889 ms |
| page（CJK） | 57.7 | 2.91 ms | 1083 ms |
| scroll1 | **57.7** | **1.93–2.03 ms** | 647–682 ms |
| region1 | **56.7** | **1.85 ms** | 620 ms |
| type | 1.4 | 0.26 ms | 145 ms |
| trail（每次跳转约 22 帧） | 5.0 | 0.33 ms | — |
| 空闲 10 s | 共 3 帧 | — | 52 ms（基本是启动） |

flood：44.3 MB 代码输出解析 406 ms（约 109 MB/s），期间只画 27 帧（帧回调节流生效）。

### 3.2 剖析

scroll1（`cycles:u`，inclusive）：`draw` 86.3% → `xdrawglyphfontspecs` 68.6%
（`wlfillrect` 35.1%、`cairo_show_glyphs` 25.6%，其中 pixman 合成 12.9%）、
`xmakeglyphfontspecs` 13.0%、`adddamage` 5.2%。

page（每帧全新文本，塑形缓存必然不命中）：st 46.8%、harfbuzz 16.1%、cairo 11.2%、
pixman 11.0%。

flood：`tputc` 26–29%、`twrite` 7–13%、`tsetchar` 7–17%；纯文本时 `tscrollup` 25%
——每换一行都要把一条冷的 4.18 kB 历史行整行清空。

### 3.3 结论

1. **滚动一行会重绘整屏**。`tscrollup` / `tscrolldown` / `kscrollup` 把整个区域标脏，
   于是每帧 57 行、约 1.9 ms，是打字帧的 7 倍。120 Hz 下持续滚动约占单核 23%。
   这是当前最大的结构性浪费。
2. **`wlfillrect` 的 27–35% 是 cache miss，不是填充算法**。循环已是 AVX-512，81% 的
   采样落在两条 64 字节 store 上。实验：帧首多做一次整块 `memmove`（然后照常全屏重绘），
   `memmove` 耗时 0.63 ms，随后的行绘制从 1.95 降到 1.31 ms，总量不变——缓冲刚被合成器
   在另一个核读过，谁先碰这 8.87 MB 谁付 0.63 ms。同核热缓存的微基准里整屏填充只要
   0.07 ms。结论：整屏帧的下限是 0.63 ms + 行绘制 1.3 ms，只能靠少画行来降。
3. 行绘制本身接近 cairo 路径的下限：`cairo_show_glyphs` 每个属性段一次调用，
   合并 `adddamage` 约可省 4%，自绘 glyph blitter 最多省约 12%，都不值得其风险。
4. 打字每键 1 次 commit、1.4 行、damage 一行；空闲无唤醒；拖尾每帧 0.33 ms。
   跳转后连续打字期间拖尾动画会持续（与 kitty 相同的门控语义），不是多余帧。

## 4. 优化项（按优先级）

| # | 项 | 目标 | 预期 | 风险 |
|---|---|---|---|---|
| P1 | 空闲释放 shm buffer：备用块不用即还，最后一块在停止绘制数秒后归还，下一帧重建并全屏重绘 | 内存 | 每个空闲终端 −8.4～−16.8 MB | 低：只动 `wl.c` 的缓冲管理 |
| P2 | 像素滚动：滚动时脏标记随行一起平移，帧首在 shm buffer 内 `memmove`，只画新行和被覆盖层污染的行 | 渲染 | 滚动帧 1.9 → 约 0.7 ms | 中：光标、选区、拖尾、预编辑、SIXEL 需逐一处理，用逐帧像素对比验证 |
| P3 | scrollback 行尾压缩：历史行只存到最后一个非重复单元，访问时原地展开 | 内存 | scrollback −60%～−88% | 中高：改动 `reflow.c` 的历史表示 |

不做：

- 字体 mmap 的 RSS（见 2.3-3）。
- 塑形缓存改紧凑布局：最多省约 0.8 MB，需同时改 `wl.c` / `x.c` 的消费方。
- foot 式 offset 滚动：免去 `memmove`，但池要 2 倍大小，与内存目标相反。
- 自绘 glyph blitter、`adddamage` 合并：收益见 3.3-3。


## 5. 实施与复测（2026-11-19）

P1、P2、P3 均已写入代码。以下前后数据来自同一台机器、同一个私有 mango
会话；保留工作树 `config.h`（`cursortrail=1`、`decay={100,300}`、threshold=3、alpha=0.85），
1920×1200@60、209×58 格。基线为 `195d835` 加同一份用户配置。
本节是重建基准后的独立对照，不把第 3 节的旧绝对耗时直接当成新的 before。

### 5.1 P1：释放空闲缓冲、跳过画面不变的帧

`wl.c` 的 `trimbuffers()` 只释放已被合成器 release、且不在绘制中的缓冲。
最后一块在停止绘制 5 秒后也释放；没有上一帧缓冲时，`startframe()` 让下一帧完整重建。
`framechanged()` 对照脏行、光标、模式、选区和覆盖层，略过无像素变化的绘制请求。

持续会话结束后继续保持窗口、等待 6.5 秒，再用虚拟键盘输入 `z`：

| 指标 | 基线 | P1+P2 |
|---|---:|---:|
| 活跃时 shm RSS | 8664 KiB | 8664 KiB |
| 空闲后 shm RSS | 8664 KiB | **0** |
| 空闲后进程 RSS | 36744 KiB | **28012 KiB** |
| 接着 2 秒空闲的主线程 CPU | 0 ns | 0 ns |
| 再输入后 shm RSS | 8664 KiB | 8664 KiB |

基线/优化版的活跃截图相同；各自的活跃/释放后截图也逐像素相同。
输入恢复后能继续渲染。先前的真实协议对照也验证了仅移动指针/释放按键不再产生空 commit。
释放缓冲由合成器的 release 时机约束，合成器仍持有的块会保留。

### 5.2 P2：滚动像素，只重画新行和覆盖层

`patch/reflow.c:tscrolldirt()` 随行平移脏标记，并记录区域滚动。
`wl.c:scrollpixels()` 在帧首移动 shm 像素，对新行、旧光标、选区、URL 下划线、
IME、拖尾和 SIXEL 覆盖区补画。多个互相覆盖的滚动按原顺序执行；过多记录、
没有可靠旧缓冲或 keyboard select 模式时保守重画。
每个 ShmBuffer 仍独立累计 damage，复制或像素滚动都不会提前清掉待上报区域。

同一份 `bat` 真彩色 `wl.c` 字节流，三个重复：前三场每次 180 个 60 Hz 更新，
打字场每次 90 个 30 Hz 更新。表中 draw 是各重复全部已画帧的加权平均，
CPU 是每个重复的 st 主线程 `/proc/PID/schedstat` 增量，不含子进程。

| 场景 | 基线 draw | P1+P2 draw | 基线重画行/帧 | P1+P2 行/帧 | 基线 CPU/次 | P1+P2 CPU/次 |
|---|---:|---:|---:|---:|---:|---:|
| 整屏代码刷新 | 1.710 ms | 1.721 ms | 55.82 | 56.09 | 400.70 ms | 398.17 ms |
| 主屏逐行滚动 | 1.760 ms | **0.868 ms** | 56.00 | **2.96** | 355.72 ms | **194.33 ms** |
| 备用屏区域滚动 | 1.673 ms | **0.707 ms** | 55.70 | **2.00** | 325.31 ms | **154.99 ms** |
| 逐字输入 | 0.216 ms | 0.216 ms | 2.00 | 2.00 | 66.66 ms | 67.52 ms |

滚动 draw 分别减少 **50.7% / 57.8%**，CPU 减少 **45.4% / 52.4%**。
整屏刷新与逐字输入在这次对照中基本持平，未观察到收益。

`tools/render_state_test.sh` 的随机滚动每种构建执行 6000 步，每帧与强制整屏重画
逐像素比较，并检查所有改变的像素均在上报 damage 内。五种构建包括无连字、
无宽字优化、最小功能和 REFLOW 关闭；无连字构建现在也输入宽字符，并修正旧光标
覆盖在宽字尾部时的恢复范围。真实 SIXEL 解析生成的透明/不透明图片覆盖主/备用屏、
历史浏览、滚动区域和文字覆盖；cursor trail 测试同时检查尾迹恢复。

### 5.3 P3：无损历史行尾压缩

`thiststore()` 保存整行前缀以及重复尾部 cell 的一份副本，按完整 `Glyph` 比较，
保留字符、宽字/换行标记、前景/背景、下划线颜色和 SIXEL 等属性。
`TLINE` / `TLINEABS` 经 `thistline()` 展开为可读写的完整行；屏幕行一直保持完整宽度。
独立记录历史行逻辑列数，避免 reflow 和备用屏尺寸变化时按错误的宽度展开。

事件循环结束后，`thistcompact()` 重新收紧离开视口的已展开行；正在显示的历史行和
keyboard select 两个持久光标引用的行保持地址稳定。历史元数据增加约 16 KiB。
满行内容没有重复尾部时不节省该行；不能把短日志的比例套到满宽文本上。

压缩时重新分配紧凑副本再释放原大行。实测发现原地 `realloc` 缩小虽然减少 requested
bytes，但会在大量原始页面各留下小前缀，两次 resize 后 RSS 反而超过未压缩版本；
改为单独分配副本后，resize 的收益恢复。真实搜索又验证了另一种情况：
可见行/键盘光标把 heap 末端留住，内部已释放的页面还会常驻 RSS。因此一次压缩
归还至少 1 MiB cell 时再调用 `malloc_trim(0)`，普通滚动不触发它。
修正后的搜索、持续交互与十轮 resize 对照都能保持内存收益。

`tools/history_test.sh` 对比完整历史和压缩历史：3500 步解析/滚动/选区/resize/
换屏操作，以及独立的 SIXEL reflow、键盘搜索、持久光标指针和整段历史读取测试。
文字、属性、选区文本和图片状态摘要一致；同一负载也与 P3 之前的代码独立对照一致。
2000 行短日志的历史 requested allocation 从 **8.36 MB 降至约 0.9 MB**（约 −89%）。
读取全部历史后再压缩，检查分配量回到读取前，防止浏览/搜索永久展开历史。

真实交互用 P1+P2 与 P1+P2+P3 对照：以 333 行/秒输出 2300 行短日志，
虚拟键盘连续 `Ctrl+b` 翻历史、`Ctrl+Shift+b` 搜索、输入关键词、`n`、Escape，
把输出从 1920×1200 改为 1600×1000 再改回，用键盘输入 `xyz`，再以 60 Hz 输出
100 行，最后保持窗口空闲 6 秒。每个状态都截图并采集 `/proc/PID/smaps_rollup`。

| 同一会话阶段 | P1+P2 RSS | 加入 P3 后 RSS |
|---|---:|---:|
| 历史已填满 | 40.12 MiB | **32.97 MiB** |
| 搜索结果 | 40.68 MiB | **33.75 MiB** |
| 缩小再恢复 | 40.46 MiB | **34.63 MiB** |
| 再输入/输出后 | 40.79 MiB | **34.82 MiB** |
| 最后空闲 | 32.33 MiB | **26.36 MiB** |

每次操作后等待 300 ms，尺寸切换等待 700 ms，再抓取同视角画面；
九个对应阶段的前后截图逐像素相同。输入回执严格为 `xyz`，搜索词/Return/Escape
均由 st 消费，未泄漏给子进程，并检查了搜索高亮和退出后的画面。
搜索操作的主线程 CPU 这轮为 10.79 → 20.38 ms；历史展开、重压缩与页面归还有代价。
另在未搜索的会话做十轮缩小/恢复，恢复到大窗口后的 RSS 为 34.84～35.48 MiB，后八轮波动约
0.21 MiB，没有随每轮持续增长。保留少量 allocator/cache 内存，因此 RSS 节省小于
requested allocation 的下降，未用后者代替实际进程内存。


P3 的最后一轮持续负载单独重复两次，P1+P2 与加入 P3 的同条件对照如下：

| 场景 | P1+P2 draw | 加入 P3 draw | P1+P2 CPU/次 | 加入 P3 CPU/次 |
|---|---:|---:|---:|---:|
| 主屏逐行滚动（180 次/3 秒） | 0.867 ms | 0.904 ms | 194.44 ms | 203.34 ms |
| 逐字输入（90 次/3 秒） | 0.216 ms | 0.218 ms | 66.69 ms | 69.98 ms |

新增历史复制/分配有代价：这轮滚动主线程 CPU 增加 4.6%，输入增加 4.9%，绝对量分别
约 3.0 / 1.1 ms CPU 每秒。输入的绘制时间基本持平。P3 主要换取内存，不能宣称它让
所有渲染场景都更快；P2 的滚动收益仍保留。没有读过历史时，压缩收尾直接返回，
普通输入不再无条件扫描 2000 个历史槽。

### 5.4 验证方法与产物

代码回归可直接运行：

```sh
sh tools/render_state_test.sh
sh tools/history_test.sh
sh tools/cursortrail_test.sh
sh tools/sixel_cache_test.sh
CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
LDFLAGS='-fsanitize=address,undefined' sh tools/render_state_test.sh
CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
LDFLAGS='-fsanitize=address,undefined' sh tools/history_test.sh
```

本机这次测量的原始 CSV、JSON、输入字节流、驱动脚本与同视角 PNG 保留在
`~/.cache/st-analysis/`，未把插桩加入正式源码。主要产物：

- `out-base/` / `out-p2/`：P1+P2 前后每帧耗时、进程 CPU、空闲内存与截图；
- `interactive-accepted-p2/` / `interactive-accepted-p3/`：P3 修正后九个交互状态的截图、RSS/PSS、输入回执；
- `interactive-resize-p3/resize-samples.json`：十轮尺寸切换后的内存；
- `quick-p2/` / `quick-p3/`：最终持续滚动与输入的额外两次对照；
- `workload.py`、`interactive.py`、`interactive-accepted-run.py`、
  `interactive-resize-run.py`、`quick-workload.py`、`quick-run.py`：对应负载与键盘/合成器驱动。

这些结果覆盖私有 headless mango 的真实 Wayland 缓冲、输入、滚动和 resize；
实际桌面高刷新率、多窗口遮挡及不同合成器的收益仍需在对应环境测量。
构建、14 个回归脚本、render-state/history 的 ASan+UBSan 和 `git diff --check` 均通过。
