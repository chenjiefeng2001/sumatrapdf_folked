# SumatraPDF (folked) — UI 现代化改造分支

基于 [SumatraPDF](https://www.sumatrapdfreader.org/) 的分支，核心目标：**在不引入任何重型框架（CEF/WebView2、Qt、Electron、WPF）的前提下，利用 Windows 原生 API 对 UI 进行渐进式现代化升级**，保留「极度轻量、秒开、低内存」的产品灵魂。

完整路线图与实现分析见：

- [UI 现代化路线图](docs/md/UI-modernization-roadmap.md) — 四大技术策略 + 分阶段实施状态
- [UI 实现综合分析报告](docs/UI_REPORT.md) — 架构扫描 / 性能对比 / 进度总览 / 风险矩阵
- [docs/reports/](docs/reports/) — 专项分析（标注子系统、D2D 设备世代、连续滚动拉伸、低端优化、多线程等）

## 已落地进度（截至 2026-08）

### 渲染管线（策略一，已收官）

- **Renderer 抽象**（`wingui/Renderer.{h,cpp}`）：统一的 `FillRect/DrawText/DrawBitmap/Clip` 接口，`GDIRenderer` + `D2DRenderer`（`ID2D1DCRenderTarget`）双后端，启动时探测 d2d1.dll 自动选择、D2D 不可用时回退 GDI
- **DirectWrite 文字**（`wingui/DWriteText.{h,cpp}`）：按 HFONT 缓存 `IDWriteTextFormat`，ClearType 亚像素渲染，GDI 回退 `ExtTextOutW`（含 RTL）
- **控件迁移收官**：TabsCtrl / Notifications / FrameRateWnd / ThumbnailPanel 已接入统一 Renderer；Toolbar/FindBar/TreeView 为系统控件、OverlayScrollbar 为逐像素自绘，判定不适用迁移
- **通知弹性布局**：按父窗口 resize 水位线全量重排，长文本自动换行收缩不超 canvas（issue #2916），实测 964→592→964px 双向弹性

### 交互现代化（策略二/三，已收官）

- **WM_POINTER 高精度输入**：`PointerInput.{h,cpp}` 动态加载 `EnableMouseInPointer`（Win8+，Win7 自动回落），滚轮逐像素、触控逐像素平移 + `PointerVelocityTracker` 速度采样
- **多点触控捏合缩放**：`GetPointerFramePoints` 取同帧双触点，缩放中心保持；平移基点改 per-window 消除多窗口串扰
- **惯性滚动**（`InertiaScrolling.{h,cpp}`）：指数衰减，边界处把动量交给 Overscroll
- **Overscroll 弹性回弹**（`OverscrollEffect.{h,cpp}`）：弹性拉伸 + EaseOutQuad 回弹
- **过渡动画**：侧边栏 slide / 通知 slide-in / 命令面板淡入 + 非连续模式翻页滑入动画
- 纯逻辑（EMA 权重 / 衰减 / clamp）抽为头文件内联函数，由 `src/base/tests/InputScrolling_ut.cpp` 无头验证

### 标注子系统重构

- 内容流与标注层分离：`GetOrBuildContentDisplayList` 常驻缓存，修改标注不再重跑页面内容
- 移动/resize 失效改「旧∪新」双矩形，消除大页面残留
- 命中测试空间索引 `AnnotHitIndex`（网格 + 惰性重建）替代 O(n) 线性扫描
- 分析报告：`docs/reports/annotation-system-analysis.md`

### 架构加固与构建

- MainWindow 生命周期状态机 + WndProc 守卫（修复会话恢复空指针崩溃）
- EXE 体积门禁（Release ≤15MB）、`cmd/build-installer.ts` 安装包构建
- RDP/VM 检测 + 低端 GDI 降级、Win7 兼容 shim、SEH-safe COM 释放（ComSafe.h）
- 编译期 FNV-1a 字符串哈希（`base/StrHash.h`）、命令面板相关性打分（头文件内联 + 单测）
- 无头单测体系：`bun cmd/run-unit-tests.ts -dbg`；集成测试 `tests/`（Bun TS，含 `-dbg-control` 管道驱动）

## 构建

```sh
bun ./cmd/build.ts          # 产出 out/dbg64/SumatraPDF-dll.exe（测试请用此目标）
bun cmd/run-unit-tests.ts   # 无头单测
```

需 Visual Studio 命令行工具（cl.exe / msbuild.exe）在 PATH 中；外部依赖见 `ext/` 与 `mupdf/`。

---

上游信息：SumatraPDF 是 Windows 平台多格式（PDF、EPUB、MOBI、CBZ、CBR、FB2、CHM、XPS、DjVu）阅读器，(A)GPLv3 许可（部分代码 BSD，见 AUTHORS）。

- [上游官网](https://www.sumatrapdfreader.org/free-pdf-reader)
- [手册](https://www.sumatrapdfreader.org/manual)
- [开发者信息](https://www.sumatrapdfreader.org/docs/Contribute-to-SumatraPDF)
