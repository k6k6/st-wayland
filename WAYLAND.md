# st X11 → Wayland 迁移总结

> 日期：2026-09-21 ｜ 环境：Hyprland 0.52.2（`XDG_SESSION_TYPE=wayland`，monitor scale=1）
> wayland 1.26.0 / wayland-protocols 1.49 / xkbcommon 1.13.2 / cairo 1.18.4（含 cairo-ft）/
> freetype 26.6.20 / fontconfig 2.18.3 / harfbuzz 14.4.0 / imlib2 1.12.7 / gdlib 2.3.3

## 1. 方案决策（已与用户确认）

| 决策点 | 选择 |
|---|---|
| 软件渲染 | **cairo + cairo-ft**（字形/矩形/波浪线/sixel 贴图全由 cairo 完成，写入 `wl_shm` ARGB8888 buffer） |
| X11 后端 | **完全替换为 `wl.c`**；`x.c` 保留在仓库中但不再参与构建 |
| 窗口图标（NETWMICON） | **xdg-toplevel-icon-v1** 协议提交 `st.png`（仍用 gdlib 解码），不支持的合成器自动回退到 desktop entry |
| HiDPI | **fractional-scale-v1 + viewporter**，字体按 scale 重载，buffer 按逻辑尺寸 × scale 渲染 |

## 2. 新增文件

- **`wl.c`**（约 5000 行）：Wayland 后端，替代 `x.c`。包含：
  - registry / xdg-shell / xdg-decoration（server-side）/ viewporter / fractional-scale / cursor-shape / primary-selection / xdg-toplevel-icon / xdg-system-bell；
  - `wl_shm` 双缓冲（NBUFFERS=4）+ 常驻 cairo offscreen surface + 脏矩形追踪（per-buffer 累积 damage）；
  - 字体：fontconfig 匹配（含 fallback ring cache）→ cairo-ft scaled font，像素尺寸按 `usedfontsize × scale`；
  - 输入：xkbcommon（keysym/UTF-8/modifiers），自研 key repeat（rate/delay 来自 `repeat_info`）；
  - 指针：button/axis（滚轮支持 `axis_value120` / discrete / 连续累积三档）→ X11 风格 `MouseEvent`，复用原有 `mousereport/mousesel/mouseaction` 逻辑；
  - 剪贴板：`wl_data_device`（clipboard）+ primary-selection 协议；粘贴走 pipe 异步读入主循环，bracketed-paste 包裹；
  - OSC 52 / selection 写入走 primary + clipboard（CLIPBOARD_PATCH 语义保留）；
  - 输入法：`zwp_text_input_v3`（fcitx5/ibus）。`xximspot()` 把光标格子（逻辑像素）通过 `set_cursor_rectangle` 报给 IM，候选窗口贴在光标下方；`commit_string` 直接 `ttywrite`（KEYBOARDSELECT 搜索模式下走 `kbds_pasteintosearch`），`preedit_string` 以下划线覆盖绘制在光标行（`drawpreedit()`，在 `xdrawcursor` 之后叠加）。
- **`protocols/`**（构建产物，git-ignored 候选）：wayland-scanner 生成的 9 个协议 client header + private code。注意：`sudo make install` 会以 root 重新生成该目录，之后普通用户 `make` 会 Permission denied，需 `sudo chown -R $USER protocols`（或先 `make` 再 `sudo make install`）。

## 3. 既有文件改动

