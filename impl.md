# st：X11 → Wayland 迁移——难度评估与实现原理

> 对象：st 0.9.3 + 19 个 patch（st-flexipatch 体系）
> 结果：`x.c`（4082 行）→ `wl.c`（5234 行，163 个函数）；其余文件 diff 约 +1600/−1450 行
> 环境：Hyprland 0.52 / wayland 1.26 / cairo 1.18 / xkbcommon 1.13 / fcitx5

---

## 一、难度评估

### 1. 总体结论

**中高难度**。st 的核心（`st.c` 终端仿真、VT 解析、选区、滚动等）与显示层是干净分离的：`win.h` 定义了约 25 个 `x*` 函数作为唯一接口，所以"换后端"在架构上是可行的。难点不在于接口数量，而在于 **X11 与 Wayland 的模型差异**——X11 提供的是"服务器替你做事"（渲染、剪贴板、光标、输入法、窗口管理），Wayland 提供的只是"你自己画好，我来合成"。因此几乎每一项 X11 功能都不是一对一翻译，而是要**在客户端从头实现**。

### 2. 分项难度

| 模块 | X11 做法 | Wayland 做法 | 难度 | 说明 |
|---|---|---|---|---|
| 窗口创建/映射 | `XCreateWindow` + WM hints | `wl_compositor` + `xdg_wm_base` + `xdg_toplevel`，等 configure 后才能 attach buffer | ★★ | 协议样板多，但逻辑固定；必须处理"先收到 configure 再 draw"的异步顺序 |
| **渲染** | Xft/Xrender：服务器端绘字形、填矩形，`XdbeSwapBuffers` | 自己申请 `wl_shm` 共享内存，用 cairo 软件光栅化，再 `attach + damage + commit` | ★★★★ | 最大的一块。要自己管理多缓冲、脏矩形、premultiplied alpha、字形缓存 |
| 字体 | Xft（fontconfig + FreeType + 服务器端 glyph cache） | fontconfig 匹配 + `cairo-ft` scaled font；fallback 链自己维护 | ★★★ | 保留了 st 原来的 fontconfig 匹配逻辑，只换渲染对象；harfbuzz（ligatures）需要拿 FT_Face 锁 |
| 键盘 | `XLookupString` + `XK_*` keysym | `wl_keyboard` 给 keymap fd → xkbcommon 解析；**按键重复要自己实现** | ★★★ | keysym 值体系与 X11 兼容（`XKB_KEY_*` == `XK_*`），config.h 几乎不用改；repeat 是 X 服务器免费送的，这里要自己计时 |
| 鼠标 | `ButtonPress/MotionNotify`，滚轮是 Button4/5 | `wl_pointer` button/axis/frame；滚轮是连续值 + discrete + value120 三种 | ★★ | 要把滚轮"归一化"回 Button4/5 才能复用 st 的 `mousereport/mousesel` |
| **剪贴板** | `XSetSelectionOwner` / `XConvertSelection`，同步式属性传输 | `wl_data_device`（CLIPBOARD）+ `zwp_primary_selection_v1`（PRIMARY），全部走 **fd + pipe 异步** | ★★★★ | 读写都是 fd，不能阻塞主循环；要把 pipe 接进 `pselect`，写端还要处理 EAGAIN 分片 |
| 光标形状 | `XCreateFontCursor(XC_xterm)` | `wp_cursor_shape_v1`（新）或 `wl_cursor_theme` 自己贴图（旧） | ★★ | 两条路都得写，因为不是每个合成器都支持 cursor-shape |
| **输入法** | XIM（`XOpenIM`/`XCreateIC`，服务器转发） | `zwp_text_input_v3`：preedit 要**自己画**，commit 要自己写 pty，光标矩形要主动上报 | ★★★ | 协议本身简单，但 preedit 渲染要嵌进 st 的绘制流程，且要避免 `done → 重绘 → commit → done` 的乒乓 |
| HiDPI | 无（X11 靠 Xft.dpi 全局） | `wp_fractional_scale_v1` + `wp_viewporter`：逻辑尺寸 × scale = buffer 像素，字体按 scale 重载 | ★★ | 概念清楚，但所有坐标要区分"逻辑 px"和"buffer px"两套 |
| 窗口图标/标题/铃声 | `_NET_WM_ICON` 属性 / `XStoreName` / `XBell` | `xdg_toplevel_icon_v1` / `xdg_toplevel_set_title` / `xdg_system_bell_v1` | ★ | 直译 |
| 事件循环 | `XPending`/`XNextEvent`，事件即命令 | `wl_display_prepare_read/read_events/dispatch_pending` 三段式；**合成器会对每次 commit 回 release 事件** | ★★★ | 隐蔽坑：不能"有事件就重绘"，否则 release→draw→commit→release 死循环烧 CPU |
| patch 移植 | — | boxdraw/sixel/netwmicon/openurlonclick 直接调 Xft/X11 API，需要重写；其它 patch 只换 keysym 名 | ★★★ | 19 个 patch 里 4 个需要真正重写，其余是机械替换 |

