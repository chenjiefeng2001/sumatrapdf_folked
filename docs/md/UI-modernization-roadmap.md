# SumatraPDF UI 现代化路线图：原生 Win32 的渐进式升级

> **核心理念：** 在不引入任何重型框架（CEF/WebView2、Qt、Electron、WPF）的前提下，利用 Windows 操作系统原生的 API 演进，对 SumatraPDF 的 UI 进行渐进式现代化升级。目标是在保留「极度轻量、秒开、低内存」灵魂的同时，实现接近现代阅读器的视觉与交互品质。

---

## 一、现状全景扫描

### 1.1 架构亮点（应保留并演进）

| 特性 | 现状 | 评价 |
|---|---|---|
| **轻量级控件框架** (`wingui/`) | 自建 Wnd 基类 + ILayout 布局接口 + ~20 自定义控件 | 独特高效，路线正确 |
| **约束布局系统** (Layout.h: VBox/HBox/Padding/Align) | 仿 Flutter flexbox 声明式布局 | 在纯 Win32 中属于前瞻设计 |
| **GPU 管道** (GpuBackend.h, D2DCompositor.h, wingui/Renderer.h) | 已引入 Direct2D 用于页面渲染 | ✅ Renderer 抽象已落地 (2026-08-02), 见策略一 |
| **覆盖层滚动条** (OverlayScrollbar) | Smart/Thick/Hidden 三模式 | 接近 macOS/Chrome 体验 |
| **SVG 图标系统** (SvgIcons.cpp) | 工具栏 SVG 渲染 | 方向正确 |
| **主题系统** (Theme.cpp) | 12+ 预定义主题 | 丰富可扩展 |
| **命令面板** (CommandPalette) | VS Code 风格多前缀 | PDF 阅读器中独一无二 |
| **MVC 架构** (DisplayModel + Canvas.cpp) | 数据视图清晰分离 | 稳健设计 |

### 1.2 当前问题清单

**渲染层（优先级：高）**