| 文件 | 改动 |
|---|---|
| `config.mk` | 去掉 X11/Xft/Xrender/Xcursor 依赖，换成 `wayland-client/cursor`、`xkbcommon`、`cairo/cairo-ft`、`freetype/fontconfig`；`NETWMICON_LIBS` 置空（不再需要 gdlib）；`STCPPFLAGS` 改 `_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE`（`memfd_create` 需要） |
| `Makefile` | `SRC = st.c wl.c`；新增 9 个协议的 scanner 生成规则；`st.o/wl.o` 依赖更新；`clean` 删除 `protocols/` |
| `st.h` | 删除全部 X11 头文件；新增 `KeySym = xkb_keysym_t`、X11 兼容的 modifier/button mask 枚举（值与 X11 一致，`config.h` 无需改键位语义）；`Color` 改为 16-bit RGBA + `0xAARRGGBB pixel`；`Font.match` 改为 `cairo_scaled_font_t*`；`XWindow` 重写为 Wayland 对象集合（display/seat/keyboard/pointer/xdg 对象/viewport/scale/offscreen cairo surface/specbuf…）；`XSelection` 改为 Wayland offer/source 句柄；boxdraw API 改为 cairo 签名 |
| `win.h` | 仅新增 `wlsetpointer(int)` 声明（供 openurlonclick patch 用） |
| `st.c` | 删除 X11 include；`dc.col[defaultbg].pixel` 的 alpha 提取保留（`Color.pixel` 仍含 alpha 字节，供 sixel 背景色用） |
| `hb.h` / `hb.c` | harfbuzz 适配 cairo：`hbtransform(..., cairo_scaled_font_t*, ...)`，每次 shape 前 `cairo_ft_scaled_font_lock_face`、shape 后 unlock |
| `patch/boxdraw.c` | 全部 `XftDrawRect` 改为 cairo `bdrect()`（SOURCE 算子，等价语义），shade 混合改为纯内存计算 |
| `patch/netwmicon.c` | 改为空实现：图标由合成器/任务栏按 `app_id` 从 `st.desktop`（`Icon=st` → `pixmaps/st.png`）解析，**不再链接 libgd**（去掉了它拖进来的 26 个编解码/X11 库）；`xdg-toplevel-icon` 协议同时移除 |
| `patch/openurlonclick.h` | `restoremousecursor()` 改调 `wlsetpointer(POINTER_TEXT)` |
| `sixel.c` / `sixel.h` | 删除 X11 pixmap/clipmask 逻辑（`XFreePixmap`→`cairo_surface_destroy`，`sixel_create_clipmask` 删除）；缩放改用 cairo（`CAIRO_FILTER_GOOD`/`NEAREST`），透明图用 `OVER`、不透明用 `SOURCE` 算子直接画到 offscreen |
| `config.def.h` / `config.h` | `XK_*` → `XKB_KEY_*`（sed 批量替换，`XK_ANY_MOD/NO_MOD/SWITCH_MOD` 保留为本地宏）；`mouseshape` 由 `XC_xterm` 常量改为字符串 `"text"`（cursor-shape 协议/theme 名）；删除 `sixelbyteorder`（cairo 固定 ARGB，无字节序问题）；用户本地修改（字体 Maple Mono、配色、快捷键等）原样保留 |
| `patch/fixkeyboardinput.c`、`keyboardselect_*.c`、`reflow.c` | 仅 `XK_*`→`XKB_KEY_*` 更名，无逻辑改动 |

说明：`x.c` 未删除，仅不再被 Makefile 引用；`patches.def.h` 的 40 行 diff 是用户此前的本地启用项（本次未动）。

## 4. 启用的 19 个 patch 移植状态