### 3. 工作量与坑的分布

- **样板代码**（协议 listener、registry bind、scanner 生成规则）：约 30%，枯燥但不难。
- **渲染子系统**（shm/damage/cairo/字体/alpha）：约 35%，需要理解合成器的 buffer 生命周期与 premultiplied alpha。
- **异步化**（剪贴板 pipe、按键重复定时器、IME）：约 20%，st 原来的 `run()` 是围绕 X 事件设计的，要把新的 fd/定时器塞进 `pselect`。
- **调试阶段暴露的问题**（本次实际踩到的）：
  1. 快速连续 resize 时脏矩形超出旧 buffer → `memcpy` 越界 segfault；
  2. `wl_buffer.release` 唤醒主循环 → 无条件重绘 → 60–70% CPU；
  3. 反显/选区/光标与半透明背景色互换后，**文字**变成半透明；
  4. fcitx5 报的 `cursor_begin/end` 是"高亮区间"而非单点光标。

  这类问题共同点：**编译通过、首屏正常，但在动态场景下才暴露**。ASan + `WAYLAND_DEBUG=1` + `hyprctl dispatch` 脚本化压测是定位它们的关键手段。

### 4. 结论

如果只求"能显示、能打字"，一周内可做出雏形；要达到**日常可用**（透明、IME、HiDPI、剪贴板双通道、19 个 patch 不退化、无 CPU/内存异常），需要对 Wayland 的 buffer/事件模型有正确心智模型，并做系统性压测。这是一次"重写显示层"而非"移植"。

---

## 二、实现原理

### 1. 分层与接口

```
st.c (终端仿真, 不变)  ──win.h──▶  wl.c (显示/输入后端)
                                     ├── 渲染: wl_shm + cairo
                                     ├── 输入: wl_keyboard/xkbcommon, wl_pointer, text-input-v3
                                     ├── 选区: wl_data_device, primary-selection
                                     └── 窗口: xdg-shell, decoration, viewporter, fractional-scale,
                                              cursor-shape, toplevel-icon, system-bell
```

`win.h` 接口全部保留原签名（`xdrawline`、`xdrawcursor`、`xsetsel`、`xximspot`…），`st.c` 只删掉了 X11 头文件。`st.h` 中与 X11 类型绑定的结构（`XWindow`、`XSelection`、`Color`、`Font`、`KeySym`）被重新定义，但保留了原名，使 patch 代码改动最小。

### 2. 初始化与协议协商