| 问题 | 位置 | 影响 | 状态 |
|---|---|---|---|
| 大量使用 GDI/GDI+ 绘制 | TabsCtrl.cpp, wingui/*, OverlayScrollbar.cpp | 高 DPI 锯齿、无硬件加速 | 🔶 Renderer 抽象已就绪, 控件迁移属阶段二 |
| GDI 字体无现代排版特性 | 各控件 HdcMeasureText/SelectObject | 无连字/可变字体/Emoji | ✅ DWriteText 缓存已落地 (2026-08-02), 控件接入属阶段二 |
| 图标位图/SVG 混用 | BuildStdToolbarImageList + GetSvgIcon | DPI 切换时模糊 | 🔶 DrawSvgIcon 接口已声明, 迁移属阶段二 |
| 无脏矩形增量刷新 | HwndScheduleRepaint 全区域刷新 | CPU 占用偏高 | ❌ 阶段五任务 (5.2) |

**交互层（优先级：中高）**

| 问题 | 位置 | 影响 | 状态 |
|---|---|---|---|
| 仅 WM_MOUSEWHEEL 累加滚动 | Canvas.cpp gDeltaPerLine | 触控板体验差 | ✅ 已解决 (WM_POINTERWHEEL 逐像素, 见策略二) |
| WM_GESTURE 触控老旧 | TouchState (MainWindow.h) | 无惯性/捏合 | ✅ 平移/惯性/捏合均已由 WM_POINTER 覆盖 (2026-08-03 捏合落地), WM_GESTURE 仅作历史保留 |
| 无 UI 过渡动画 | 侧边栏/工具栏/标签切换 | 交互生硬 | ✅ 已落地 (2026-08-03): 侧边栏 slide / 通知 slide-in / 命令面板淡入 |
| 无翻页动画 | 非连续模式 | 翻页跳跃 | ✅ 已落地 (2026-08-03): 非连续模式新页滑入动画 |
| 无 Overscroll | 滚到边界无反馈 | 缺少常见提示 | ✅ 已解决 (OverscrollEffect 已接线, 见策略二) |


---

## 二、四大核心技术策略

### 策略一：渲染管线升级 — 全面转向 Direct2D + DirectWrite

**总体架构：**
```
┌──────────────────────────────────────────┐
│          wingui / 自定义控件              │
├──────────────────────────────────────────┤
│    抽象绘制接口层 (abstract Renderer)      │
├─────────────────┬────────────────────────┤
│ID2D1RenderTarget│   GDI HDC (回退)       │
│ (硬件加速)       │   (WARP 软件回退)      │
├─────────────────┴────────────────────────┤
│ Windows 内置 DLL (d2d1.dll, dwrite.dll)   │
│ 零额外分发体积                             │
└──────────────────────────────────────────┘
```

**关键设计：** 抽象 RendererBackend 接口，实现 D2DRenderer 和 GDIRenderer。运行时检测 D2D1 工厂创建是否成功，据此选择后端。
> **✅ 已落地 (2026-08-02)**: `wingui/Renderer.{h,cpp}` — 抽象 `Renderer` 接口 (FillRect / FillRectF / DrawRect / DrawLine / DrawText(W) / DrawBitmap / PushClip / PopClip / SetTextAntialias / GetHDC / BeginPaint / EndPaint + 统一 `RgbaColor`), `GDIRenderer` 完整 GDI 实现, `D2DRenderer` 基于 `ID2D1DCRenderTarget` + `SetTextAntialiasMode`; `CreateRenderer()` 启动时经 `IsD2D1Available()` (d2d1.dll 动态加载探测) 选择后端, `gRenderer`/`gRendererKind` 全局追踪, D2D 不可用时自动回退 GDI。首个迁移控件 `wingui/FrameRateWnd.cpp` 已通过 gRenderer 绘制 (D2D + DirectWrite, 失败回退 GDI)。无头验证: `src/base/tests/Renderer_ut.cpp` (后端选择 / GDI 像素落盘 / D2D smoke / DWrite 缓存键纯逻辑)。

**现有基础设施：** GpuBackend.h 已具备 ID2D1Factory、ID2D1DCRenderTarget、ID2D1Bitmap。当前只用于页面内容渲染。

**控件绘制接口抽象：** `virtual void OnPaint(HDC hdc, PAINTSTRUCT* ps)` → 新 `RenderTarget` 接口含 FillRect/DrawText/DrawSvgIcon/DrawBitmap/GetHDC。
> ✅ 即 Renderer 接口 (见上); 控件逐个迁移属阶段二。

**字体渲染升级路线：** 短期 wingui 新增 DWriteRenderer(IDWriteTextFormat+IDWriteTextLayout) 获得 ClearType；中期引入富文本支持；长期 IDWriteFontFallback 构建全局回退链（CJK/Emoji）。
> **✅ 已落地 (短期, 2026-08-02)**: `wingui/DWriteText.{h,cpp}` — DWriteTextCache 按 HFONT (family/size/weight/style) 缓存 `IDWriteTextFormat`, DWriteTextRenderer 提供 CreateLayout / DrawLayout / MeasureLayout (IDWriteTextLayout), D2DRenderer::DrawText 经 DirectWrite 获得 ClearType; GDIRenderer 回退 ExtTextOutW (含 RTL 处理)。

**SVG 图标优化：** 通过 ID2D1SvgDocument(Win8.1+) 实时渲染，无需预生成位图。
> 🔶 **接口已就绪**: `Renderer::DrawSvgIcon` 已在抽象接口声明 (默认 no-op, 说明见头文件); 图标迁移仍走引擎管线 (mupdf SVG 栅格化 → HBITMAP → DrawBitmap), 见 Toolbar。ID2D1SvgDocument 实时渲染列为阶段二可选增强。

---

### 策略二：交互革命 — 高精度输入与微动画

**WM_POINTER 高精度触控：** 启用 EnableMouseInPointer(true)，Canvas WndProc 处理 WM_POINTERWHEEL（逐像素偏移）和 WM_POINTERUPDATE（触控平移+速度记录）。
> **✅ 已落地 (2026-08-02)**: `PointerInput.{h,cpp}` — EnablePointerInput() 动态加载 EnableMouseInPointer (Win8+ 才启用, Win7 自动回落); Canvas.cpp 的 WM_POINTERWHEEL 通过 GetPointerFrameInfo 动态解析真实 wheel delta (不再误判为 Ctrl 缩放), 触摸路径 WM_POINTERDOWN/UPDATE/UP 做逐像素平移 + PointerVelocityTracker 速度采样。
> **✅ 补充 (2026-08-03)**: 多点触控捏合缩放 — `GetPointerFramePoints()` 取同一 frame 内两触点屏幕坐标, 触点 ≥2 时以增量比例 `newZoom = startZoom * (dist/lastDist)` + `SetZoomVirtual(newZoom, fixPt)` 缩放中心保持; 平移基点改 per-window (`MainWindow.panLastX/Y`), 消除多窗口 static 串扰。Pen 与触摸共用平移/惯性路径, 无 DisplayModel 时鼠标模拟。

**惯性滚动（指数衰减）：** `velocity *= friction(0.95)` — 亚像素累积 → `MoveDocBy(ix, iy)` — 低于阈值时停止。
> **✅ 已落地 (2026-08-02)**: `InertiaScrolling.{h,cpp}` — InertiaScrollState 在 WM_POINTERUP/离屏时经 StartInertialScroll() (px/ms→px/tick 换算) + SetTimer 驱动, Tick 内按文档边界裁剪并在命中边界时把动量交给 Overscroll。

**Overscroll 弹性边界：** 惯性到边界触发弹性拉伸 + 回弹动画 (EaseOutQuad 300ms), 低端 Fast-Path 直接瞬移归零。
> **✅ 已落地 (2026-08-02)**: `OverscrollEffect.{h,cpp}` — ApplyDelta 累积弹性拉伸 (clamp maxStretch), kOverscrollTimerID 驱动 TickSpring 回弹, 绘制层视觉提示条。

> 全部纯逻辑 (EMA 权重 / 速度混合 / 单位换算 / 衰减 / clamp / 滚轮累积) 抽取为头文件内联纯函数, 由 `src/base/tests/InputScrolling_ut.cpp` (8 组用例) 在 test_util.exe 中无头验证。


---

## 三、分阶段实施路线图

### 阶段一：基础设施（2-3 个月）

| # | 任务 | 涉及文件 | 效果 | 状态 |
|---|---|---|---|---|
| 1.1 | 实现 RendererBackend 抽象接口 | wingui/Renderer.h/.cpp | D2D/GDI 双后端可切换 | ✅ 已落地 (2026-08-02, 见策略一) |
| 1.2 | 实现 D2DRenderer | 同上 + GpuBackend.cpp | 控件绘制走 D2D 管道 | ✅ 已落地 (2026-08-02, 见策略一) |
| 1.3 | 实现 DWriteTextRenderer | wingui/DWriteText.h/.cpp | 文字 ClearType 亚像素 | ✅ 已落地 (短期, 2026-08-02, 见策略一) |
| 1.4 | 全局动画管理器 | wingui/Animation.h/.cpp | 统一动画调度 | ✅ 已落地 (Animation.h, kAnimTimerID 已接线) |
| 1.5 | WM_POINTER 处理骨架 | Canvas.cpp | 接收高精度触控输入 | ✅ 已落地 (2026-08-02, 见策略二) |

### 阶段二：控件迁移（2-3 个月）

| # | 任务 | 文件 | 效果 | 状态 |
|---|---|---|---|---|
| 2.1 | TabsCtrl → D2D | wingui/TabsCtrl.cpp | 标签硬件加速 | ✅ 已迁移 (gRenderer + GDI fallback) |
| 2.2 | OverlayScrollbar → D2D | OverlayScrollbar.cpp | 半透明渐变 | ❌ 不适用 — 逐像素 premultiplied-alpha DIB + UpdateLayeredWindow 已是最优路径, 迁移无收益 |
| 2.3 | Toolbar → D2D | Toolbar.cpp | SVG 实时渲染无锯齿 | ❌ 不适用 — Win32 ReBar/Toolbar 由系统绘制 (NM_CUSTOMDRAW 仅调色); SVG 图标已由引擎管线栅格化 (DrawSvgIcon 声明为 no-op, 见策略一) |
| 2.4 | FindBar/Notifications → D2D | FindBar.cpp, Notifications.cpp | 一致高质量渲染 | 🟡 部分 — Notifications ✅ 主路径已迁移 (文本/关闭按钮/进度条 viaRenderer); FindBar 为系统控件 + NM_CUSTOMDRAW 背景, 不适用 |
| 2.5 | 侧边栏 → D2D | TableOfContents.cpp | 树视图文字清晰 | 🟡 部分 — TOC 为 Win32 TreeView 系统绘制不适用; **ThumbnailPanel (缩略图侧边栏) 已迁移 gRenderer (2026-08-03)** |

> 结论 (2026-08-03): 自定义绘制控件 (TabsCtrl / Notifications / FrameRateWnd / ThumbnailPanel) 已全部接入统一 Renderer 后端; 其余为系统原生控件 (Toolbar/FindBar/TreeView) 或像素级自绘 (OverlayScrollbar), 不适用 D2D 迁移。阶段二收官。

### 阶段三：交互现代化（2-3 个月）

| # | 任务 | 文件 | 效果 | 状态 |
|---|---|---|---|---|
| 3.1 | WM_POINTER 完整滚动 | Canvas.cpp | 逐像素精确 | ✅ 已落地 (2026-08-02, 见策略二) |
| 3.2 | 惯性滚动算法 | 新建 + Canvas.cpp | 平滑减速停止 | ✅ 已落地 (2026-08-02, 见策略二) |
| 3.3 | Overscroll 弹性反馈 | 新建 + Canvas.cpp | 拉伸-弹回 | ✅ 已落地 (2026-08-02, 见策略二) |
| 3.4 | UI 过渡动画 | MainWindow.cpp + 动画框架 | 200ms 平滑过渡 | ✅ 已落地 (2026-08-03): 侧边栏 slide / 通知 slide-in / 命令面板淡入 |
| 3.5 | 翻页动画 | Canvas.cpp | 新页从右侧滑入 | ✅ 已落地 (2026-08-03): 非连续模式新页滑入动画 |

> 剩余缺口 (2026-08-03 已全部关闭): ~~多点触控/捏合~~ (✅ WM_POINTER 捏合缩放已实现, 见策略二)、~~Pen 完整路径~~ (✅ 平移/惯性共用 + 无 DM 鼠标模拟)、~~OnPointerMessage 内 static 平移基点的多窗口串扰~~ (✅ 平移基点改 per-window 字段 panLastX/Y)。

### 阶段四：视觉现代化（1-2 个月）

| # | 任务 | 文件 | 效果 |
|---|---|---|---|
| 4.1 | Win11 Mica 云母 | SumatraStartup.cpp/MainWindow.cpp | 桌面壁纸透出质感 |
| 4.2 | 无边框+圆角 | SumatraPDF.cpp (WndProcSumatraFrame) | 现代沉浸式外观 |
| 4.3 | 自定义标题栏完善 | MainWindow.cpp | 完整拖拽/按钮/菜单 |
| 4.4 | 缩略图侧边栏 | ThumbnailPanel.h/.cpp | 页面缩略图预览 |

### 阶段五：架构完善（2-3 个月）

| # | 任务 | 文件 | 效果 |
|---|---|---|---|
| 5.1 | 全局布局树移植 | MainWindow.cpp | 声明式替代手工 MoveWindow |
| 5.2 | 脏矩形增量刷新 | Canvas.cpp + 各控件 | 减少无效重绘 |
| 5.3 | 响应式断点 | MainWindow.cpp | 窄窗口自动折叠 |
| 5.4 | 工具栏溢出菜单 | Toolbar.cpp | 窄窗口自动纳入...菜单 |

---

## 四、性能与体积影响评估

| 组件 | 来源 | 额外体积 | 落地状态 |
|---|---|---|---|
| d2d1.dll / dwrite.dll / dwmapi.dll | Windows 内置 | 0 KB | 系统提供 |
| D2D 封装代码 ~1500 行 | 本项目新增 | ~50 KB | ✅ GpuBackend (页面渲染) |
| 动画框架 ~500 行 | 本项目新增 | ~15 KB | ✅ Animation.h (已接线 kAnimTimerID) |
| WM_POINTER + 惯性 + Overscroll ~430 行 | 本项目新增 | ~15 KB | ✅ PointerInput + InertiaScrolling + OverscrollEffect (2026-08-02) |
| **总计** | | **~80 KB** | |

**内存与 CPU：** 空白窗口 ~18→20MB；PDF 内存不变(~45MB)；闲置 CPU 1-3%→<0.5%；翻页 8-15%→3-8%。

**兼容性矩阵：**

| Windows | D2D | Mica | 圆角 | WM_POINTER |
|---|---|---|---|---|
| Win7 | ✅ WARP | ❌ | ❌ | ❌ |
| Win8/8.1 | ✅ 硬件 | ❌ | ❌ | ✅ |
| Win10 1607+ | ✅ 硬件 | ❌ | ❌ | ✅ |
| Win11 21H2+ | ✅ 硬件 | ✅ | ✅ | ✅ |
| Win11 22H2+ | ✅ 硬件 | ✅ Mica Alt | ✅ | ✅ |
| Wine | ⚠️ GDI | ❌ | ❌ | ❌ |

---

## 五、风险评估与缓解措施

| 风险 | 缓解策略 |
|---|---|
| D2DRenderer 未覆盖绘制路径 | GetHDC() 回退到 GDI；gUseD2DRenderer 全局开关；保留 GDI fallback |
| 低端设备掉帧 | 自适应帧率(超20ms跳过)；低端降级；动画100-300ms |
| 多 DPI 切换 | WM_DPICHANGED 重建渲染目标；ID2D1DeviceContext；DpiScale 保持不动 |
| 与 darkmodelib 冲突 | 已迁移控件取消子类化；未迁移继续使用；ThemeColorizeControls 通过 D2D 参数生效 |
| 破坏现有功能 | 每个阶段保留 GDI fallback；CI 覆盖两路径；-for-testing 标志 A/B 比较 |

---

## 六、现有基础设施可复用清单

| 资源 | 位置 | 复用方式 |
|---|---|---|
| GpuBackend (ID2D1Factory) | src/GpuBackend.h/.cpp | D2DRenderer 基础 |
| D2DCompositor | src/D2DCompositor.h | 参考互操作方法 |
| IsRunningOnWine() | src/base/Win.h | 检测禁用 D2D/Mica |
| GetWindowsBuildNumber() | src/base/Win.h | Win11 版本检测 |
| WinDynCalls.h | src/base/WinDynCalls.h | DWM API 动态加载 |
| SvgIcons.cpp | src/SvgIcons.cpp | SVG 源数据存在 |
| HwndScheduleRepaint | src/base/Win.h | 与动画框架集成 |
| HwndIsRtl / IsUIRtl | src/base/Win.h / Translations.cpp | RTL 支持 |
| DarkModeSubclass.h | ext/darkmodelib | 取消已迁移控件的子类化 |
| Layout.h (VBox/HBox) | src/wingui/Layout.h | 直接用于全局布局树 |

---

## 七、总结

### 改造三大原则

1. **零额外运行时依赖** — 所有 API 都是 Windows 内置，不增加分发体积
2. **渐进式迁移** — 每个控件留有 GDI 回退，不搞「大爆炸」重构
3. **保留核心 DNA** — 极简主义、极速启动、低内存占用永远优先

### 最终效果预测

| 指标 | 当前 | 改造后 |
|---|---|---|
| 二进制体积 | ~5.5 MB | ~6.2 MB (+12%) |
| 空白窗口内存 | ~18 MB | ~20 MB (+11%) |
| 典型 PDF 阅读 | ~45 MB | ~45 MB (不变) |
| Win11 外观 | 标准 Win32 | Mica+圆角+无边框 |
| 触控板滚动 | 分段跳跃 | 逐像素+惯性 |
| UI 动画 | 无 | 200ms 缓动过渡 |
| 闲置 CPU | 1-3% | <0.5% |
| Win7 兼容 | ✅ | ✅ 自动回退 GDI |


---

## 八、落地进度记录 (2026-08-02)

以下为与 `docs/UI_REPORT.md`「风险与规避策略矩阵」对应的实际落地项, 均已通过构建与无头测试:

| 落地项 | 涉及文件 | 验证 |
|---|---|---|
| MainWindow 生命周期状态机 + WndProc 守卫 | src/WindowLifecycle.h, src/MainWindow.{h,cpp}, src/SumatraPDF.cpp, src/SumatraStartup.cpp | src/base/tests/WindowLifecycle_ut.cpp + tests/ad-hoc-startup-smoke.ts |
| EXE 体积 CI 门禁 (Release ≤15MB) | cmd/exe-size-limit.ts + cmd/build.ts | tests/ad-hoc-exe-size.ts |
| RDP/VM 检测 + 极简 GDI 降级 (纯函数决策) | src/HardwareProfile.{h,cpp} | src/base/tests/HardwareProfile_ut.cpp |
| Win7 兼容 shim (ETW 导入 stub) | src/Win7Compat.cpp | Debug x64 Rebuild 0 错 |
| 编译期字符串哈希 (FNV-1a) | src/base/StrHash.h | src/base/tests/StrHash_ut.cpp |
| 命令面板: 结果计数 + ×清除按钮 + 分类图标 | CommandPalette*.{h,cpp} | tests/ad-hoc-command-palette-enhancements.ts |
| D2D 源矩形修复 (连续滚动压缩) + 标注渲染 atomic | src/RenderCache.cpp, src/EngineMupdf.{h,cpp} | tests/issue-page-geometry-scroll.ts |
| **策略一: 渲染管线抽象 (Renderer + GDI/D2D 双后端 + DirectWrite)** | src/wingui/Renderer.{h,cpp}, src/wingui/DWriteText.{h,cpp}, src/wingui/FrameRateWnd.cpp, src/SumatraStartup.cpp, premake5.files.lua | src/base/tests/Renderer_ut.cpp (test_util.exe 无头) |
| 命令面板: 结果计数布局刷新 (LayoutToSize 整布局重排) + SizeToIdealSize 保位修复 (2026-08-03) | src/CommandPaletteFilter.cpp, src/wingui/Wnd.cpp | 见 UI_REPORT.md §7.6/§7.7 |
| **阶段二: 控件迁移收官** — ThumbnailPanel 迁移 gRenderer (D2D + DirectWrite, GDI 回退); TabsCtrl/Notifications 主路径已迁移; 系统控件 (Toolbar/FindBar/TreeView/OverlayScrollbar) 不适用结论 (2026-08-03) | src/ThumbnailPanel.cpp, src/wingui/TabsCtrl.cpp, src/Notifications.cpp | Debug x64 构建 + 单测通过 + -view thumbs 冒烟 |
| **阶段三: 交互收官** — 多点触控捏合缩放 (GetPointerFramePoints + SetZoomVirtual fixPt) + 平移基点 per-window (static 串扰修复) (2026-08-03) | src/PointerInput.{h,cpp}, src/Canvas.cpp, src/MainWindow.h | Debug x64 构建 0 错 + unit-tests 通过 |
| **阶段三: UI 过渡动画 + 翻页动画** — 侧边栏 slide / 通知 slide-in / 命令面板淡入 + 非连续模式翻页动画 (2026-08-03) | src/MainWindow.cpp, src/Notifications.cpp, src/CommandPalette.cpp, src/Canvas.cpp | tests/ad-hoc-command-palette-enhancements.ts 通过 |
| **通知弹性布局 (真正的弹性宽度)** — RelayoutNotifications 按 lastParentDx 水位线检测父窗口 resize + keepWidth=false 全量重排: 变窄时长文本换行收缩不超 canvas (issue #2916), 变宽恢复完整宽度; 修复 keepWidth=true 消息更新 (ZoomChanged 重写页面信息) 污染水位线导致重排被跳过的缺陷 (2026-08-03) | src/Notifications.cpp | tests/ad-hoc-notif-elastic.ts (964→592→964px 双向弹性实测) |

> 注: 实际 EXE 大小 12.6 MB (见 UI_REPORT.md §1), 高于本文档第 4/7 节的 ~6.2 MB 预估 — 差异主要来自 libmupdf 渲染库的静态链接; 15 MB 门禁以实际产物为准。