| Patch | 状态 |
|---|---|
| ALPHA（含 focus-highlight） | ✅ 背景 alpha 经 cairo premultiply 正确合成；失焦切换 `xloadcols`+`tfulldirt` 保留 |
| ANYSIZE | ✅ `hborder/vborderpx` 居中逻辑保留，按 buffer 像素计算 |
| BOXDRAW / braille / bold | ✅ cairo 重绘，首屏渲染正确 |
| CLIPBOARD | ✅ 主选区同步写 clipboard（wl_data_device selection） |
| EXTERNALPIPE / EXTERNALPIPEIN | ✅ 纯 st.c 逻辑，未动；`st-paste-gateway.sh` 流程不受影响 |
| FIXKEYBOARDINPUT | ✅ key 表整体更名，Ctrl/Alt/CSI-u 映射逻辑不变 |
| KEYBOARDSELECT（reflow 版） | ✅ 代码路径保留，随 xdrawline/status-bar 联动；交互待手动验证 |
| LIGATURES | ✅ `fi fl => ->` 首屏 ligature 正常 |
| NETWMICON | ✅ 改为 desktop-entry 方式（`Icon=st`，`make install` 会装 `st.png` 到 `pixmaps/`），不再依赖 gd |
| OPENURLONCLICK | ✅ hover 检测 + 手形光标（cursor-shape/text 回退）+ 点击打开保留；`$WINDOWID` 在 Wayland 下无意义，已 `unsetenv`（`openurlcmd` 的 `dmenu -w $WINDOWID` 在 Wayland 下需另行处理） |
| REFLOW / SCROLLBACK / SCROLLBACK_MOUSE / SCROLLBACK_MOUSE_ALTSCREEN | ✅ st.c 侧逻辑未动；滚轮 Button4/5 上报已验证接线（语义与 X 一致） |
| SIXEL | ✅ 解析/缩放/擦除-删除逻辑已移植；**真实 sixel 图片显示待测** |
| SYNC（BSU/ESU） | ✅ `ttyread_pending`/`tinsync` 节流逻辑原样保留 |
| UNDERCURL（CAPPED） | ✅ 首屏波浪线渲染正常 |
| WIDE_GLYPHS | ✅ BG/FG 双 pass + clip 逻辑保留，CJK 双宽首屏正常 |

## 5. 构建与验证（均已通过）

- `make` 一次通过（仅剩无害 warning：`Mouse/Shortcut.screen` 缺省初始化、`st.h:79` 类型比较等 X11 时代遗留项）。
- Hyprland 实测：
  - ✅ 启动无崩溃，窗口正常 map（`hyprctl clients` 可见 `class=st`，Wayland 原生非 XWayland）；
  - ✅ 首屏渲染：bold / italic / underline / undercurl / CJK / boxdraw / braille / ligature / 彩色 URL；
  - ✅ 背景透明 + 壁纸模糊（alpha 0.8 正确合成）；
  - ✅ 键盘输入（含中文/特殊字符经 xkb UTF-8 路径）、key repeat（1.5 s ≈ 24 字符，≈25/s 吻合）、Ctrl+A 控制字符；
  - ✅ 双击选词、拖拽选区（primary + clipboard 均可被 `wl-paste -p` / `wl-paste` 读到）；
  - ✅ 中键粘贴 primary、Shift+Insert 粘贴 clipboard（含 bracketed-paste 包裹）；
  - ✅ I-beam 光标（cursor-shape-v1，theme 回退链完整）。
- **SIXEL 
- **KEYBOARDSELECT
- **HiDPI 
- **`openurlcmd` 
- **boxdraw
- 测试手段：`wtype`（键盘）、`ydotool`（鼠标，需配合 `hl.dsp.cursor.move({x=..,y=..})`  warp 光标）、`wl-copy/wl-paste`、`grim` 截图。

### 全屏时自动不透明

启用 `ALPHA_PATCH` 时，Wayland 合成器通知窗口进入全屏后，背景不透明度自动设为
`1.0`；退出全屏后恢复配置中的 `alpha`（启用焦点透明度时按当前焦点使用
`alpha` / `alphaUnfocused`）。最大化不视为全屏，不需要启用 `FULLSCREEN_PATCH`。

全屏只覆盖实际绘制的透明度，不改写原有设置；全屏中调整透明度会在退出后生效。
切换不重载配色，保留应用通过 OSC 设置的颜色；选区透明度和渐变也遵循全屏覆盖。
即使窗口尺寸未改变，进入／退出全屏也会重绘背景和边框。

无显示服务器回归测试：`tools/fullscreen_alpha_test.sh`。运行 `make` 后用 `./st`
试用；已运行的终端需重新启动，构建不会自动安装。

### FONT2_PATCH：备用字体

Wayland 后端已支持 `FONT2_PATCH`，使用 Fontconfig + Cairo，不再依赖 Xft。
启用时将 `patches.h` 中的 `FONT2_PATCH` 设为 `1`（如需新配置也默认启用，同步修改
`patches.def.h`），然后在 `config.h` 的 `font2[]` 中按优先级配置字体，例如：