1. `wl_display_connect` → `wl_display_get_registry` → `registry_global` 回调里按接口名 `wl_registry_bind`。所有扩展协议（decoration、viewporter、fractional-scale、cursor-shape、toplevel-icon、system-bell、text-input）都是**可选**的：绑不到就退化（如 cursor-shape 缺失则用 `wl_cursor_theme` 贴图，fractional-scale 缺失则用 `wl_output`/`preferred_buffer_scale` 的整数 scale）。
2. 两次 `wl_display_roundtrip`：第一次拿到 globals，第二次拿到 seat capabilities 与 shm formats。
3. 创建 `wl_surface` → `xdg_surface` → `xdg_toplevel`，设置 app_id/title，`wl_surface_commit` 一次空 commit 请求 configure。
4. `run()` 开头阻塞 dispatch 直到 `xw.configured`；`toplevel_configure` 只记录 `pendingw/h`，`xdgsurface_configure` 才 `ack` 并调用 `updatesize()`——这是协议要求的"configure 是原子批次"语义。
5. 协议 C 代码由 Makefile 内的 `wayland-scanner` 规则从 `/usr/share/wayland-protocols/*.xml` 生成到 `protocols/`，共 10 个协议。

### 3. 渲染管线

#### 3.1 尺寸体系

- `xw.lw/lh`：逻辑尺寸（合成器 configure 给的）。
- `xw.scale`：`wp_fractional_scale_v1.preferred_scale / 120`，或整数 `outputscale`。
- `win.w/h = round(lw × scale)`：**buffer 像素**，所有绘制坐标都用它。
- `wp_viewport_set_destination(lw, lh)` 告诉合成器"这块 buffer 显示成逻辑 lw×lh"。
- 字体像素大小 = `usedfontsize × scale`，scale 变化时 `xunloadfonts/xloadfonts` 重载并 `cresize`。

#### 3.2 离屏 surface + 多缓冲 + 脏矩形

```
cairo image surface (xw.buf, ARGB32, 常驻)
        │  drawrect / drawglyphs / drawlines / sixel paint  → adddamage(x,y,w,h)
        ▼
present():
   b = getbuffer(win.w, win.h)     // 4 个 wl_shm buffer 轮转, busy 由 wl_buffer.release 清零
   memcpy 仅 b 的累计脏矩形 (xw.buf → b->data)
   wl_surface_attach + damage_buffer(脏矩形) + commit
```

- **为什么要离屏 surface**：合成器持有 buffer 期间（`busy`）客户端不能写它；而 st 的绘制是增量的（只画脏行），必须有一份"完整的当前画面"作为源。
- **buffer 数量**：`NBUFFERS = 2`。有常驻离屏 surface 时，"合成器持有一个、我们写另一个"即可；每个 buffer 是 `win.w × win.h × 4` 字节，多开只是浪费。
- **为什么每个 buffer 各自累计 damage**：buffer A 提交后，随后几帧的改动 A 都没收到；等 A 再次被复用时，要把这期间的所有脏区一起拷过去。所以 `adddamage` 把矩形合并到**所有**已分配的 buffer 上，`present` 只拷当前 buffer 的那份。
- **resize 时的边界**：buffer 尺寸不同于 `win.w/h` 时在 `getbuffer` 里销毁重建（damage 置为全屏）；`adddamage` 与 `present` 都按 buffer 与 surface 自身尺寸裁剪脏矩形——这是修复"快速 resize segfault"的关键。
- `wl_shm` 内存来自 `memfd_create`（回退 `shm_open`），格式固定 `ARGB8888`，与 cairo `ARGB32` 字节布局一致，可直接 memcpy。

#### 3.3 颜色与透明

- `Color` 改为 16-bit RGBA + `0xAARRGGBB pixel`；`setsource()` 把它喂给 `cairo_set_source_rgba`。
- 背景矩形用 `CAIRO_OPERATOR_SOURCE`（覆盖写，等价 `XftDrawRect`），字形用 `OVER`。因为 `wl_shm` ARGB 是 premultiplied，cairo 自动完成预乘，合成器直接混合桌面壁纸即可实现 ALPHA patch 的效果。
- **文字不透明**：st 的反显/选区/光标逻辑通过"交换 fg/bg 指针"实现，这会让 `defaultbg` 的 alpha 跑到前景上。`xdrawglyphfontspecs` 在进入前景绘制前把 fg 复制一份并强制 `alpha = 0xffff`，只让背景保留透明。

