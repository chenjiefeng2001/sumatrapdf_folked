# SumatraPDF (folked) — UI 现代化改造分支

基于 [SumatraPDF](https://www.sumatrapdfreader.org/) 的分支，核心目标：**在不引入任何重型框架（CEF/WebView2、Qt、Electron、WPF）的前提下，利用 Windows 原生 API 对 UI 进行渐进式现代化升级**，保留「极度轻量、秒开、低内存」的产品灵魂。

完整路线图与实现分析见：

- [UI 现代化路线图](docs/md/UI-modernization-roadmap.md) — 核心技术策略 + 分阶段实施状态
- [UI 实现综合分析报告](docs/UI_REPORT.md) — 架构扫描 / 性能对比 / 风险矩阵；其中的历史进度描述以本文和路线图为准
- [docs/reports/](docs/reports/) — 专项分析（标注子系统、D2D 设备世代、连续滚动拉伸、低端优化、多线程等）

## 当前实现状态（截至 2026-09）

### 渲染管线（基础设施已接入，迁移未完成）

- **Renderer 抽象**（`gui/win/Renderer.{h,cpp}`）：提供 GDI/D2D 后端接口；MSVC 构建在运行时探测 d2d1.dll，MinGW 构建使用 GDI 回退
- **DirectWrite 文字**（`gui/win/DWriteText.{h,cpp}`）：由 D2D 后端使用，尚未全面替换各控件的 GDI/GDI+ 文本绘制
- **当前接入范围**：ThumbnailPanel 使用统一 Renderer；TabsCtrl、Notifications、FrameRateWnd 等仍保留原有绘制路径，不能视为已迁移
- **通知弹性布局**：按父窗口 resize 水位线全量重排，长文本自动换行收缩不超 canvas（issue #2916），实测 964→592→964px 双向弹性

### 交互现代化（基本输入已接入，视觉过渡部分完成）

- **WM_POINTER 高精度输入**：`PointerInput.{h,cpp}` 动态加载 `EnableMouseInPointer`（Win8+，Win7 自动回落），滚轮逐像素、触控逐像素平移 + `PointerVelocityTracker` 速度采样
- **多点触控捏合缩放**：`GetPointerFramePoints` 取同帧双触点，缩放中心保持；平移基点改 per-window 消除多窗口串扰
- **惯性滚动**（`InertiaScrolling.{h,cpp}`）：指数衰减，边界处把动量交给 Overscroll
- **Overscroll**（`OverscrollEffect.{h,cpp}`）：已接入偏移和回弹路径，尚未覆盖完整文档橡皮筋效果
- **过渡动画**：当前仅侧边栏 slide 已接入；通知 slide-in、命令面板淡入和非连续翻页动画尚未接入
- 纯逻辑（EMA 权重 / 衰减 / clamp）抽为头文件内联函数，由 `src/base/tests/InputScrolling_ut.cpp` 无头验证

### 标注子系统重构

- 内容流与标注层分离：`GetOrBuildContentDisplayList` 常驻缓存，修改标注不再重跑页面内容
- 移动/resize 失效改「旧∪新」双矩形，消除大页面残留
- 命中测试空间索引 `AnnotHitIndex`（网格 + 惰性重建）替代 O(n) 线性扫描
- 分析报告：`docs/reports/annotation-system-analysis.md`

### 架构加固与构建

- MainWindow 生命周期状态机 + WndProc 守卫（修复会话恢复空指针崩溃）
- EXE 体积门禁（Release ≤15MB）、`cmd/build-installer.ts` 安装包构建
- RDP/VM 检测 + 低端降级决策；`GpuBackend.cpp` 和 `Win7Compat.cpp` 为 MSVC-only，MinGW 使用对应回退路径
- 编译期 FNV-1a 字符串哈希（`base/StrHash.h`）、命令面板相关性打分（头文件内联 + 单测）
- 无头单测体系：`bun cmd/run-unit-tests.ts -dbg`；集成测试 `tests/`（Bun TS，含 `-dbg-control` 管道驱动）

## 同步上游（2026-09-16）

已选择性同步上游 `sumatrapdfreader/sumatrapdf` 的安全修复（不整批合并，避免引入推广/遥测/品牌类改动）：

- **LIT 解析器整数溢出加固**（上游 10a83278a / 5d43b8cf9 / 820dea312）：`src/LitDoc.cpp` 全套越界检查
- **MOBI EXTH hdrLen / huffman 记录数 / 打印副本偏移越界**（上游 1ef900cba / 820dea312）：`src/MobiDoc.cpp`
- **PalmDB GetRecord 负索引防护**（上游 33a83925e）：`src/PalmDbReader.cpp`
- **ByteReader INT_MAX 偏移越界**（上游 4815430ca，fixes #6161）：新增 `ByteReader::CanRead()`，覆盖 EPS/WebP/TIFF/JXR 探测
- **TGA v2 footer strlen 越界 + 死代码修复**（上游 a7576b5e6）：`GuessFileType.cpp`、`TgaReader.cpp`（并额外修复扩展区偏移回绕）

同步原则：只取崩溃/安全修复与明确的性能改进；**不引入**推广、捐赠、作者展示、更新重定向、遥测上报等影响体验的变更；分支的 MSVC GPU 渲染后端、惯性滚动、窗口生命周期等定制全部保留，MinGW 构建继续使用 GDI 回退。上游的界面级大改动（Windows 11 打印对话框、滚动条重写等）按需单独评估。

## 构建

```sh
bun ./cmd/build.ts -debug   # 产出 out/dbg64/SumatraPDF.exe（测试请用此目标）
bun cmd/run-unit-tests.ts -dbg  # 无头单测
bun cmd/build-installer.ts  # 构建 Release 安装包 out/rel64/SumatraPDF-<ver>-64-install.exe
```

需 Visual Studio 命令行工具（cl.exe / msbuild.exe）；外部依赖见 `ext/`（`ext/mupdf` 为就地修改的 vendored 树，改动需同步记录补丁到 `ext/patches/`）。

## 文档

- 完整用户/开发文档：`docs/md/`（构建系统、命令行参数、高级设置、快捷键定制等）
- 上游版本历史：`docs/md/Version-history.md`

---

上游信息：SumatraPDF 是 Windows 平台多格式（PDF、EPUB、MOBI、CBZ、CBR、FB2、CHM、XPS、DjVu）阅读器，(A)GPLv3 许可（部分代码 BSD，见 AUTHORS）。

- [上游官网](https://www.sumatrapdfreader.org/free-pdf-reader)
- [手册](https://www.sumatrapdfreader.org/manual)
- [开发者信息](https://www.sumatrapdfreader.org/docs/Contribute-to-SumatraPDF)