```c
static char *font2[] = {
    "Noto Sans CJK SC:size=12",
    "Symbols Nerd Font Mono:pixelsize=16",
};
```

- 主字体已有的字形不受影响；缺字时按 `font2[]` 顺序查找，均缺字才进入自动字体回退。
- 每个备用字体预加载 normal / italic / bold-italic / bold 四种样式；空列表也可正常使用。
- `size` 为点数（默认 DPI 为 96，也可指定 `dpi`），`pixelsize` 为逻辑像素；未指定大小时为 12 点。
  字体随终端 zoom 和 Wayland 整数/分数缩放重载，保留备用字体与主字体的大小比例。
- 字体名使用 Fontconfig 格式，不支持 X11 XLFD 格式；请用 `fc-match '字体名'` 检查实际匹配结果。
  未安装的字体可能由 Fontconfig 替换为其他字体。
- 修改配置后运行 `make`。开关默认保持关闭，不改动现有用户字体配置。
- 无显示服务器回归测试：`tools/font2_test.sh`（需构建依赖、Python 3 和 DejaVu Sans/Serif/Mono 字体）。
  在临时副本中验证补丁关闭、空列表、多字体及关闭 ligatures 的构建与字体选择、缩放和缓存清理。

### 内建 Powerline 几何符号

启用 `BOXDRAW_PATCH` 且 `config.h` 中 `boxdraw = 1` 时，以下字符与 `█` 一样按
**实际单元格的像素宽高**绘制，不再依赖字体轮廓、字体回退或 HarfBuzz shaping：

- `U+E0B0–U+E0BF`：实心／细线箭头、圆角端帽、四个方向的三角形及斜线。
- `U+E0D2`、`U+E0D4`：上下梯形端帽。

这覆盖 Neovim 字符画中的 `   ` 和状态栏中的 `  `，避免它们与
终端生成的方块、背景色块采用不同尺寸而产生接缝。曲线和斜线保留灰度抗锯齿；
绘制限制在当前格子内，保留已有背景、透明度及正常反显／选区处理。字号、行高与
Wayland 缩放改变后直接使用新的 cell 尺寸；实心图形不随 bold/italic 变形。

普通 Nerd 图标（例如 ``、文件夹）、装饰性 Powerline 字符仍走原字体路径。
关闭 `boxdraw` 可恢复这些几何符号的字体绘制；不需要修改字体文件。

实现：`patch/powerline.c`、`patch/boxdraw.c`；`wl.c` 的几何字符 run 在 shaping 前
直接生成一格一个绘制项。新增文件是 `wl.o` 的显式构建依赖。

无显示服务器回归测试：`tools/powerline_test.sh`。测试在临时目录构建，不改动当前
字体配置或安装终端。编译后可运行 `./st -e sh tools/powerline_demo.sh` 查看拼接示例。

### 扩展 Unicode 几何字符

同一个 `BOXDRAW_PATCH` / `boxdraw = 1` 开关也接管 Unicode 几何绘图字符：

- 完整的 `U+2500–U+259F` 框线／块元素：补齐双段、三段、四段虚线及斜线，
  `╭╮╰╯` 使用真正的圆弧并与原有直线的像素中心、线宽对齐。
- Legacy Computing 六分块、分数斜坡、三角形分块、八分条纹、棋盘格与填充图案。
- 新旧单线／双线斜线网格、菱形，含补充平面 `U+1CCxx`、`U+1CExx` 字符。
- 单色圆、椭圆、扇形、半圆和四分／十二分圆弧；多格圆弧采用共同的几何坐标，
  不依赖字体是否包含较新 Unicode 字形。

范围按照几何语义筛选，不接管分数字符、乐谱符号、彩色圆形 Emoji 或普通 Nerd 图标。
闭合图形的字重／斜体不改变占格边界；线条可以按 `boxdraw_bold` 加粗。
极小单元格无法表示所有分块或虚线细节，采用像素取整并限制在当前单元格内。
圆形保持圆形比例，只有椭圆及指定多格圆弧使用独立横／纵半径；不强行将圆形拉伸
填满高而窄的单元格。双斜线按平行笔画绘制，不推断邻格连接，连续铺排的角点可能
有间断；内建绘制并不意味着任意字符组合都能组成无缝图案。

