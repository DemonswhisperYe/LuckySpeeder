# LuckySpeeder build — branch kysmj, tag v0.1.2-kysmj

GitHub Actions run 40186201878（success），四目标产物下载自 workflow artifacts。
Release 页面：https://github.com/DemonswhisperYe/LuckySpeeder/releases/tag/v0.1.2-kysmj

注入 Chaos Zero Nightmare（SSR.app）：iPhone 用 `LuckySpeeder-arm64-apple-ios.dylib`，
TrollFools 注入。

## v0.1.2 修复内容

修复「点加速就卡死」（崩溃日志 1806）：hook 内部曾调用被断点的原函数
（`real_ticks` 调 `sys_mach_absolute_time`），断点二次触发递归进 hook，
同线程重入 `os_unfair_lock` → libplatform abort(259) → 主线程卡死 →
watchdog 0x8BADF00D 杀进程。

真实时钟源改为断点集合之外的符号：
- 单调钟：`mach_continuous_time`（独立符号、同一时基、commpage 直读）
- 墙钟：hook 生效前用裸 `syscall(SYS_clock_gettime, CLOCK_REALTIME)` 锚定一次，
  之后 = 锚点 + 连续钟换算，任何用户态 hook 都影响不了它
