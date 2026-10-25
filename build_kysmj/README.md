# LuckySpeeder build — branch kysmj, tag v0.1.3-kysmj

CI run 40188628955（success）。iPhone 注入用 `LuckySpeeder-arm64-apple-ios.dylib`（TrollFools）。

## v0.1.3 修复内容（日志 191544「还是卡死」）

第一性原理定位：本次进程里跑的确实是 v0.1.2（UUID 9d2c2b0a 匹配），递归锁
问题已不存在（无 abort）。新问题是**硬件断点异常风暴饿死主线程**：

- `mach_absolute_time` 是全进程最热的调用之一（UIKit 渲染/GCD timer/objc 每帧上千次）
- hwbp 每次命中 = 一次 Mach 异常往返，由**单线程** `mach_msg_server` 串行处理
- 到达率 > 处理率 → 异常队列堆积 → 所有调用者阻塞 → 主线程 10 秒内推不完一帧
- 日志佐证：总 CPU 14%（system+handler 自旋），应用自身 CPU 仅 1%，
  handler 线程（thread[31]）满转，主线程停在 CFRunLoopServiceMachPort

修复：彻底放弃硬件断点，三个符号（clock_gettime / gettimeofday /
mach_absolute_time）全部改 fishhook（GOT 重绑定）——单次调用零开销，
覆盖面相同（主二进制外部调用全走 GOT stub）。统一虚拟时钟不变，
真实时钟源仍是断点外的 mach_continuous_time + 裸 syscall 锚定。

## 历史版本

- v0.1.2：修复递归锁崩溃（1806 日志）——hook 内曾回调被断点的原函数
- v0.1.1：release 产物命名修正
- v0.1.0：统一虚拟时钟（修共享累加器 + 全时钟源一致）