实现分为 `patch/boxlines_extra.c`、`patch/boxblocks_extra.c`、`patch/boxcircles.c`、
`patch/boxmosaics.c`、`patch/boxbranch.c`，由 `patch/boxextra.c` 统一管理裁剪和 Cairo
状态。`BDX` 编码以 12 位（4 位页号 + 8 位码点）保存 Unicode
页号及页内码点，在原有 16 位 boxdraw 接口中保留补充平面字符，不截断成低字节。
普通 ASCII 文本在扩展几何判定入口快速排除；没有新增库依赖或常驻字形缓存。

回归测试：`tools/box_geometry_test.sh`。`tools/box_geometry_inventory.h` 记录了参考
`box.txt` 中的 445 个几何字符、追加的 423 个 Ghostty 兼容字符，以及 12 个保留
字体绘制的字符。测试不依赖字体或显示服务器，在临时副本中验证连字、boxdraw、
branch 开关、完整 Unicode 码点路由、像素边界、字形语义、缩放和状态恢复。
原有 `tools/powerline_test.sh`、`tools/font2_test.sh` 应一并运行。

编译后可在新终端中运行 `cat ~/Notes/box.txt` 检查完整拼接图；此文件仅作输入，
构建和测试不会修改它。运行 `./st` 只试用本地构建，`make` 不会安装系统终端。

## 6. 已知问题 / 待办

### 内存优化（2026-09-21 晚）
对比 foot 的 `smaps_rollup`，差距主要在 Anonymous（堆）与 libgd 拖入的共享库。已做：

| 项 | 改动 | 效果 |
|---|---|---|
| 去 libgd | `netwmicon.c` 空实现，图标走 `st.desktop`（`Icon=st`）；`config.mk` 不再链 gdlib；移除 xdg-toplevel-icon 协议 | `ldd` 49 → 30 个库，文件页 −10 MB |
| 滚动缓冲懒分配 | `reflow.c`：`tnew` 不再预分配 2000 行；`tscrollup` 滚入时 `xmalloc`；`treflow` 释放 `histf` 之外的行；新增 `tfreehist()` 供 `treset`/`ED 3` 调用 | 空闲终端 −12 MB；`ED 3` 真正释放 |
| Glyph 32 → 20 字节 | `st.h`：undercurl 的 `ustyle + ucolor[3]`（16 B）打包成一个 `uint32_t ul`（`UL_STYLE/UL_CTYPE/UL_RGB/UL_SET_*` 宏） | 所有行 −37% |
| `NBUFFERS` 4 → 2 | 常驻 offscreen surface 下两个 shm buffer 足够 | 虚拟 −17 MB |
| `malloc_trim(0)` | `cresize()` 与 `tfreehist()` 之后归还堆空洞 | resize 不再累积增长 |

实测（1906×1152，190 列）：启动 49 → **33 MB**，拉宽 61 → **42 MB**，滚满 2000 行 70 → **~50–58 MB**，连续 12 次 resize 75 → **58 MB（不再增长）**，`ED 3` 后 −6 MB。foot 同尺寸 52 MB。剩余差距是常驻 cairo offscreen surface（8.8 MB），如需再降需改为直接在 shm buffer 上绘制。