#### 3.4 字体

- 匹配：沿用 st 的 fontconfig 流程（`FcNameParse` → `FcConfigSubstitute` → `FcFontMatch`），`xloadfont` 拿到 `FcPattern *match`。
- 光栅化：`opencairofont()` 用 `cairo_ft_font_face_create_for_pattern(match)` + `cairo_scaled_font_create(face, 像素矩阵, options)`；抗锯齿/hinting/subpixel 从 pattern 的 `FC_ANTIALIAS/FC_RGBA/FC_HINT_STYLE` 映射到 `cairo_font_options`。
- 字形查找：`glyphindex()` 用 `cairo_scaled_font_text_to_glyphs` 得到 glyph index，带一个小哈希缓存；找不到则走 `fallbackfont()`——保留 st 的 `frc[]` fallback 环，只是元素从 `XftFont*` 变成 `cairo_scaled_font_t*`。
- 绘制：`xmakeglyphfontspecs` 产出 `(font, glyph, x, y)` 数组，`drawglyphs` 按连续相同 font 分段 `cairo_show_glyphs`。
- Ligatures（harfbuzz）：`hb.c` 在 shape 前 `cairo_ft_scaled_font_lock_face` 拿到 `FT_Face` 建 `hb_font`，shape 后 unlock。

#### 3.5 patch 的渲染部分

- **boxdraw**：所有 `XftDrawRect` → `bdrect()`（cairo SOURCE 矩形）；shade 混合改为纯内存算 RGB 后再画。
- **undercurl**：`drawlines()` 用 cairo 路径画折线/波浪，线宽与 cap 对齐原 `XDrawLines` 语义。
- **sixel**：像素格式本来就是 `0xAARRGGBB`，直接 `cairo_image_surface_create_for_data` 包装，缩放用 cairo（原尺寸 NEAREST，其它 GOOD），缓存到 `im->pixmap`；透明图 OVER、不透明 SOURCE；X11 的 clipmask 概念不再需要，因为 cairo 直接按 `ATTR_SIXEL` 分段 clip。
- **netwmicon**：最初用 gd 解码 PNG 经 `xdg_toplevel_icon_v1` 提交，后因 libgd 拖入 26 个库（avif/x264/x265/X11…）而改为**不做任何事**——Wayland 合成器与任务栏按 `app_id` 匹配 `st.desktop` 的 `Icon=st` 自行解析 `pixmaps/st.png`。

### 4. 事件循环

```
loop:
  while (wl_display_prepare_read() != 0) wl_display_dispatch_pending();
  wl_display_flush();
  pselect({tty, wayland fd, paste pipe, 待写的 selection fd...}, timeout)
  timeout 由 min/maxlatency 节流、按键重复、光标闪烁、SU 超时共同决定
  read_events / cancel_read
  ttyread(); dispatch_pending(); repeatkey(); pasteread(); sendqueue_flush();
  if (tty 有输出 || wlevents 变化 || 闪烁到期) needdraw = 1
  ...节流逻辑同原 st...
  if (needdraw) draw();     ← 关键：不能无条件 draw
```

- `wlevents` 计数器：每个"值得重绘/处理"的 listener 回调（键盘、指针、focus、configure、IME done、paste 完成）自增；主循环用它判断是否有输入事件，替代 X11 的 `XPending`。
- `wl_buffer.release` **不**自增 `wlevents`，也不能触发 draw。否则 draw → 画光标 → damage → commit → release → 唤醒 → draw……这就是曾经的 60–70% CPU 死循环。
- `prepare_read/read_events` 三段式是 libwayland 在单线程 + 自管 poll 时的正确用法，避免 `wl_display_dispatch` 内部阻塞。

### 5. 键盘

