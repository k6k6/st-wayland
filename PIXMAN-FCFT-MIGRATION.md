# 2026-11-19 渲染后端迁移计划：cairo → pixman + fcft

接在 [MEMORY-RENDER-ANALYSIS.md](MEMORY-RENDER-ANALYSIS.md) 之后。第 1–6 节保留原计划。
**2026-11-20 最新状态：按用户选择继续 fcft + pixman，M1–M6 实现及本轮验证已完成。**
M4–M6 移除了 Cairo、旧文本路径及临时开关，当前只保留 fcft + pixman 渲染。
M0 与 M1–M3 的原始结果继续保留，原性能关口的未通过项不因后端清理自动视为通过。
本轮最终记录见第 10 节；第 9 节为先前 M1–M3 交付状态。
迁移之后参考 foot 的矢量缓存优化，另见 [FOOT-OPTIMIZATION.md](FOOT-OPTIMIZATION.md)。
fcft 的测量与停止原因见[第 7 节](#7-m0-执行记录2026-11-20)，
FreeType 备选的可应用补丁、逐像素对照和最终关口结论见[第 8 节](#8-直接-freetype-备选执行记录2026-11-20)。
第 2 节的数字是制定计划时的本机试验；标了“未验证”的推断由 M0 检查。

## 1. 目标与预期

- 去掉 cairo：文本用 fcft（FreeType + HarfBuzz + Fontconfig 回退），合成用 pixman，
  保留 wl_shm、按 buffer 累计 damage、像素滚动等现有结构。
- 保留现有功能：连字、FONT2、粗/斜体、宽字、boxdraw、powerline、undercurl、
  cursor trail、SIXEL、alpha。
- 预期收益不大，先说清楚：
  - 上一轮剖析里 `cairo_show_glyphs` 占滚动帧 25.6%，结论是自绘 blitter 最多省约 12%。
  - 内存上能去掉的是 cairo 的 glyph 缓存（heap 里约 2 MB 的一部分）和 libcairo 及它带入的
    8 个 X11/xcb 库（`ldd st` 现为 30 项）。这些库多数是共享页，PSS 收益未验证。
  - 所以本计划设两个 go/no-go 关口（M0、M3），不达标就停或改走第 6 节的备选。

## 2. 本机试验结果

环境：fcft 3.3.3、pixman 0.46.4、harfbuzz 15.1.0、freetype 2.15.0、cairo 1.18.6。
试验源码在 `~/.cache/st-analysis/fcft-spike/`（`t.c m.c e.c c.c d.c`），编译方式：
`cc x.c -I/usr/include/pixman-1 -lfcft -lpixman-1`。

| 项 | 结果 |
|---|---|
| 能力 | `fcft_capabilities()` = 0x7，text-run shaping 可用 |
| 构建 | `pkg-config --cflags fcft` 失败（`Requires.private: tllist` 未安装）；直接 `-lfcft` 加 pixman-1 的 cflags 可以编译 |
| 连字 | `a != b` 的 `!=`：前一格是空 glyph（w0），后一格 glyph w16、x=-8。与现在 hb.c 的摆放方式相同 |
| 字体特性 | 名字里写 `fontfeatures=cv01,cv02,cv61` 生效（日志 `+cv01 +cv02 +cv61`），对应 `hb.c:121` 的硬编码 |
| 度量 | Maple Mono NF CN 在 13.33 / 14.67 / 16 / 20 / 29.33 px：`'M'` 的 advance 与 cairo 的 cw 相同，ascent、descent 相同。`fcft_font.height` 在 14.67 / 16 / 20 px 比 ascent+descent 小 1 |
| text-run 不缓存 | 同一段重复调用每次都重新光栅化：16 字符 0.031 ms，29 字符 0.063 ms，约 2 µs/字符 |
| text-run 常驻成本 | 把 run 留着不销毁：446.5 B/字符（现在的 shaping cache 是 44 B/字符） |
| run 内普通 glyph | 位图和 bearing 与 `fcft_rasterize_char_utf32()` 的结果逐字节相同，只有连字部分不同；advance 不同（run 里 8，单字 9，14.67 px） |
| glyph 缓存无上限 | 8000 个 CJK 字 +4.68 MiB（613 B/glyph），第二遍不增长；源码里只有扩容，没有淘汰 |
| 回退 | U+1F600 → Segoe UI Symbol（单色）；Segoe UI Emoji 因 COLRv1 被 fcft 跳过；U+1F9D1 → Noto Color Emoji（彩色）。现有 `fallbackfont()` 的等价查询对 U+1F600 也给出 Segoe UI Symbol，只验证了这一个码点 |

由此得出的三个约束：

1. **必须自建 run 缓存**。按 2 µs/字符估算，一屏 4000 个字符全部未命中约 8 ms，
   而现在整屏刷新整帧是 1.72 ms。这是最大的风险。
2. **缓存不能直接存 `fcft_text_run`**，446.5 B/字符太贵。
3. **cell 高度用 ascent+descent，不用 `fcft_font.height`**，否则行高会变。

## 3. 现状盘点

cairo 调用数（`grep -c cairo_`）：

| 文件 | 数量 | 内容 |
|---|---:|---|
| `wl.c` | 155 | ShmBuffer 的 surface/cr、clip/source 状态缓存、`drawglyphs`、字体加载、回退、undercurl、SIXEL |
| `patch/powerline.c` | 58 | 路径填充、描边、曲线 |
| `patch/boxcircles.c` | 49 | 圆弧、描边、虚线（`:135` 是唯一真正的 dash） |
| `patch/boxblocks_extra.c` | 47 | 多边形、重复图案 mask |
| `patch/boxbranch.c` `boxlines_extra.c` | 35 + 35 | 圆弧、线性渐变、贝塞尔、描边 |
| `patch/boxmosaics.c` `boxextra.c` | 23 + 21 | 多边形、group/mask |
| `patch/cursortrail.c` | 15 | 两个三角形填充 |
| `hb.c` `hb.h` `st.h` `sixel.c` | 9 + 1 + 5 + 1 | 类型引用 |

测试里有 9 个 C 文件直接用 cairo：`box_geometry`(34)、`powerline`(42)、`hb_face_lifetime`(32)、
`sixel_cache`(27)、`cursortrail`(13)、`shape_cache`(9)、`fullscreen_alpha`(7)、`render_state`(6)、
`font2`(3)。

pixman 没有路径 API，所以矢量部分（约 285 处调用）比文本部分工作量更大。

## 4. 设计决定

**D1 编译开关。** 加 `PIXMAN_FCFT_PATCH`（`patches.h`），迁移期间新旧路径并存，
同一棵树能编出两个二进制做同条件对比。M6 再删旧路径。

**D2 字体。** `Font.match` 从 `cairo_scaled_font_t *` 换成 `struct fcft_font *`，共四个实例
（regular / bold / italic / bold-italic，用 `weight=bold`、`slant=italic` 属性）。
名字数组是 `{font, font2[0], …}`，每个名字由 st 用现有的 FcPattern 逻辑改写 pixelsize
（含 zoom 和输出缩放）再 `FcNameUnparse`。因此 `opencairofont`、`glyphindex`、`fallbackfont`、
`frc`、`FallbackResult` 和 `patch/font2.c` 都删除，回退交给 fcft。

**D3 度量。** cw 沿用现公式（ASCII 可打印字符 advance 均值取 ceil），ch = ascent + descent。

**D4 文本。** `GlyphFontSpec` 改为 `{const struct fcft_glyph *glyph; float x, y;}`，boxdraw 仍用
索引。未开连字时每格调 `fcft_rasterize_char_utf32()`，fcft 自带缓存。

**D5 连字 run 缓存。** 新文件 `patch/textrun.c` 取代 `hb.c`：

- 按空格把属性段切成词，以 (fcft_font, runes) 为键，词的重复率高。
- 未命中时调 `fcft_rasterize_text_run_utf32()`，逐个 glyph 与单字结果比较位图和 bearing
  （不比 advance）。相同的只存 fcft 缓存里的指针；不同的（连字部分）拷成自有 pixman 图，
  按内容哈希去重。随后销毁 run。
- 上限和淘汰沿用 `hb.c` 的做法（4096 项 + 字节上限 + second-chance clock）。

**D6 合成。** `pixman_image_composite32(PIXMAN_OP_OVER, 纯色, glyph->pix, dst, …)`；
彩色 glyph（`is_color_glyph`）直接当 src。纯色 image 做小缓存。裁剪用手算矩形交集。
`wlfillrect` 已经直接写像素，只换取指针的方式。

**D7 矢量。** 新文件 `patch/vec.c`，提供现用到的 cairo 子集：move/line/curve/arc/close/rect、
winding 与 even-odd 填充、描边（线宽、butt/round/square、miter/round、miter limit）、
translate/scale。实现用 FreeType 的 `FT_Outline` + `FT_Stroker` + `FT_Outline_Render`
生成 a8 蒙版，再用 pixman 合成。渐变用 `pixman_image_create_linear_gradient`，
重复图案用 pixman repeat，cursor trail 用 `pixman_composite_triangles`。
FT_Stroker 与 cairo 描边的像素差异未验证。

**D8 SIXEL。** `im->pixmap` 换成 `pixman_image_t *`，原尺寸仍是 `im->pixels` 的非拥有包装。
缩小时用 `pixman_filter_create_separable_convolution`，因为 pixman 的 `FILTER_GOOD` 只是双线性。

**D9 glyph 缓存上限。** fcft 不淘汰。st 侧统计新光栅化的码点数，超过阈值时在帧边界
销毁并重建四个 fcft_font，同时清空 run 缓存。

## 5. 阶段

开始前先提交工作树里未提交的改动（制定计划时 `git status` 有 10 个修改文件），作为基线提交。
执行时已确认此前修改在 `1c2e0affffe4953ed3fb8779cc1f4f235c8b04bb` 中，正式源码工作树干净，
只有本文尚未跟踪，因此沿用该提交，无须重复提交代码。
性能测量按现有规则在 `~/.cache/st-analysis/` 的隔离副本上做，不把插桩写入正式源码。

每个阶段的通用验收：

- `tools/` 下相关回归脚本通过，`render_state` 和 `history` 另跑 ASan + UBSan。
- 私有 headless mango、1920×1200、209×58 格，用 `~/.cache/st-analysis/` 现有驱动
  对新旧二进制抓同视角截图，逐像素比对，列出有差异和无差异的场景。
- 四个负载（整屏刷新、主屏滚动、区域滚动、逐字输入）的 draw 耗时和主线程 CPU，
  以及填满历史、空闲后的 RSS / PSS。

### M0 试验与关口（不改正式源码）

1. 在隔离副本里只替换 `drawglyphs` 和 `xmakeglyphfontspecs`，实现 D5 的最小版本。
2. 用同一份 `bat` 真彩色字节流测 run 缓存命中率、整屏刷新冷/热帧耗时、缓存字节数。
3. 用一份码点语料（ASCII、CJK、Nerd Font 私用区、emoji、组合字符）对比新旧两边
   每个码点选中的字体文件，列出不一致项。
4. 确认 `config.h` 的实际字体串（`size=11:antialias=true:autohint=false`）在缩放 1 / 1.25 / 2
   下 cw、ch 与现状相同。第 2 节的度量试验用的是 `autohint=true`。

**关口 1**：整屏刷新 draw 不高于基线 1.72 ms，run 缓存字节数不高于现在的 shaping cache，
回退不一致项逐条可接受。不满足则停，或改走第 6 节。

### M1 缓冲底座

- `ShmBuffer` 增加 `pixman_image_t *pix`，与 cairo surface 包同一块内存；`xw.buf` 相关的取址改走 pixman。
- `config.mk` 加 `-lfcft` 和 pixman-1，暂时保留 cairo。
- 验收：与基线逐像素相同（这一步不该改变任何像素）。

### M2 字体加载与非连字文本

- `xloadfont` / `xloadfonts` / `xunloadfonts` 按 D2、D3 改写，`drawglyphs` 按 D6 改写。
- `LIGATURES_PATCH=0` 的构建先跑通，包括宽字、粗/斜体、FONT2、zoom、输出缩放重载。
- 测试：`font2_test` 改成回退顺序测试；`render_state_test` 的 no-ligatures 构建通过。
- 验收：cw / ch 不变；文本像素差异逐场景列出。

### M3 连字

- 加 `patch/textrun.c`，`xmakeglyphfontspecs` 的连字分支改用它；`hb.c` 在开关下不再编译。
- 测试：`shape_cache_test` 改写为 run 缓存测试（上限、淘汰、命中、分配失败、去重）；
  `hb_face_lifetime_test` 删除（face 由 fcft 持有，原问题不存在）；
  `render_state_test`、`cursortrail_test` 的连字构建通过。
- 验收：连字截图与基线对比；D9 的重建路径用 CJK 压力输入验证内存回落。

**关口 2**：四个负载的 draw 和 CPU 不比基线差 5% 以上，空闲 RSS 不高于基线 26.36 MiB。

### M4 矢量

- 加 `patch/vec.c`，先用少量形状对比 FT_Stroker 与 cairo 的输出，再逐文件移植：
  `cursortrail.c` → undercurl（`wl.c` 的 `drawlines` 和波浪线）→ `powerline.c` → `box*.c`。
- 测试：`box_geometry_test`、`powerline_test`、`cursortrail_test` 的 cairo 断言换成 pixman 读像素。
- 验收：`tools/box_geometry_demo.py`、`powerline_demo.sh`、`cursortrail_demo.py` 的同视角前后截图；
  `cursortrail_reference.py` 的角点比对仍为 0 误差（它不依赖绘图后端）。

### M5 SIXEL

- 按 D8 改 `sixelsurface`、绘制处和 `sixel.c:delete_image`。
- 测试：`sixel_cache_test` 改用 pixman；原尺寸要求逐像素相同，缩放允许容差并报告最大差值。

### M6 清理

- 删旧路径和 `PIXMAN_FCFT_PATCH` 开关、`hb.c`、`hb.h`、cairo 的 include 与 `config.mk` 依赖。
- `ldd st` 确认不再有 libcairo 和 X11/xcb 库。
- 更新 `README.md`、`WAYLAND.md`，把最终前后数据追加到本文。

## 6. 风险与备选

| 风险 | 处理 |
|---|---|
| run 缓存冷启动慢（约 2 µs/字符） | 按词缓存；关口 1 实测 |
| 回退字体选择与现在不同 | M0 语料对比，逐条确认 |
| COLRv1 彩色字体（如 Segoe UI Emoji）fcft 不支持 | 会落到下一个回退字体；现状 cairo 是否在用它渲染需在 M0 确认 |
| fcft glyph 缓存无上限 | D9 |
| 矢量形状像素与 cairo 不完全一致 | 同视角截图逐形状确认；必要时该形状改为自写扫描线 |
| 抗锯齿文本像素差异 | hint style 映射相同（slight → `FT_LOAD_TARGET_LIGHT`），预期接近，实际差异在 M2 报告 |

备选（关口 1 不过时）：**pixman + 直接用 FreeType**。保留 `hb.c` 和现有回退逻辑，
只把 `cairo_show_glyphs` 换成按 glyph index 的自建位图缓存。连字路径不变，没有 text-run
的冷启动成本，代价是 glyph 缓存、彩色位图缩放要自己写。M1、M4、M5 对两条路线通用。

## 7. M0 执行记录（2026-11-20）

**当时的结论（已由第 9 节的路线选择更新）：fcft 方案不进入正式迁移。** run 缓存内存和热刷新有收益，但冷页与持续输入
退步；1.25 缩放的列宽不等价，字体回退也有实际外观变化。按关口 1 的停止条款结束本轮。
未添加 `PIXMAN_FCFT_PATCH`，未实施 M1–M6。随后继续完成了第 6 节的直接 FreeType
隔离试验，结果见第 8 节；两轮测量时基线耗时不同，不能直接跨轮比较绝对耗时。

### 7.1 原型与对照条件

全部源码、驱动、日志、原图和分析脚本位于
[`~/.cache/st-analysis/migration-m0/`](/home/kong/.cache/st-analysis/migration-m0/)。
`src-base/` 是上述提交的插桩副本；`src-fcft/` 只接管文本 specs 和 draw 路径，
保留 Cairo 字体度量、矢量、SIXEL 和缓冲管理。另用字体探针独立检查 D3 的新度量。
因此本轮不是完成后的无 Cairo 后端，也不能用 `ldd` 或 RSS 推算最终库依赖收益。

实际 `config.h` 已额外复制并记录 SHA-256，主字体为
`Maple Mono NF CN:size=11:antialias=true:autohint=false`，FONT2 为
`Maple Mono NF CN:size=10:autohint=true`；alpha 为 0.85，其余开关和优化参数保持一致。
原型按四种字重、FONT2 的独立字号、输出缩放及 `cv01/cv02/cv61` 创建 fcft 字体。

D5 最小实现位于隔离目录的 `textrun.c`：按空格切词，4096 项、8 MiB 上限及 second-chance
淘汰；普通 glyph 引用 fcft 单字缓存，位图不同的连字按内容去重。
`fcft_text_run_destroy()` 会释放 run 内 glyph，故自有位图必须复制；已淘汰 run 的位图
延迟至帧末回收，以免本行 BG 阶段生成的 specs 在 FG 绘制前失效。
位图回收和字节上限处理包含在本次 draw 计时中。

对照使用同一个私有 headless mango、1920×1200@60、209×58 格和同一份 `bat` 真彩色流。
输入文件 SHA-256 为 `59b8ce7f772efb3128ba3cf5ab65367ed3950bb55e1b6b7d83ee838a515c7bb2`。
一轮持续会话跑三个重复的四种负载、截图、2300 行历史写入、空闲及真实虚拟键盘 `z` 恢复；
另以 fcft / Cairo / Cairo / fcft 顺序复测冷页、热页及四种负载。
整屏和滚动为 60 Hz，逐字输出为 30 Hz；逐字负载由 PTY 驱动，恢复按键由 `wtype` 注入。
没有用加载后立即退出的循环替代持续使用。

CPU 为 st 主线程 `/proc/PID/schedstat` 增量，不含子进程。
没有修改 CPU governor（当前为 `amd-pstate-epp` / `powersave`）；下表同时保留冷页及区域滚动
的轮间波动，不能把小幅变化当成稳定收益。

### 7.2 耗时、命中率和内存

下表为四次交错复测中同版本两次的按帧加权 draw 均值；CPU 是每次场景的均值。
完整冷页样本为每次 60 个不同代码页，热页为每次 60 次刷新同一页；其余每次 180 次更新，
逐字输出每次 90 字。原始时间戳、帧数和重画行数见 `repeat-*/frames.csv`。

| 场景 | Cairo draw | fcft draw | Cairo 主线程 CPU/次 | fcft 主线程 CPU/次 |
|---|---:|---:|---:|---:|
| 冷页浏览 | 3.878 ms | 5.967 ms | 272.03 ms | 390.03 ms |
| 同一页热刷新 | 2.340 ms | 2.112 ms | 172.09 ms | 159.00 ms |
| 整屏代码刷新 | 2.845 ms | 2.687 ms | 643.04 ms | 612.32 ms |
| 主屏滚动 | 1.643 ms | 1.632 ms | 368.36 ms | 366.31 ms |
| 区域滚动 | 1.570 ms | 1.336 ms | 348.35 ms | 293.64 ms |
| 逐字输出 | 0.331 ms | 0.622 ms | 105.77 ms | 155.58 ms |

- 冷页：Cairo 两次为 3.884 / 3.871 ms，fcft 为 4.538 / 7.396 ms；两次均变慢，但幅度不稳定。
- 区域滚动：fcft 两次为 1.526 / 1.149 ms；平均改善受波动影响，不能认定稳定降低 15%。
- 逐字输出：Cairo 为 0.333 / 0.330 ms，fcft 为 0.622 / 0.621 ms；这组复测 draw 增加
  **87.7%**，主线程 CPU 增加 **47.1%**。第一轮三个重复也观察到退步（0.293 → 0.390 ms），
  但缓存预热历史不同，不能把两套结果混合成一个精确百分比。
- 现有 shaping cache 命中可直接复用 glyph index；fcft 的新词每次仍要重新 shaping 和光栅化。
  逐字追加会持续产生新词键，按词缓存没有消除这个代价。

| 缓存指标（交错复测） | Cairo shaping cache | fcft run cache |
|---|---:|---:|
| 冷页命中率 | 84.47% | 81.15% |
| 同页热刷新命中率 | 100.00% | 99.99% |
| 整屏代码刷新命中率 | 99.89% | 99.81% |
| 逐字输出命中率 | 93.90% | 89.50% |
| 整屏代码刷新时的最大字节数 | 2,656,402 B | 615,764 B |
| 全部六种负载的最大字节数 | 2,805,278 B | 686,244 B |

缓存统计沿用原 shaping cache 的请求分配字节口径；fcft 一侧另计自有连字位图及其结构。
表中不包含固定槽表、分配器开销、pixman 内部对象，以及 fcft 自身的普通 glyph 缓存。
故 **run 缓存项通过**，但不能将其解释为字体缓存的全部内存，D9 也尚未实施。

第一轮完成所有字体截图、填满历史后，同一个进程空闲 6.5 秒：

| 指标 | Cairo | fcft 原型 |
|---|---:|---:|
| 空闲 RSS | 52.59 MiB | 49.51 MiB |
| 空闲 PSS | 26.60 MiB | 22.50 MiB |
| 随后 1 秒主线程 CPU | 0 ns | 0 ns |
| 注入 `z` 后恢复绘制 | 通过 | 通过 |

这些内存值属于本次包含多类字体截图的长会话，不能与前一份报告的 26.36 MiB 直接混比；
原型仍同时持有两套字体对象。它们只记录本次同条件观察，不构成 M3 验收。

**历史 1.72 ms 在本次 Cairo 基线也未重现。** 已核对字号、源码配置、编译参数、输入文件
及 `draw()` 计时范围；本轮 Cairo 整屏刷新为约 2.84 ms。未定位剩余环境差异，因此不把
1.72 → 2.69 ms 当作迁移造成的退步。即使只采用本轮同条件对照，冷页/输入退步及下面的
显示差异也不能支持进入下一阶段；文档中的绝对阈值同样没有达到。

### 7.3 度量与字体回退

四种字重的结果相同：

| 输出缩放 | Cairo cw×ch | D3 的 fcft cw×ch | 结果 |
|---|---:|---:|---|
| 1 | 9×20 | 9×20 | 相同 |
| 1.25 | 12×25 | 11×25 | 列宽改变 |
| 2 | 18×39 | 18×39 | 相同 |

1.25 时，Cairo 的 ASCII 平均 advance 为 `11.000000000000002`，现有 `ceil()` 得到 12；
fcft 的平均值为整数 11。行高和 ascent 相同。直接照写原公式仍不能保持现有字符格宽度，
本轮没有通过改动正式度量算法来掩盖该差异。

字体探针按固定顺序遍历 897 个码点（ASCII、CJK、Nerd PUA、emoji、组合附加符号），
四种字重共 **3588 项**。在真实 `FT_New_Face` / `FT_Load_Glyph` 调用中记录文件名，
分别检查 fcft 的单字及单码点 text-run 路径；两条路径均有 **788 项字体文件不同**，
原 Cairo 查询没有缺失 glyph。主要包括 Segoe UI Emoji → Noto Color Emoji、
DejaVu Sans → Segoe UI Symbol，以及组合符号的等宽回退变化。

逐项记录见 [fallback-differences.tsv](/home/kong/.cache/st-analysis/migration-m0/fallback-differences.tsv)，
完整语料、字重、face index、色彩标记和度量见
[font-probe.tsv](/home/kong/.cache/st-analysis/migration-m0/font-probe.tsv)。
现有 `fallbackfont()` 会复用先前打开且覆盖该码点的字体，故语料顺序会影响结果；
此清单只表示该固定语料顺序，不能替代实际文本序列截图。没有把这 788 项默认标为可接受。

### 7.4 同视角图像检查

原图为 `out-base/*.png` / `out-fcft/*.png`，各 1920×1200，画面和裁剪坐标一致。
`compare-*.png` 是带前后标识的同位置裁剪图；`diff-*.png` 为逐通道差分。
下表按整张截图计数，最大差值按 0–255 通道值计算，不能把大量空白区摊薄后的比例
理解成文字误差很小。

| 场景 | 变化像素数 | 最大通道差值 | 观察 |
|---|---:|---:|---|
| 纯背景 / alpha | 0 | 0 | 逐像素相同 |
| 真彩色代码 | 21,842 | 7 | 文本抗锯齿不同 |
| ASCII（含粗斜体） | 6,824 | 27 | 位置基本一致，边缘覆盖不同 |
| 连字样例 | 2,238 | 30 | 本次 `!=`、箭头等样例未见断开，文本边缘不同 |
| CJK（含粗斜体） | 3,733 | 6 | 本次格对齐相同，文本边缘不同 |
| Nerd Font | 3,087 | 66 | 部分图形边缘差异较大 |
| emoji / 序列 | 4,937 | 206 | 字体造型、大小和部分字符的彩色呈现改变 |
| 组合附加符号 | 859 | 142 | 部分符号外观改变 |
| geometry 整图（含文字） | 1,258 | 9 | 差异落在文字；所选纯矢量区域相同 |

geometry 图中单独裁剪的 box 顶/底边、blocks、powerline 区域全部 **0 差异**；
坐标在 `acceptance.json`。这仅验证 M0 未迁移的选定矢量区域，不代表 M4 已验收。

重点对照图：

- [文字与几何样例并排总览](/home/kong/.cache/st-analysis/migration-m0/compare-overview.png)
- [连字前后](/home/kong/.cache/st-analysis/migration-m0/compare-ligatures.png)
- [emoji 前后](/home/kong/.cache/st-analysis/migration-m0/compare-emoji.png)
- [组合符号前后](/home/kong/.cache/st-analysis/migration-m0/compare-combining.png)

更正试验过程中的一次肉眼判断：**🧑 在两边均为彩色**，变化是 Segoe 与 Noto 的造型/大小。
emoji 截图中的部分后续字符确有彩色变单色现象，不能将它概括为 fcft 全部丢失彩色支持。

### 7.5 验证与复现材料

已通过：

- Cairo 基线 `tools/render_state_test.sh` 的全部原有变体、`tools/history_test.sh` 的 enabled/minimal
  raw/compact 对照，均启用 ASan + UBSan。
- fcft 原型当前配置的 `render_state`：buffer damage、轮换、复制、像素滚动、旧光标行恢复、空闲释放；
  同样启用 ASan + UBSan。
- fcft 原型当前配置的 `history`：raw/compact 解析与历史状态对照，ASan + UBSan。
- 原型专用缓存测试：命中、6001 个 run 的淘汰、同帧待绘位图的有效性、内容去重、帧末 32 KiB
  上限及清理；最终 32,752 / 32,768 B、3 个自有位图，ASan + UBSan 无报错。
- 两套真实 Wayland 会话及四次交错复测正常退出，所有会话均收到 `z` 并恢复绘制。

这些验证覆盖 M0 原型，不表示 `LIGATURES_PATCH=0` 或 M1–M6 的迁移矩阵已完成。
正式树没有引入原型或性能插桩。

隔离目录的 `README.md` 记录复现命令和数据口径；`prepare.py` 从已记录提交恢复源码并复制
实际配置，`run.py` / `repeat.py` 执行两组会话，`analyze.py` / `finalize.py` 生成汇总，
`build-probe.sh` 运行字体探针，`verify.py` 记录 sanitizer 构建与测试命令。
驱动会拒绝覆盖已有测量目录；复跑请使用新的证据目录，保留本轮原始记录。

## 8. 直接 FreeType 备选执行记录（2026-11-20）

**结论：显示一致性通过，性能大致持平；按原 1.72 ms 门槛暂不进入正式迁移。**
最终交错复测整屏 draw 为 1.766 → 1.744 ms，另一组持续会话为 1.711 → 1.777 ms；
没有稳定的整屏收益，两组原型结果都高于关口 1 的 1.72 ms。差距只有几个百分点，
不能据此认定直接 FreeType 有显著性能退步，也不能把交错复测的小幅收益当成已达标。
本轮仍停在隔离 M0，M1–M6 没有开始。

### 8.1 原型范围与可复现补丁

证据目录：[`~/.cache/st-analysis/migration-ft-m0/`](/home/kong/.cache/st-analysis/migration-ft-m0/)。
最终版本和结果固定在 [`accepted/`](/home/kong/.cache/st-analysis/migration-ft-m0/accepted/)，
该目录名只表示冻结候选，不表示迁移关口通过。顶层 `out-*`、`repeat-*`、`final/` 和
`visual-v4/` 属于先前试验，不混入本节数字或图像。

可单独应用到 `1c2e0af` 的补丁：
[`prototype.patch`](/home/kong/.cache/st-analysis/migration-ft-m0/accepted/prototype.patch)。
补丁只接入 `patch/ftraster.c`、`wl.c` 的字体打开/卸载与 `drawglyphs` 钩子，以及 pixman
链接依赖，不含性能 CSV 插桩；插桩由隔离目录的 `prepare.py` 添加。
实际 `config.h`、`patches.h`、输入流与基线相同，哈希在 `artifact-hashes.json`。

原型保留 `hb.c`、现有 Fontconfig/FONT2 回退、四种字重和全部字体度量，按 glyph index
缓存 FreeType 位图，并使用 pixman 合成。位图预算为 2 MiB，8192 个固定槽的结构体和
待绘数组另计；超预算时 second-chance 淘汰，待绘字形独立持有 image 引用，字体重载前清空。
单个超大字形仍可能超过预算，尚未覆盖任意字号的严格内存上限，不能作为完整 D9 验收。

按本机 Cairo 1.18.6 源码对齐了加载标志、四分之一像素相位、整数定位、度量框裁剪，
以及相邻字形重叠时先用 ADD 合并蒙版再着色的行为。彩色字体中的单色轮廓仍保留原 hint
target，避免组合键帽 U+20E3 等字形边缘变化。回退字体选择与 shaping 均沿用原函数。

**原型仍依赖 Cairo。** Cairo 继续持有和设置字体 face；COLRv0/COLRv1、SVG 及需要变换的
纯位图字体仍用原绘制路径。直接 `FT_Render_Glyph` 不能等价替代这些路径，本轮没有通过
换字体或丢弃彩色支持绕过差异。矢量、SIXEL、缓冲仍是原实现，`ldd` 仍有 libcairo/X11/xcb。
因此本轮结果只验证混合文本原型，不能证明完全移除 Cairo 的收益或兼容性。

### 8.2 同条件耗时和内存

同一私有 headless mango、1920×1200@60、209×58 格，同一配置、优化参数和 `code.ansi`。
冻结版本完成 sanitizer 后才开始测量，期间没有并行编译或测试，也没有修改 CPU governor。
先跑两侧持续会话：四种负载各重复三次，随后截图、写入 2300 行历史、空闲和真实
`wtype z` 恢复；另以 FreeType / Cairo / Cairo / FreeType 顺序复测冷页、热页和四种负载。
逐字场景是 PTY 定时输出，恢复输入是实际 Wayland 虚拟键盘；没有用加载后立即退出代替会话。

以下为四次交错复测、各版本两次的按帧加权 draw 均值。CPU 为每次场景主线程
`/proc/PID/schedstat` 增量的均值，不含子进程。计时覆盖 `draw()` 至 `xfinishdraw()` 及缓存
统计读取，不含 CSV 写出。原始帧数和重画行数见 `repeat-*/frames.csv` 与 `summary.json`。

| 场景 | Cairo draw | FreeType draw | draw 变化 | Cairo CPU/次 | FreeType CPU/次 |
|---|---:|---:|---:|---:|---:|
| 冷页浏览 | 2.404 ms | 2.343 ms | −2.54% | 172.86 ms | 169.00 ms |
| 同页热刷新 | 1.626 ms | 1.635 ms | +0.58% | 119.94 ms | 118.44 ms |
| 整屏代码刷新 | 1.766 ms | 1.744 ms | −1.30% | 404.15 ms | 404.54 ms |
| 主屏滚动 | 0.884 ms | 0.840 ms | −4.92% | 202.43 ms | 193.23 ms |
| 区域滚动 | 0.837 ms | 0.816 ms | −2.56% | 191.33 ms | 185.05 ms |
| 逐字输出 | 0.241 ms | 0.227 ms | −5.51% | 76.50 ms | 72.76 ms |

三轮负载连续运行的另一组会话结果如下。两组预热顺序不同，不能合并成一个平均值：

| 场景 | Cairo draw | FreeType draw | draw 变化 | 主线程 CPU 变化 |
|---|---:|---:|---:|---:|
| 整屏代码刷新 | 1.711 ms | 1.777 ms | +3.84% | +2.03% |
| 主屏滚动 | 0.860 ms | 0.872 ms | +1.46% | +1.70% |
| 区域滚动 | 0.713 ms | 0.702 ms | −1.55% | −1.24% |
| 逐字输出 | 0.220 ms | 0.218 ms | −0.76% | +0.51% |

两组四负载的 draw/CPU 均未出现超过 5% 的退步，但整屏/主屏滚动的改善方向不稳定。
区域滚动和输入在两组的 draw 均有改善，幅度不同；热页略慢，冷页没有再出现 fcft 的
大幅退步。区域滚动交错复测的 FreeType 两次为 0.785 / 0.847 ms，也能看到轮间波动。
最终会话的 Cairo 已接近历史 1.72 ms，未重现 fcft 试验时约 2.84 ms 的基线；环境差异
原因仍未定位。本节只做轮内同条件比较，不据跨轮数字声称性能提升。

交错复测的整屏 shaping cache 两边均为 **2,644,968 B**，全部负载峰值均为
**2,791,744 B**，因为仍用原 `hb.c`。FreeType 另持有最多 **9,620 B** 的字形位图；
包含各类字体截图的完整会话位图峰值为 **62,436 B**。这些字节不含固定槽表、pixman
对象和分配器开销，也不包含仍保留的 Cairo 字体缓存，不能把它们当作总字体内存。

完成所有截图和历史写入、空闲 6.5 秒后的同进程测量：

| 指标 | Cairo | FreeType 原型 |
|---|---:|---:|
| RSS | 52.805 MiB | 52.465 MiB |
| PSS | 25.007 MiB | 24.776 MiB |
| 随后 1 秒主线程 CPU | 0 ns | 0 ns |
| 注入 `z` 后恢复绘制 | 通过 | 通过 |
| 使用保留 Cairo 路径的字形绘制次数 | 原实现 | 17 |

PSS 差为 **0.230 MiB**；只是一次相同内容长会话中的观察值，没有证明稳定的内存收益。
这些会话加载了多类回退字体，不能与 M3 的旧场景 26.36 MiB RSS 直接比较，M3 未验收。

### 8.3 同视角图像与度量

最终 `out-base/` 和 `out-ft/` 各 12 张 1920×1200 原图的差异像素数及最大通道差值均为 **0**：
背景/alpha、代码、ASCII、CJK、连字、Nerd Font、emoji、组合符号、geometry、历史、空闲、
恢复输入。也保存了带标签的相同坐标裁剪图：

- [代码前后](/home/kong/.cache/st-analysis/migration-ft-m0/accepted/compare-code.png)
- [连字前后](/home/kong/.cache/st-analysis/migration-ft-m0/accepted/compare-ligatures.png)
- [emoji 前后](/home/kong/.cache/st-analysis/migration-ft-m0/accepted/compare-emoji.png)
- [组合符号前后](/home/kong/.cache/st-analysis/migration-ft-m0/accepted/compare-combining.png)

另用相同 glyph specs 对照 Cairo 与原型，覆盖四种字重、ASCII/CJK/Nerd/emoji/组合字符/连字，
以及 0.125 px 步进的小数坐标。三档缩放的 12 组整图全部逐像素一致，日志在
[`asan-ftcompare.log`](/home/kong/.cache/st-analysis/migration-ft-m0/accepted/asan-ftcompare.log)。

| 输出缩放 | 两侧 cw×ch | 像素差异 |
|---|---:|---:|
| 1 | 9×20 | 0 |
| 1.25 | 12×25 | 0 |
| 2 | 18×39 | 0 |

这是本机字体、当前 hinting/抗锯齿配置与这些样例的结果；没有把任意字体、LCD 模式或
所有字号都标成已通过。彩色 glyph 的一致也包含保留 Cairo 路径的贡献，不能推及无 Cairo 后端。

### 8.4 回归与复现

冻结版本的下列验证均通过 ASan + UBSan：

- `render_state` 全部五种构建，包括 no-ligatures 和 no-reflow；`history` 的 enabled/minimal、
  raw/compact 对照。
- `cursortrail` 五种构建；初次隔离运行因相对路径缺少 kitty 源码跳过了参考比对，随后设置
  `KITTY_SOURCE=/home/kong/git-apps/kitty` 补跑，各启用变体 2996 帧角点误差均为 **0 px**。
- `font2` 四种构建（含无连字）、`hb_face_lifetime`、`shape_cache` 两种预算。
- 新增缓存测试：64 KiB 预算、10,000 次查找，实际 8525 次预算淘汰；待绘 image 在淘汰后仍有效，
  裁剪文本像素相同，字体重载和清空不留下位图。
- 三档缩放/四字重像素探针，以及 U+20E3、肤色修饰符等回退顺序的独立像素探针。

[`README.md`](/home/kong/.cache/st-analysis/migration-ft-m0/README.md) 给出在新目录复现的完整命令。
`accepted/prototype.patch` 已在临时基线副本中独立应用、带实际配置编译通过，并核对光栅源码
与冻结候选一致；检查完成后移除了临时副本。`prepare.py` 可重建带插桩的两侧源码。
`verify-done`、`measure-done` 标记验证/测量正常结束，原始日志、图像和哈希都保存在最终目录。
补丁与正式树分开，正式源码、默认配置和构建依赖均未修改。

后续若重新推进，应先在新测量目录验证整屏 1.72 ms 门槛，并补齐独立字体 face 管理、
COLR/SVG 和彩色位图缩放，之后才有依据判断移除 Cairo 是否值得；本轮没有把这些工作
算作已经完成，也没有更改原关口来通过验收。


## 9. 选择 fcft + pixman 后的正式实施（2026-11-20）

用户明确选择 fcft + pixman 继续推进。此前 M0 关口 1 的停止决定由这次选择覆盖；
第 7、8 节的原始测量仍保留。当前落地 M1–M3 的实现，保留阶段开关，不把实现完成
等同于关口 2 验收通过。M4 矢量、M5 SIXEL、M6 移除 Cairo 尚未实施。

### 9.1 实现

- 默认 `PIXMAN_FCFT_PATCH=1`，`patches.h` 与默认配置一致；改为 0 可编译旧 Cairo 路径。
  构建仍链接 Cairo，另外增加 fcft/pixman。当前系统缺 fcft 私有依赖 tllist 的 `.pc`，
  因此用 pixman 的 cflags 和直接 `-lfcft`，不伪造 pkg-config 文件。关闭渲染开关仍需这些构建依赖。
- M1：每个 ShmBuffer 的 pixman image 与 Cairo surface 包装同一块 wl_shm 内存，
  销毁包装后才 munmap；矩形、buffer 间复制和像素滚动从 pixman 取址。
  跨两种 API 写入时保留 Cairo flush/mark-dirty，现有 damage、buffer 轮换和空闲回收保留。
- M2：`patch/fcft.c` 提供四种样式、FONT2 顺序/独立字号、zoom 和输出缩放；
  `Font.match` 在新路径中持有 fcft 对象，不再持有 Cairo 字体。
  单字使用 fcft 缓存，彩色字形作为 source、单色作为 mask 交由 pixman 合成。
  额外修复彩色字形无法通过 `fg=bg` 隐藏的问题：invisible/blink 隐藏阶段跳过前景，保留背景 alpha。
- M3：`patch/textrun.c` 按词缓存 shaping，4096 项、8 MiB 请求字节预算、second-chance 淘汰；
  普通字形复用 fcft 单字对象，连字位图按内容去重，淘汰字形延迟到帧末释放。
  超过 1024 码点或单项预算的 run 仅留到帧末，不进入缓存；分配失败回退单字绘制。
  验证 cluster 和 glyph 数量，每个源字符最多四个 specs，避免字体展开越界。
- D9：记录不同 `(font, codepoint)`，默认 8192 个后在帧末清空 run 缓存并销毁、重建四个字体。
  计数包含被连字吸收及超长临时 run 的输入码点。阈值是码点数量，不是全部字体内存的硬字节上限；
  当前帧的临时位图和 fcft 内部 Fontconfig/FreeType 分配可能超出缓存请求字节统计。
- `hb.c`/`hb.h` 和 `patch/font2.c` 的旧实现只在关闭新路径时使用，尚未删除。

### 9.2 回归与字体度量

构建通过。`render_state` 两后端各五种配置、`history` enabled/minimal × raw/compact
通过 ASan + UBSan；最终新路径额外补跑 `render_state` 五种配置，包含合成彩色字形的
invisible/blink 显隐及背景 alpha 回归。`font2` 两后端共八种普通构建通过，
最终新路径四种构建再通过 ASan + UBSan，覆盖备用字体优先级、独立字号、四样式、
zoom/scale、缺字及缩小到 512 个码点预算后的 CJK 帧末重建。

`textrun_test` 在 8 MiB/1 KiB 两档预算通过普通构建及 ASan + UBSan，覆盖真实 fi 连字、
命中、普通字形共享、位图去重、淘汰后 specs 生命周期、超长 run 和分配失败回滚。
沙箱 ptrace 环境不支持 LeakSanitizer，因此这些 sanitizer 运行设 `ASAN_OPTIONS=detect_leaks=0`；
没有将泄漏检测记作通过。

其余通过：`box_geometry`、`powerline`、`cursortrail`、`sixel_cache`、`fullscreen_alpha`、
`kittykbd`、`selection_snapshot`、`blink_bounds`、旧路径 `shape_cache` 和 `hb_face_lifetime`。
Cursor trail 每个启用变体的 Kitty 参考对照为 2996 帧、最大角点误差 0 px。

实际配置 Maple Mono 的字体度量：

| 输出缩放 | 旧 Cairo cw×ch | 当前 fcft cw×ch | 说明 |
|---|---:|---:|---|
| 1 | 9×20 | 9×20 | 相同 |
| 1.25 | 12×25 | 11×25 | 保留 fcft 整数 advance，不模拟旧浮点误差 |
| 2 | 18×39 | 18×39 | 相同 |

字体回退采用 fcft，M0 已记录的 emoji/组合符号字体选择变化仍然存在；
本轮没有重新声称 3588 项字体文件完全相同。

### 9.3 冻结版本的同条件测量

证据归档目录：[migration-fcft-m1-m3-20261120](/home/kong/.cache/st-analysis/migration-fcft-m1-m3-20261120/)。
测量先在 `/tmp` 隔离副本完成，随后归档到既有分析目录；插桩未写入正式源码。
`production-sha256.json`、`production.patch` 和 `added-files/` 记录最终代码，
`src-base/`、`src-fcft/` 是带相同 draw 计时的冻结副本。

使用私有 headless mango、1920×1200@60、209×58 格、实际 config.h 和相同 code.ansi。
沙箱内无法取得 DRM，私有合成器通过获准的沙箱外命令运行。
先完成两个长会话（各三轮四种负载、12 张截图、2300 行历史、空闲和真实虚拟键盘恢复），
再按 fcft / Cairo / Cairo / fcft 交错运行；复测时没有并发编译或回归测试。
CPU governor 保持 `amd-pstate-epp / powersave`，没有锁频；小幅差值不可视为稳定收益。

下表为四轮交错复测中同版本两次的按帧加权 draw 均值、每次场景的主线程 CPU 均值：

| 场景 | Cairo draw | fcft draw | Cairo CPU/次 | fcft CPU/次 |
|---|---:|---:|---:|---:|
| 冷页浏览 | 2.504 ms | 4.353 ms | 180.15 ms | 285.32 ms |
| 同页热刷新 | 1.714 ms | 1.930 ms | 125.98 ms | 137.85 ms |
| 整屏代码刷新 | 1.833 ms | 1.691 ms | 419.12 ms | 398.28 ms |
| 主屏滚动 | 0.938 ms | 0.944 ms | 214.26 ms | 213.87 ms |
| 区域滚动 | 0.867 ms | 0.868 ms | 197.02 ms | 195.90 ms |
| 逐字输出 | 0.216 ms | 0.519 ms | 67.05 ms | 125.58 ms |

冷页 draw 增加约 73.9%，逐字 draw 增加约 140.9%、CPU 增加约 87.3%；
逐字的 Cairo 两轮为 0.168 / 0.264 ms，fcft 为 0.507 / 0.531 ms，幅度受波动影响，
但两轮都退步。另一个长会话的逐字 draw 为 0.229 → 0.296 ms，也显示退步。
整屏交错复测达到历史 1.72 ms 目标，但不能据此声称所有负载通过。

全部交错负载的请求缓存字节峰值：Cairo shaping cache **2,780,088 B**，
fcft run cache **944,976 B**（含自有位图）。整屏命中率 99.86% → 99.80%，
逐字 93.93% → 89.61%。这些数值不含固定槽表、分配器与 fcft 的普通 glyph/字体分配；
不能当作总字体内存。D9 另用实际 8192 阈值连续三轮加载 8192 个 CJK 码点并调用生产帧末重建路径，
计数均归零，之后 ASCII 仍可光栅化。首轮 RSS 从 35.531 降至 27.289 MiB，
后两轮均从 35.223 降至 27.289 MiB；未额外调用 malloc_trim。
这是隔离字体压力探针，没有 wl_shm 窗口，不能代替整终端 RSS；源码和原始数值见
归档中的 `cjk-memory.c` / `cjk-memory.log`。

长会话填满历史、空闲 6.5 秒后：

| 指标 | Cairo | fcft |
|---|---:|---:|
| RSS | 52.441 MiB | 47.531 MiB |
| PSS | 24.699 MiB | 19.797 MiB |
| 随后 1 秒主线程 CPU | 0 ns | 0 ns |
| 虚拟键盘注入 z 后恢复绘制 | 通过 | 通过 |

这是一次同条件多字体长会话的观察，PSS 减少约 4.90 MiB；不能与原计划不同场景的
26.36 MiB RSS 直接比较，也不代表已移除 Cairo 的最终内存。

### 9.4 图像与关口状态

最终两侧各 12 张 1920×1200 原图已保存，逐像素差异与 M0 对应场景一致：
背景/alpha 0，代码 21,842，ASCII 6,824，连字 2,238，CJK 3,733，Nerd 3,087，
emoji 4,937，组合符号 859，geometry 1,258，历史/空闲各 31,590，恢复输入 31,620。
`summary.json` 另列每项最大通道差值及差异区域，`geometry-crops.json` 中四个纯几何区域
仍为 0 差异。与 M0 相同的截图不等于 Cairo 文本逐像素等价。

已目视检查代码、连字及 emoji 的同位置对照：连字连续、格对齐；抗锯齿边缘有差别。
emoji 部分字符仍从彩色变为单色，🧑 两边均为彩色但造型不同，未把这项变化隐藏成完全等价。

- [代码前后](/home/kong/.cache/st-analysis/migration-fcft-m1-m3-20261120/compare-code.png)
- [连字前后](/home/kong/.cache/st-analysis/migration-fcft-m1-m3-20261120/compare-ligatures.png)
- [emoji 前后](/home/kong/.cache/st-analysis/migration-fcft-m1-m3-20261120/compare-emoji.png)

**关口 2 尚未通过**：逐字输出超过 5% 退步限制，RSS 旧场景未重新匹配。
这轮交付的是默认可构建、经过回归的 M1–M3 迁移实现，不是 M4–M6 的完成声明。
后续优先降低新词 shaping/光栅化成本并复测，再推进矢量、SIXEL 和 Cairo 依赖清理。

## 10. M4–M6 实施与最终验证（2026-11-20）

用户要求继续实施 M4–M6，覆盖第 9 节末尾暂缓后续阶段的安排。当前已完成矢量、
SIXEL 和依赖清理，生产构建仅保留 fcft + FreeType + pixman。实现完成与原性能关口
分开记录；第 7–9 节仍是当时的结果，不代表当前源码还提供 Cairo 后端。

### 10.1 实现与依赖

- **M4**：新增 `patch/vec.c`、`patch/vec.h`，使用 FreeType Outline/Stroker 生成 A8 覆盖，
  pixman 完成合成。支持现有路径、曲线、变换、描边端点/连接、虚线、even-odd 裁剪、
  线性渐变、重复图案和 alpha group；覆盖蒙版限制在路径与裁剪的交集。
  `box*.c`、Powerline、下划线/undercurl 已移植，cursor trail 使用 pixman 两个三角形，
  共享覆盖蒙版并排除当前光标。运动学和 damage 恢复逻辑保留。
- **M5**：`ImageList.pixmap` 改为 pixman image。原尺寸直接包装已有像素，不复制数据；
  缩小使用 BOX 面积卷积，放大使用 LINEAR/IMPULSE，分别按轴选择，采样相位精度为 8 bit。
  透明图片用 OVER、不透明图片用 SRC；部分单元格擦除及缓存重建继续按原裁剪规则处理。
- **M6**：删除 `PIXMAN_FCFT_PATCH`、Cairo 类型/调用/依赖、`hb.c/hb.h`、旧 `font2.c/h`，
  删除旧 HB 专属测试。ShmBuffer 由 pixman image 与 VecContext 包装 wl_shm 内存，
  所有包装释放后才 munmap；移除跨 Cairo/pixman 的 flush/dirty 同步及旧绘图状态缓存。
  `x.c` 仍为不参与构建的历史 X11 文件，不是可切换后端。
- 构建直接依赖 fcft、pixman、FreeType、Fontconfig、Wayland client/cursor/protocols、
  xkbcommon。保留直接 `-lfcft` 以适配本机缺少私有 tllist `.pc` 的安装；HarfBuzz 仍由
  fcft 间接使用。`README`、`README.md`、`WAYLAND.md` 和测试脚本已更新。
  `ldd st` 确认没有 Cairo、X11、xcb：原 Cairo 30 项、M3 混合路径 32 项、最终 23 项
  （均含 vdso 和 loader）。实际配置构建、发行包解压后由两份默认配置构建均通过。

### 10.2 回归与像素证据

下列配置通过 ASan + UBSan，并设置 `UBSAN_OPTIONS=halt_on_error=1`：
`render_state` 五种、`history` enabled/minimal × raw/compact、`cursortrail` 五种、
`sixel_cache` 三种、`box_geometry` 四种、`powerline` 三种，以及新增 `vec_test`。
发现并修复了空 VecPath 的零长度 `memcpy(NULL, 0)`，随后相关 sanitizer 重新通过。
`ASAN_OPTIONS=detect_leaks=0` 仍用于沙箱 ptrace 限制，没有将 LeakSanitizer 标为通过。
普通构建另通过 FONT2 四种、fullscreen alpha 七种、键盘、选择快照、blink bounds 和
两档预算的 textrun 缓存测试；未降低已有几何语义断言或放宽测试容差。
真实 Wayland 选择传输集成测试也通过：控制版本因 SIGPIPE 退出，最终版本在读取端
提前关闭后保持运行，随后仍能向正常读取端提供选择文本。
三种 undercurl（CURLY/SPIKY/CAPPED）另各验证 16 个起始列/长度组合，调用真实字形
绘制路径，均产生非空波浪线且 VecContext 状态正常，ASan + UBSan 无错误。

- **矢量原语**：64×48 的独立 Cairo/FreeType 对照中，even-odd 矩形完全相同；
  三角形、曲线描边、椭圆、虚线圆、非均匀变换描边的最大通道差为 10–22/255，
  各图平均通道差不超过 0.218。
- **完整几何 atlas**：868 个 inventory + 18 个 Powerline，覆盖两个尺寸和 normal/bold，
  共 3544 个案例，2508 个逐像素一致；1,972,236 像素中 46,884 个不同。
  最大通道差 72/255，集中于 9×17 的方端斜线连接；小虚线圆最大 68，Powerline 最大 32。
  Octant 的 920 个案例全部一致。检查原图和最大差异放大图后，轮廓、连接和空洞保持，
  FT_Stroker 与 Cairo 的抗锯齿覆盖差异确实存在。修复了极小 Powerline 分离轮廓的
  相反 winding 导致覆盖抵消的问题。
- **Cursor trail**：各启用变体与 Kitty 参考比对 2996 帧，最大角点误差 0 px。
  另外固定时钟、使用真实 `trailupdate/advance/paint` 的八个前后场景角点记录完全一致；
  常规运动最大通道差 7–16/255，合成的小数折叠场景为 68/255，集中在重叠边缘。
  实心内部、当前光标排除区和外边框一致。固定帧证据承担定量比较，避免实时截图的
  采样时刻差异被误算成渲染差异。
- **SIXEL**：独立探针以实际生产缩放函数对照 Cairo GOOD，覆盖不透明/预乘 alpha、
  原尺寸、缩小、放大和非等比缩放：最大通道差分别为 **0 / 1 / 3 / 1**。
  回归额外检查窄白条缩小的面积平均，避免简单双线性导致混叠；部分擦除单元格的
  透明/不透明合成均逐像素一致。

### 10.3 最终同条件测量

证据目录：[migration-fcft-m4-m6-20261120](/home/kong/.cache/st-analysis/migration-fcft-m4-m6-20261120/)。
所有计时插桩、合成器、截图驱动均在隔离副本中运行，未加入生产源码。
`production-source/`、`production-sha256.json`、补丁及新增文件保存最终源码；
`benchmark-production-match.json` 核对测量副本除插桩外的运行代码一致。
使用私有 headless Mango、1920×1200@60、209×58 格、实际 `config.h` 和相同 `code.ansi`。
CPU governor 保持 powersave；没有锁频。性能采样期间没有并发编译或回归测试。

先分别运行原 Cairo、M3 混合路径、最终后端的长会话：各三轮四种负载、15 张截图、
2300 行历史、空闲 6.5 秒及真实虚拟键盘恢复；随后按最终 / Cairo / Cairo / 最终交错复测。
下表为交错复测的按帧加权 draw 均值、每次场景主线程 CPU 均值：

| 场景 | 原 Cairo draw | 最终 draw | 原 Cairo CPU/次 | 最终 CPU/次 |
|---|---:|---:|---:|---:|
| 冷页浏览 | 2.488 ms | 4.440 ms | 179.29 ms | 293.44 ms |
| 同页热刷新 | 1.738 ms | 1.608 ms | 128.25 ms | 121.20 ms |
| 整屏代码刷新 | 1.861 ms | 1.729 ms | 444.92 ms | 422.44 ms |
| 主屏滚动 | 0.982 ms | 0.859 ms | 219.61 ms | 201.44 ms |
| 区域滚动 | 0.915 ms | 0.848 ms | 203.11 ms | 192.40 ms |
| 逐字输出 | 0.256 ms | 0.503 ms | 79.45 ms | 123.04 ms |

整屏/滚动有所改善，但冷页 draw 增加 **78.5%**，逐字 draw 增加 **96.5%**、CPU 增加
**54.9%**。逐字两轮 Cairo 为 0.251 / 0.261 ms，最终为 0.496 / 0.511 ms，均退步。
整屏两轮最终为 1.841 / 1.618 ms，均值 1.729 ms，未稳定达到历史 1.72 ms 目标。
没有通过反复重测选择较好数字覆盖这些结果。

长会话中 M3 → 最终的 draw 为：整屏 1.704 → 1.587 ms、主屏滚动 0.942 → 0.893 ms、
区域滚动 0.730 → 0.719 ms、逐字 0.300 → 0.285 ms；这只是一次顺序运行的观察，
不足以宣称移除 Cairo 带来稳定的相同比例收益。

长会话空闲后的内存和输入恢复：

| 指标 | 原 Cairo | M3 混合路径 | 最终后端 |
|---|---:|---:|---:|
| RSS | 52.727 MiB | 47.746 MiB | 46.246 MiB |
| PSS | 24.710 MiB | 19.900 MiB | 19.826 MiB |
| 随后 1 秒主线程 CPU | 0 ns | 0 ns | 0 ns |
| 注入 z 后恢复绘制 | 通过 | 通过 | 通过 |

最终相对原 Cairo 的 PSS 减少 4.884 MiB；单独 M4–M6 相对 M3 仅减少约 0.074 MiB。
这是加载多类字体和几何/SIXEL 后的一次会话，不能替代原计划 26.36 MiB RSS 场景。
交错负载的请求缓存峰值为 Cairo 2,804,504 B、最终 949,560 B；统计范围仍不包括 fcft
普通 glyph、字体对象、固定槽表和分配器开销。

### 10.4 同视角结果与最终状态

三侧各保存 15 张 1920×1200 原图。**最终与 M3 的全部文本、背景、历史、空闲、输入恢复
以及原尺寸 SIXEL 场景逐像素一致**。本轮新增差异为：geometry 1084 像素/最大通道差 5，
完整 geometry demo 当前视口 1942/31，Powerline demo 1439/5。demo 超出窗口的内容由
10.2 的完整 atlas 补足。原 Cairo 与最终的文本差异沿用 M1–M3 的字体选择与抗锯齿变化，
包括 emoji 部分彩色变单色；本轮没有将它们标为已修复。

SIXEL 场景相对原 Cairo 的 487 个差异像素全部在标题文字，图片区域一致。
`cursortrail_demo.py` 另通过隔离 wrapper 运行原脚本，分别捕获 block/bar/underline
第二次移动后的相同视口；实时截图只用于目视检查，因合成器调度不同不做像素相等声明。
已检查 geometry/Powerline/undercurl 及 trail 的前后图，定量差异见 atlas 与固定帧探针。

- [完整几何 atlas](/home/kong/.cache/st-analysis/migration-fcft-m4-m6-20261120/geometry-comparison.png)
- [最大几何差异放大](/home/kong/.cache/st-analysis/migration-fcft-m4-m6-20261120/geometry-largest-differences.png)
- [Powerline demo 前后](/home/kong/.cache/st-analysis/migration-fcft-m4-m6-20261120/compare-m3-powerline-demo.png)
- [Cursor trail 固定帧对照](/home/kong/.cache/st-analysis/migration-fcft-m4-m6-20261120/trail-evidence/overview.png)
- [最终测量与像素数据](/home/kong/.cache/st-analysis/migration-fcft-m4-m6-20261120/summary-final.json)

**M4–M6 实施和上述功能验证完成；原性能关口 2 仍未通过。** 后续性能工作应针对冷词
shaping/光栅化与逐字输出缓存命中成本，再按相同场景复测。当前不再保留可切换的 Cairo
路径；重现前后比较使用归档中的独立源码，不需要向生产代码重新引入迁移开关。
