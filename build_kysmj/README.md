# LuckySpeeder build — branch kysmj, tag v0.1.0-kysmj

下载自 https://github.com/DemonswhisperYe/LuckySpeeder/releases/tag/v0.1.0-kysmj
（GitHub Actions macos-14，Xcode clang，-O3 -flto -fvisibility=hidden）

用途：注入 Chaos Zero Nightmare（SSR.app）。iPhone 用 `LuckySpeeder-arm64-apple-ios.dylib`
配 TrollFools 注入即可，其余三个目标是 macabi / xros / tvOS 变体。

本次改动：梅花模式（Club）改为统一虚拟时钟，同时 hook
clock_gettime + gettimeofday + mach_absolute_time（3 个硬件断点），
两条虚拟时间线（mono / rt）独立锚定，修掉共享累加器把墙钟推到几百年后
的问题，并补上 CLOCK_BOOTTIME(6) 的支持。