### 已修复（2026-09-21 下午）
- **fcitx5 无法输入**：新增 text-input-v3 支持（见 §2），Hyprland + fcitx5-rime 实测 preedit/候选/提交均正常。
- **背景透明但字体不透明**：`xdrawglyphfontspecs` 中 fg 若继承了半透明 alpha（反显/选区/光标与默认 bg 互换时），统一改为不透明后再绘制（`opaquefg`），只让背景保留 alpha。
- **快速多次 Super+F（`colresize`）后 segfault**：两次 configure（950→1906→950）之间没有 present 时，`adddamage()` 按当时的 `win.w` 把 1906 宽的脏矩形记到了仍为 950 宽的空闲 buffer 上；尺寸回到 950 后该 buffer 不会被重建，`present()` 的 memcpy 就越过 offscreen surface 末尾。修复：`adddamage()` 按每个 buffer 自身尺寸裁剪脏矩形，`present()` 再按 offscreen 与 shm buffer 二者尺寸兜底裁剪。ASan 下 180 次快速切换无报错。
- **空闲时 CPU 60–70%、发热**：Wayland 合成器对每次 `wl_surface.commit` 都会回一个 `wl_buffer.release` 事件，主循环被它唤醒后无条件 `draw()`，而 `draw()` 每次都重画光标格 → 产生 damage → 再 commit → 再 release……形成死循环（失焦时光标不画所以不明显）。修复：`run()` 增加 `needdraw` 标志，只有 tty 输出 / 输入事件 / 光标闪烁定时器才触发 `draw()`。实测聚焦空闲 0% CPU，输入、`seq 200000` 刷屏、连续 colresize、fcitx5 均正常。

7. **`x.c` 与 X11 构建PLAN**——当前 `x.c` 留作参考；确认稳定后可删除，并把 `protocols/` 加入 `.gitignore`（目前是未跟踪状态）。

### Ghostty 兼容扩展：423 个字符

以 Ghostty v1.3.1 / commit `76895d97b74ff6b24c2b1543bcd69ccc18048a4d` 的
sprite 注册范围为参考，增加以下全部 A–I 分组：

| 组 | 类型 | 码点 | 数量 |
|---|---|---|---:|
| A | 空心直角三角形 | U+25F8–25FA、25FF | 4 |
| B | 左侧 1/3、2/3 宽块 | U+1FBCE–1FBCF | 2 |
| C | 十六分块、局部条块、居中四分块 | U+1CE90–1CEAF、1FBE4–1FBE7 | 36 |
| D | 框线接头 | U+1CC1B–1CC1E、1CE16–1CE19 | 8 |
| E | 反色斜线／菱形 | U+1FBBD–1FBBF | 3 |
| F | 八分块：2 列 × 4 行 | U+1CD00–1CDE5 | 230 |
| G | 分离四分块 | U+1CC21–1CC2F | 15 |
| H | 分离六分块 | U+1CE51–1CE8F | 63 |
| I | Git 风格分支图 PUA | U+F5D0–F60D | 62 |

默认全部启用。I 组额外受 `config.h` 中 `boxdraw_branch = 1` 控制；改成 `0`
并重新编译可仅恢复这些 PUA 字符的字体绘制。它们是私用区约定，不是通用 Unicode
字义。普通 Nerd 图标和字母不受影响；没有把光标或下划线等非字符装饰混入此列表。

G/H 的留缝是符号语义，不应填平。极小格子无法保留分离块的空隙时退化为无间隙
的相应占位，可能消失的分块也不会溢出当前格子。八分块的 230 项位图由 Unicode
名字导出，并与上述 Ghostty 版本的数据交叉校验。反色图案和分支节点在临时 alpha
组中计算覆盖率，最后只合成一次前景色；不清除终端背景，也不将其强行变成不透明。

内建码点总数现在为 **1,143**（此前 720 + 本次 423），包含全部上述版本 Ghostty
的 1,084 个有效 sprite 码点，以及 st 原有另外 59 个几何字符。U+1FB93 空占位不
接管。这里比较的是码点覆盖范围，不承诺与 Ghostty 逐像素相同。

演示：
```sh
./st -e python3 tools/box_geometry_demo.py
```

额外测试头 `tools/box_geometry_extra_test.h` 校验完整码点路由（包括 Branch 独立
关闭）、全部八分块掩码、分离块空隙、反色补集、节点空心和连线方向、渐隐方向、
半透明合成及 1.25/1.5/2 倍 CTM 缩放。可选的 sanitizer 运行方式：
```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
BOX_GEOMETRY_TEST_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
BOX_GEOMETRY_TEST_LDFLAGS='-fsanitize=address,undefined' tools/box_geometry_test.sh
```
此命令关闭了内存泄漏检测（图形库可能保留进程级缓存）；地址和未定义行为检查仍开启。