- `keyboard_keymap`：mmap 合成器给的 fd → `xkb_keymap_new_from_string` → `xkb_state_new`。
- `keyboard_modifiers`：`xkb_state_update_mask`；`modstate()` 把 Shift/Ctrl/Alt/Super/NumLock 映射成 X11 的 `ShiftMask/ControlMask/Mod1Mask/Mod4Mask/Mod2Mask` 位值，因此 `config.h` 里的 `shortcuts[]`/`key[]` 表与 `match()` 逻辑完全不变。
- `keyboard_key`：keycode + 8 → `xkb_state_key_get_one_sym` + `xkb_state_key_get_utf8`，之后就是 st 原来的 `kpress` 三段（shortcut → kmap → 直接写 UTF-8）。
- **按键重复**：Wayland 不发重复事件。`keyboard_repeat_info` 给 rate/delay；`kbrepeat` 记录当前按住的 key 与下次触发时间，`repeattimeout()` 参与 `pselect` 超时计算，`repeatkey()` 到期后再次调用 `kpress`。焦点丢失或 key release 时取消。
- `XK_*` → `XKB_KEY_*`：数值完全一致，`config.h` 与 patch 只做了 sed 更名。

### 6. 指针

- `pointer_enter/motion/button/axis/frame` 拼成一个 X11 风格的 `MouseEvent {x, y, button, state, type}`，交给原有的 `mousereport / mousesel / mouseaction / bpress / brelease / bmotion`。
- Linux 的 `BTN_LEFT/MIDDLE/RIGHT` → Button1/2/3；滚轮统一归一化：优先 `axis_value120`（高分辨率）→ `axis_discrete` → 连续 `axis` 值每 10 个单位累计为一格，最终都变成 Button4/5（垂直）与 Button6/7（水平），使 SCROLLBACK_MOUSE 等 patch 无感。
- 光标形状：`wlsetpointer(POINTER_TEXT/LINK/DEFAULT/HIDDEN)` → 有 `wp_cursor_shape_device_v1` 就 `set_shape`，否则 `wl_cursor_theme_load` + 自己的 `cursorsurface` attach。`config.h` 的 `mouseshape` 从 `XC_xterm` 常量变成字符串 `"text"`。

### 7. 选区与剪贴板

- 两条通道：`wl_data_device`（CLIPBOARD）和 `zwp_primary_selection_device_v1`（PRIMARY），CLIPBOARD_PATCH 的"选区同时写入 clipboard"语义保留。
- **拥有**（st → 别人）：`xsetsel` 创建 `wl_data_source`，offer 一组文本 mime；对方请求时 `datasource_send(fd)` 把数据交给 `sendjob`（非阻塞 write，EAGAIN 时挂到 `pselect` 的写集合分片发送），避免大段文本阻塞主循环。`set_selection` 必须带最近一次输入 serial（`xw.serial`）。
- **获取**（别人 → st）：`data_offer` 到达时挑一个文本 mime 记在 `OfferData`；粘贴时 `pastestart` 建 pipe，`wl_data_offer_receive(mime, 写端)`，读端 `O_NONBLOCK` 加入 `pselect`；`pasteread` 分块读并按 bracketed-paste 包裹写入 pty（KEYBOARDSELECT 搜索模式下则送进搜索框）。
- OSC 52 与选区写入共用 `xsetsel`/`xclipcopy`。

### 8. 输入法（text-input-v3）

- `seat` 拿到 keyboard capability 后 `zwp_text_input_manager_v3_get_text_input`。
- `enter` → `enable` + `set_content_type(TERMINAL)` + `set_cursor_rectangle` + `commit`；`leave` → `disable` + `commit`，清空 preedit。
- 协议是"批量 + done"模型：`preedit_string/commit_string/delete_surrounding_text` 只暂存，`done` 时一次应用：先把 commit 字符串 `ttywrite` 到 pty，再用新 preedit 替换旧的并标记该行 dirty。
- **preedit 渲染**：`drawpreedit(cx, cy)` 在 `xdrawcursor` 之后调用，从光标格开始逐字 `mbrtowc` + `wcwidth`，用 `xdrawglyph` 画带下划线的临时 Glyph（不进 `term.line`，所以不会污染终端内容），换行/超宽截断。
- **光标矩形**：`st.c` 的 `draw()` 每帧调用 `xximspot(term.ocx, term.ocy)`，把光标格子换算成**逻辑像素**后 `set_cursor_rectangle + commit`，候选窗口即可贴在光标下方。IME 的 `done` 会自增 `wlevents` 触发一次重绘，但重绘并不会再产生新的 IME 事件（commit 是幂等的），所以不会乒乓。

