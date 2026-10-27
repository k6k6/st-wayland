# 2026-10-27 性能与内存优化

当前改动保留字体优先级、连字、SIXEL 缩放与历史、选区传输和 Wayland 多缓冲语义。
`config.h`、`patches.h` 与编译选项不变。

- SIXEL 原尺寸缓存直接包装 `im->pixels`，避免第二份完整像素分配；缩放缓存继续从原图生成。
  原图始终保留到 `delete_image`，surface 在像素释放前销毁。原尺寸图像节省
  `4 × width × height` 字节（1000×1000 约 3.8 MiB，不含 surface 元数据）。
- 字体回退先筛选样式。1024 槽、固定约 24 KiB 的结果表保存字符／来源字体的查询结果，
  包括缺字；实际字体按 Cairo scaled-font 指针及样式去重。新增字体后重查旧缺字结果，
  字体重载时清空结果。FONT2 顺序不变，不再为每个不同缺字追加相同字体。
- 本帧已完整绘制的旧光标行直接复用。记录来源 Line 指针，避免误用历史行；单格／光标
  覆盖会使对应行的记录失效。行未完整恢复时仍重绘整行，恢复被光标打断的连字。
- 每块 shm buffer 分别累计最多 32 个 damage 矩形，合并包含关系与相邻等跨度区间；
  超过上限后保守退回包围矩形。轮换缓冲只复制这些区域，跳过本帧即将重画的行；
  复制不清除累计 damage，只有该 buffer 提交后才清除。裁剪按每块缓冲自己的尺寸进行。
- primary、clipboard 和并发发送持有不可变内容快照的引用。`setsel` 接管输入分配，
  仅与当前两个选区合并相同内容，不驻留历史文本；正在发送的旧内容不受选区更新影响。
  既有 EAGAIN、EINTR、队列上限和 SIGPIPE 保护保留。
- 修复闪烁属性检测和标脏漏掉最后一行／列的问题，以及 SIXEL ARGB 组装的有符号左移。
- `tools/bench.sh` 现在调用 Python 标准库负载，按实际可见列数构造 ANSI／Unicode，
  输出每次重复的 JSON。CPU 计量排除生产负载子进程；DSR 排空仅确认解析完成，
  不代表画面已由合成器呈现。用法和限制见 [tools/bench.md](tools/bench.md)。

## 验证入口

测试均在临时目录构建，不安装终端或连接实时桌面：

```sh
tools/font2_test.sh
tools/sixel_cache_test.sh
tools/render_state_test.sh
tools/cursortrail_test.sh
tools/selection_snapshot_test.sh
tools/selection_sigpipe_test.sh
tools/blink_bounds_test.sh
python3 tools/bench_test.py
tools/shape_cache_test.sh
tools/hb_face_lifetime_test.sh
tools/fullscreen_alpha_test.sh
tools/powerline_test.sh
tools/box_geometry_test.sh
```

新增绘制测试捕获真实 Wayland 请求，检查分散区域面积、区域上限、裁剪、多缓冲轮换、
脏行复制与缺缓冲；比较完整恢复／跳过恢复的输出，并检查实际整行调用次数，包含宽字、
选区、预编辑、聚焦／失焦、隐藏和光标形状。SIXEL 测试使用真实解析输入，比较透明与
不透明图的原尺寸、缩放、重建、隐藏和擦除，并检查原尺寸共享实际像素指针。

管道测试覆盖 8 个并发请求、真实短写／EAGAIN、EINTR 注入、传输中切换选区、两代内容
完整性、释放和 EPIPE。真实 SIGPIPE 回归使用独立 headless mangowm，同时检查未保护
对照版确实收到 SIGPIPE。修正该脚本的子进程 stdout 继承和存活进程回收，避免命令替换
等待 120 秒的测试子进程。

`font2`、`sixel_cache`、`render_state`、`cursortrail`、`selection_snapshot`、`blink_bounds`
均支持 `CFLAGS` / `LDFLAGS`，已运行 ASan/UBSan：

```sh
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
LDFLAGS='-fsanitize=address,undefined' tools/render_state_test.sh
```

图形库会保留进程缓存，因此上述图形测试关闭泄漏检查；剪贴板测试额外在
`detect_leaks=1` 下运行。通过测试不能替代真实桌面的长期使用和输入法交互验证。

## 对照测量

同配置、同 `-O2 -g -march=native -flto`，私有 headless mango + pixman，
138×34 字符、1252×692 像素；对照为 `ac9a856`。初轮三次交错测试中，60 帧顶部＋底部
更新的累计 damage 面积从 51,983,040 降至 3,906,240 像素（减少 92.49%），矩形数
从 60 增至 120。单行更新的整行绘制次数从 120 降至 60，纯光标移动仍为 60 次。
这些操作量变化确定，不等于整体 CPU 或实际桌面 GPU 的同等降幅。

原尺寸静态内容（粗体／斜体／下划线／连字／CJK／Emoji／几何／SIXEL）的真实会话截图
与对照逐像素相同。无头会话没有自然键盘焦点，光标测量在两边使用相同的临时强制聚焦；
内部强制缩放实验存在回程截图疑点，不计入通过项。缩放缓存本身的像素与释放由独立
headless 单元测试覆盖。

初轮短测三次中位数：混合属性全屏 CPU 109.734→105.050 ms，单行更新
13.937→12.503 ms；样本短且范围重叠，仅作参考。滚屏初轮出现 +6.1%，随后固定
st／负载／合成器到不同物理核，补测两组各 7 对交错运行：保留帧计数的成对 CPU
比值中位数为 0.9946，移除全部实时计数后为 0.9693。两组波动较大，未复现稳定
回退，也不足以宣称滚屏加速。两边各处理 240 万行的 profile 均主要耗在
`tsetchar`／`tputc`／`twrite`／`tscrollup`，没有把回退定位到新 damage 路径的证据。

运行证据位于 `/tmp/st-opt-implementation.9bm35yy7` 和
`/tmp/st-head-compare.AkRGcdhU`（滚屏复核位于其 `scroll-followup.y20aGFGM`）；
临时目录可能被系统清理，测试入口保存在仓库中。
