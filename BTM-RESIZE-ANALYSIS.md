# btm 快速全屏切换显示异常分析

2026-11-20，测试 bottom 0.15.0、当前工作区 st（fcft + pixman）和系统 foot 1.28.0。

## 结论

已复现与“恢复全屏后仍残留半屏内容”一致的故障路径：btm/Ratatui 的增量绘制缓存
没有在快速尺寸往返后失效。终端缩小时已经截掉部分字符，应用却仍以为这些字符存在，
随后只更新变化的数据，留下缺字、断开的边框和旧内容。

此案例不属于 st 的像素缓存或 fcft/pixman 重绘故障；foot 也能出现相同异常。
尚未捕获用户原会话的事件和输出，因此这个归因针对已复现的路径，不排除其他时序问题。

## 机制

1. btm 已按大尺寸绘制，Ratatui 保存了上一帧的字符缓存。
2. 终端经历“大 → 小 → 大”，备用屏幕在缩小时截去右侧和底部内容。
3. 应用处理通知或再次绘制时，读到的已经是最终大尺寸。
4. Ratatui 发现尺寸与上次绘制相同，不重建差分缓存；静态字符没有变化便不重新输出。

本机 btm 二进制的源码路径对应 crossterm 0.29.0、ratatui-core 0.1.2。
本地缓存中的这些版本显示：crossterm 收到 SIGWINCH 后查询当前 TIOCGWINSZ；
Ratatui 每次绘制前调用 autoresize，但只有当前尺寸与 last_known_area 不同时才 resize/clear。
这不是单纯“没收到 SIGWINCH”：收到通知后读到相同的最终尺寸，也可能发生。

st 的尺寸更新路径见 [wl.c](wl.c)、备用屏幕截断见 [patch/reflow.c](patch/reflow.c)。
`cresize()` 调用 `tresize()`、`xresize()`、`ttyresize()`，后者更新 PTY 尺寸；
`xresize()` 标记全屏重绘。foot 的 `render.c`、`grid.c` 也在备用屏幕缩小时截去超出范围的字符。

## 实测证据

| 检查 | 结果 |
|---|---|
| 独立 PTY，180×50 → 90×50 → 180×50 | btm 继续输出动态数据，但漏掉静态边框 |
| 不暂停 btm，半屏停留 0～10 ms，各做 10 次往返 | 每档均未输出清屏或静态边框字符 |
| 同一段真实 btm 输出回放到 st parser | 加入尺寸往返后，1599 个原有非空字符变为空格，全部位于右半屏 |
| st 601 次尺寸循环，每个 REFLOW 测试配置分别执行 | PTY 尺寸正确；缓冲区耗尽后恢复正常；增量画面与全量重绘逐字节一致 |
| 独立 Wayland 会话，st 与 foot 对照 | 两者均能产生右半屏边框、文字丢失 |
| 异常后只强制 st 全屏重绘 | 字符网格和像素数据均完全不变，缺字仍在 |
| btm 打开帮助后关闭 | 本次 GUI 实验的边框数由 874 恢复为 1855 |

GUI 确定性复现短暂暂停了测试 btm，确保它跨过中间尺寸；随后恢复并等待刷新。
st 的最终网格正确为 177×49，窗口为 1600×1000。暂停仅用于控制时序，
独立 PTY 的上述快速往返实验没有暂停进程。GUI 另外四档不暂停压力测试的末帧恢复正常，
没有据此声称日常切换必然触发、或测得了实际故障概率。

只强制 st 重绘前后的像素 SHA256 都为：
`e32898efd8314f6a194f4bff9f8abb7e1e7fbee4c69d365914428ee00f190ec5`。
这排除了该画面来自“字符网格正确，但旧像素未更新”的解释。

## 回归验证与材料

本次仅扩展 [tools/render_state_test.c](tools/render_state_test.c) 的缩放覆盖，未改生产渲染逻辑。
`tools/render_state_test.sh` 的五种配置正常构建及 ASan/UBSan 均通过。
新增尺寸循环在四个 REFLOW 配置执行；no-reflow 运行原有测试。
覆盖备用屏幕、连续尺寸变化、仅像素尺寸改变、滚动后缩放、三个旧缓冲区全忙及释放后重试。
Sanitizer 使用 `halt_on_error=1`，关闭了 LeakSanitizer。

原始材料暂存于以下目录（`/tmp` 清理后不可用）：

- [PTY 实验、源码依据和脚本](/tmp/st-btm-resize-audit-s4uxf3c7/README.md)
- [st parser 回放及测试记录](/tmp/st-btm-resize-audit-s4uxf3c7/st-replay-evidence.txt)
- [st 异常截图](/tmp/st-btm-resize-j1y3nh60/gui2/st/02-roundtrip.png)
- [foot 异常截图](/tmp/st-btm-resize-j1y3nh60/gui2/foot/02-roundtrip.png)
- [st 中由应用重绘后恢复的截图](/tmp/st-btm-resize-j1y3nh60/gui2/st/04-app-repaint.png)
- [独立 Wayland 测试脚本](/tmp/st-btm-resize-j1y3nh60/run.py)

本次 GUI 使用当前源码的隔离插桩副本；`/usr/local/bin/st` 与工作区二进制哈希不同，
没有把工作区的验证结果当成已安装版本的验证结果。

## 应对与修复方向

临时恢复：切到半屏，等待界面完成一次刷新，再切回全屏。默认刷新率下可先等约一秒，
若配置了更慢刷新则等待更久。本次 `?` 打开帮助后按 Esc 也恢复了边框；
它依靠界面变化补画，不保证覆盖所有缺失字符。Ctrl+L 在本机 btm 版本中无效。

应用侧修复应在处理 resize 通知时使上一帧差分缓存失效，即使报告的最终行列数相同，
也强制下一帧完整绘制；还需考虑绘制过程中发生 resize 的情况。
终端侧合并同批 configure 可以减少中间网格截断，但无法保证应用不会跨过多个事件批次，
因此不能把延迟或增加终端重绘当作这个竞态的完整修复。