### 9. 内存布局与开销

`Glyph` 是滚动缓冲的基本单元，`HISTSIZE(2000) × 列数 × sizeof(Glyph)` 决定了大头：

- `Glyph` 20 字节：`u/mode/fg/bg` 各 4 B + undercurl 的样式/颜色打包成 1 个 `uint32_t ul`（`bits 28-31` 颜色类型、`24-27` 样式+1、`0-23` RGB 或调色板索引）。原 patch 用 `int ustyle + int ucolor[3]` 是 32 字节。
- 历史行**懒分配**：`term.hist[]` 初始全 NULL，`tscrollup` 把可见行换入历史时才为换出的空行 `xmalloc`；`treflow` 只保留 `histf` 行，其余 `free` 置 NULL；`treset`/`ED 3` 调 `tfreehist()`。所有读历史的路径（`TLINE/TLINEABS`、`rscrolldown`、kbdselect 搜索）都以 `histf` 为界，故不会碰到 NULL。
- `cresize()` 与 `tfreehist()` 末尾 `malloc_trim(0)`：reflow 会 free 上千个 ~4 KB 小块，glibc 默认不把堆中间的空洞还给内核，不 trim 的话每次 resize RSS 都只涨不跌。
- 常驻开销：cairo offscreen surface 1 份 + shm buffer 2 份，各 `w×h×4`；1906×1152 时约 8.8 MB × 3（shm 只算被写过的页）。

### 10. 其它

- **标题/图标名**：`xdg_toplevel_set_title`；`xseticontitle` 在 Wayland 无对应物，空实现。
- **铃声**：`xdg_system_bell_v1_ring`，缺失时退回 VISUALBELL。
- **装饰**：`zxdg_toplevel_decoration_v1` 请求 server-side（无边框 WM 如 Hyprland 会忽略）。
- **`$WINDOWID`**：Wayland 无窗口 id，启动时 `unsetenv`；依赖它的 `openurlcmd`（`dmenu -w $WINDOWID`）需换成 Wayland 启动器。
- **构建**：`config.mk` 依赖 `wayland-client wayland-cursor xkbcommon cairo cairo-ft fontconfig freetype2 harfbuzz gdlib`；`_XOPEN_SOURCE=700 -D_DEFAULT_SOURCE` 是为了 `memfd_create/pipe2`。

---

## 三、验证方法（可复用）

| 目的 | 手段 |
|---|---|
| 键盘 | `wtype -d 30 text`、`wtype -k Return` |
| 鼠标 | `ydotool` + `hyprctl dispatch` 定位 |
| 剪贴板 | `wl-copy` / `wl-paste` / `wl-paste -p` |
| 截图 | `grim -g "x,y wxh"` 后读图检查 |
| 协议流 | `WAYLAND_DEBUG=1 ./st 2>log`，`awk '{print $4}' | sort | uniq -c` 统计事件分布 |
| 内存错误 | `make CFLAGS="-g -O0 -fsanitize=address" LDFLAGS="-fsanitize=address"` |
| 压测 | `for i in $(seq 60); do hyprctl dispatch 'hl.dsp.layout("colresize +conf")'; sleep 0.0$((RANDOM%9+1)); done` |
| CPU | `top -b -n 3 -d 1 -p $(pgrep -x st)` |
| IME | `fcitx5-remote -o` 后 `wtype nihao`，观察 preedit 与候选框 |
