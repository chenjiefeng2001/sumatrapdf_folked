# SumatraPDF UI 现代化路线图：原生 Win32 的渐进式升级

> **核心理念：** 在不引入任何重型框架（CEF/WebView2、Qt、Electron、WPF）的前提下，利用 Windows 操作系统原生的 API 演进，对 SumatraPDF 的 UI 进行渐进式现代化升级。目标是在保留「极度轻量、秒开、低内存」灵魂的同时，实现接近现代阅读器的视觉与交互品质。

---

## 一、现状全景扫描

### 1.1 架构亮点（应保留并演进）

| 特性 | 现状 | 评价 |
|---|---|---|
| **轻量级控件框架** (`gui/win/`) | 自建 Wnd 基类 + ILayout 布局接口 + ~20 自定义控件 | 独特高效，路线正确 |
| **约束布局系统** (Layout.h: VBox/HBox/Padding/Align) | 仿 Flutter flexbox 声明式布局 | 在纯 Win32 中属于前瞻设计 |
| **GPU 管道** (GpuBackend.h, gui/win/Renderer.h) | MSVC 下可选 Direct2D；MinGW 使用 GDI | 🔶 基础设施存在，控件迁移未完成 |
| **覆盖层滚动条** (OverlayScrollbar) | Smart/Thick/Hidden 三模式 | 接近 macOS/Chrome 体验 |
| **SVG 图标系统** (SvgIcons.cpp) | 工具栏 SVG 渲染 | 方向正确 |
| **主题系统** (Theme.cpp) | 12+ 预定义主题 | 丰富可扩展 |
| **命令面板** (CommandPalette) | VS Code 风格多前缀 | PDF 阅读器中独一无二 |
| **MVC 架构** (DisplayModel + Canvas.cpp) | 数据视图清晰分离 | 稳健设计 |

### 1.2 当前问题清单

**渲染层（优先级：高）**

| 问题 | 位置 | 影响 | 状态 |
|---|---|---|---|
| 大量使用 GDI/GDI+ 绘制 | TabsCtrl.cpp, gui/win/*, OverlayScrollbar.cpp | 高 DPI 锯齿、无硬件加速 | 🔶 Renderer 抽象已存在，仅部分自定义控件接入 |
| GDI 字体无现代排版特性 | 各控件 HdcMeasureText/SelectObject | 无连字/可变字体/Emoji | 🔶 DWriteText 已由 D2D 后端使用，尚未全面接入 |
| 图标位图/SVG 混用 | BuildStdToolbarImageList + GetSvgIcon | DPI 切换时模糊 | 🔶 DrawSvgIcon 仍为 no-op，图标继续由引擎栅格化 |
| 无脏矩形增量刷新 | HwndScheduleRepaint 全区域刷新 | CPU 占用偏高 | ❌ 阶段五任务 (5.2) |

**交互层（优先级：中高）**

| 问题 | 位置 | 影响 | 状态 |
|---|---|---|---|
| 仅 WM_MOUSEWHEEL 累加滚动 | Canvas.cpp gDeltaPerLine | 触控板体验差 | ✅ 已解决 (WM_POINTERWHEEL 逐像素, 见策略二) |
| WM_GESTURE 触控老旧 | TouchState (MainWindow.h) | 无惯性/捏合 | ✅ 平移/惯性/捏合均已由 WM_POINTER 覆盖 (2026-08-03 捏合落地), WM_GESTURE 仅作历史保留 |
| 无 UI 过渡动画 | 侧边栏/工具栏/标签切换 | 交互生硬 | 🟡 仅侧边栏 slide 已接入，通知/命令面板动画尚未接入 |
| 无翻页动画 | 非连续模式 | 翻页跳跃 | ❌ 未接入 |
| 无 Overscroll | 滚到边界无反馈 | 缺少常见提示 | 🟡 偏移与回弹路径已接入，完整橡皮筋效果未完成 |


---

## 二、核心技术策略

### 策略一：渲染管线升级 — 全面转向 Direct2D + DirectWrite

**总体架构：**
```
┌──────────────────────────────────────────┐
│          gui/win / 自定义控件            │
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

**关键设计：** 抽象 Renderer 接口，实现 D2DRenderer 和 GDIRenderer。MSVC 构建运行时检测 D2D1 工厂，MinGW 构建固定使用 GDI 回退。
> **✅ 基础设施已落地**: `gui/win/Renderer.{h,cpp}` 提供统一绘制接口和 GDI 后端；MSVC 下可选 D2D 后端，DWrite 文字路径随之启用。当前 `ThumbnailPanel` 已接入该接口，其他控件尚未完成迁移。

**现有基础设施：** `GpuBackend.{h,cpp}` 的页面/覆盖层路径仅在 MSVC 构建编译；MinGW 不包含该实现。

**控件绘制接口抽象：** `virtual void OnPaint(HDC hdc, PAINTSTRUCT* ps)` → `Renderer` 接口含 FillRect/DrawText/DrawSvgIcon/DrawBitmap/GetHDC。
> ✅ 即 Renderer 接口 (见上); 控件逐个迁移属阶段二。

**字体渲染升级路线：** D2D 后端已接入 `DWriteText`；GDI 后端和其他控件仍使用原有文本路径。富文本、全局字体回退链尚未接入。
> **✅ 基础设施已落地**: `gui/win/DWriteText.{h,cpp}` 提供 `IDWriteTextFormat` 缓存、布局和测量；仅 D2D 后端使用，MinGW 保持 GDI 文本路径。

**SVG 图标优化：** 通过 ID2D1SvgDocument(Win8.1+) 实时渲染，无需预生成位图。
> 🔶 **接口已就绪**: `Renderer::DrawSvgIcon` 已在抽象接口声明 (默认 no-op, 说明见头文件); 图标迁移仍走引擎管线 (mupdf SVG 栅格化 → HBITMAP → DrawBitmap), 见 Toolbar。ID2D1SvgDocument 实时渲染列为阶段二可选增强。

---

### 策略二：交互革命 — 高精度输入与微动画

**WM_POINTER 高精度触控：** 启用 EnableMouseInPointer(true)，Canvas WndProc 处理 WM_POINTERWHEEL（逐像素偏移）和 WM_POINTERUPDATE（触控平移+速度记录）。
> **✅ 已落地 (2026-08-02)**: `PointerInput.{h,cpp}` — EnablePointerInput() 动态加载 EnableMouseInPointer (Win8+ 才启用, Win7 自动回落); Canvas.cpp 的 WM_POINTERWHEEL 通过 GetPointerFrameInfo 动态解析真实 wheel delta (不再误判为 Ctrl 缩放), 触摸路径 WM_POINTERDOWN/UPDATE/UP 做逐像素平移 + PointerVelocityTracker 速度采样。
> **✅ 补充 (2026-08-03)**: 多点触控捏合缩放 — `GetPointerFramePoints()` 取同一 frame 内两触点屏幕坐标, 触点 ≥2 时以增量比例 `newZoom = startZoom * (dist/lastDist)` + `SetZoomVirtual(newZoom, fixPt)` 缩放中心保持; 平移基点改 per-window (`MainWindow.panLastX/Y`), 消除多窗口 static 串扰。Pen 与触摸共用平移/惯性路径, 无 DisplayModel 时鼠标模拟。

**惯性滚动（指数衰减）：** `velocity *= friction(0.95)` — 亚像素累积 → `MoveDocBy(ix, iy)` — 低于阈值时停止。
> **✅ 基本路径已落地**: `InertiaScrolling.{h,cpp}` 由 Canvas 的指针释放路径启动，并在边界处把剩余动量交给 Overscroll。

**Overscroll 弹性边界：** 当前接入偏移和回弹状态；完整的文档内容橡皮筋拉伸仍未完成。
> **🟡 部分落地**: `OverscrollEffect.{h,cpp}` 提供 `ApplyDelta`、限幅和定时回弹，视觉效果仍待扩展。

> 全部纯逻辑 (EMA 权重 / 速度混合 / 单位换算 / 衰减 / clamp / 滚轮累积) 抽取为头文件内联纯函数, 由 `src/base/tests/InputScrolling_ut.cpp` (8 组用例) 在 test_util.exe 中无头验证。


---

## 三、分阶段实施路线图

### 阶段一：基础设施（2-3 个月）

| # | 任务 | 涉及文件 | 效果 | 状态 |
|---|---|---|---|---|
| 1.1 | 实现 Renderer 抽象接口 | gui/win/Renderer.h/.cpp | D2D/GDI 双后端可切换 | ✅ 基础设施已落地 |
| 1.2 | 实现 D2DRenderer | 同上 + GpuBackend.cpp | 控件绘制走 D2D 管道 | ✅ MSVC-only；MinGW 使用 GDI |
| 1.3 | 实现 DWriteTextRenderer | gui/win/DWriteText.h/.cpp | 文字 ClearType 亚像素 | ✅ D2D 后端已接入，尚未全面迁移 |
| 1.4 | 全局动画管理器 | gui/win/Animation.h/.cpp | 统一动画调度 | ✅ 基础设施已落地，当前仅侧边栏使用 |
| 1.5 | WM_POINTER 处理骨架 | Canvas.cpp | 接收高精度触控输入 | ✅ 基本路径已落地 |

### 阶段二：控件迁移（2-3 个月）

| # | 任务 | 文件 | 效果 | 状态 |
|---|---|---|---|---|
| 2.1 | TabsCtrl → D2D | gui/win/TabsCtrl.cpp | 标签硬件加速 | 🔶 尚未迁移，仍使用现有绘制路径 |
| 2.2 | OverlayScrollbar → D2D | OverlayScrollbar.cpp | 半透明渐变 | ❌ 不适用 — 逐像素 premultiplied-alpha DIB + UpdateLayeredWindow 已是最优路径 |
| 2.3 | Toolbar → D2D | Toolbar.cpp | SVG 实时渲染无锯齿 | ❌ 不适用 — Win32 ReBar/Toolbar 由系统绘制，SVG 继续由引擎管线栅格化 |
| 2.4 | FindBar/Notifications → D2D | FindBar.cpp, Notifications.cpp | 一致高质量渲染 | 🔶 尚未迁移；两者仍使用现有控件/自绘路径 |
| 2.5 | 侧边栏 → D2D | TableOfContents.cpp | 树视图文字清晰 | 🟡 部分 — TOC 为 Win32 TreeView；ThumbnailPanel 已接入 gRenderer |

> 阶段二尚未收官：当前仅 ThumbnailPanel 接入统一 Renderer；系统控件和其余自绘控件仍需单独评估，不能把“未迁移”写成已完成。

### 阶段三：交互现代化（2-3 个月）

| # | 任务 | 文件 | 效果 | 状态 |
|---|---|---|---|---|
| 3.1 | WM_POINTER 完整滚动 | Canvas.cpp | 逐像素精确 | ✅ 基本路径已落地 |
| 3.2 | 惯性滚动算法 | 新建 + Canvas.cpp | 平滑减速停止 | ✅ 基本路径已落地 |
| 3.3 | Overscroll 弹性反馈 | 新建 + Canvas.cpp | 拉伸-弹回 | 🟡 偏移/回弹已接入，完整效果未完成 |
| 3.4 | UI 过渡动画 | MainWindow.cpp + 动画框架 | 200ms 平滑过渡 | 🟡 仅侧边栏 slide 已接入 |
| 3.5 | 翻页动画 | Canvas.cpp | 新页从右侧滑入 | ❌ 未接入 |

> 仍待补齐：通知 slide-in、命令面板淡入、非连续翻页动画，以及更完整的 Overscroll 绘制。WM_POINTER 平移/捏合/惯性路径已接入，不应再列为未完成。

### 阶段四：视觉现代化（1-2 个月）

| # | 任务 | 文件 | 效果 | 状态 |
|---|---|---|---|---|
| 4.1 | Win11 Mica 云母 | SumatraStartup.cpp/MainWindow.cpp | 桌面壁纸透出质感 | ❌ 未接入；当前只有降级决策接口 |
| 4.2 | 无边框+圆角 | SumatraPDF.cpp (WndProcSumatraFrame) | 现代沉浸式外观 | 🟡 系统圆角属性已设置，无边框沉浸未完成 |
| 4.3 | 自定义标题栏完善 | MainWindow.cpp | 完整拖拽/按钮/菜单 | 🟡 现有 caption/layout 仍需继续完善 |
| 4.4 | 缩略图侧边栏 | ThumbnailPanel.h/.cpp | 页面缩略图预览 | ✅ 已有实现，绘制后端范围见阶段二 |

### 阶段五：架构完善（2-3 个月）

| # | 任务 | 文件 | 效果 | 状态 |
|---|---|---|---|---|
| 5.1 | 全局布局树移植 | MainWindow.cpp | 声明式替代手工 MoveWindow | 🟡 frame 局部使用布局树，尚未覆盖所有窗口 |
| 5.2 | 脏矩形增量刷新 | Canvas.cpp + 各控件 | 减少无效重绘 | ❌ 未接入 |
| 5.3 | 响应式断点 | MainWindow.cpp | 窄窗口自动折叠 | ❌ 未接入 |
| 5.4 | 工具栏溢出菜单 | Toolbar.cpp | 窄窗口自动纳入...菜单 | ❌ 未接入 |

---

## 四、性能与体积影响评估

| 组件 | 来源 | 额外体积 | 落地状态 |
|---|---|---|---|
| d2d1.dll / dwrite.dll / dwmapi.dll | Windows 内置 | 0 KB | 系统提供 |
| D2D 封装代码 ~1500 行 | 本项目新增 | ~50 KB | 🟡 MSVC-only；MinGW 不编译 GpuBackend |
| 动画框架 ~500 行 | 本项目新增 | ~15 KB | ✅ 基础设施已接入，当前仅侧边栏使用 |
| WM_POINTER + 惯性 + Overscroll ~430 行 | 本项目新增 | ~15 KB | ✅ 基本输入路径已接入，完整视觉效果待补 |
| **总计** | | **~80 KB（估算）** | 不是当前构建的承诺值 |

**内存与 CPU：** 上述数字是路线图估算，当前工作树没有重新测量；发布前应以实际构建产物和基准测试为准。

**兼容性矩阵：**

| 构建/系统 | Renderer | Mica | 系统圆角 | WM_POINTER |
|---|---|---|---|---|
| MSVC / Win7 | D2D 运行时探测，GDI 回退 | ❌ 未接入 | 🟡 由系统决定 | ❌ |
| MSVC / Win8/8.1 | D2D 可用时启用，否则 GDI | ❌ 未接入 | 🟡 由系统决定 | ✅ |
| MSVC / Win10+ | D2D 可用时启用，否则 GDI | ❌ 未接入 | 🟡 Win11 可用 | ✅ |
| MinGW | GDI | ❌ 未接入 | 🟡 由系统决定 | ✅ Win8+ |
| Wine | GDI | ❌ 未接入 | ❌ 不保证 | 部分 API 可用 |

---

## 五、风险评估与缓解措施

| 风险 | 缓解策略 |
|---|---|
| D2DRenderer 未覆盖绘制路径 | 保留 GDI fallback；通过 `gRendererKind` 记录当前后端；MinGW 固定使用 GDI |
| 低端设备掉帧 | 自适应帧率(超20ms跳过)；低端降级；动画100-300ms |
| 多 DPI 切换 | WM_DPICHANGED 重建渲染目标；ID2D1DeviceContext；DpiScale 保持不动 |
| 与 darkmodelib 冲突 | 已迁移控件取消子类化；未迁移继续使用；ThemeColorizeControls 通过 D2D 参数生效 |
| 破坏现有功能 | 每个阶段保留 GDI fallback；CI 覆盖两路径；-for-testing 标志 A/B 比较 |

---

## 六、现有基础设施可复用清单

| 资源 | 位置 | 复用方式 |
|---|---|---|
| GpuBackend (ID2D1Factory) | src/GpuBackend.h/.cpp | MSVC 页面/覆盖层路径；MinGW 不编译 |
| Renderer | src/gui/win/Renderer.h/.cpp | GDI 回退与可选 D2D 后端 |
| IsRunningOnWine() | src/base/Win.h | 检测 D2D/DWM 能力差异 |
| GetWindowsBuildNumber() | src/base/Win.h | Win11 版本检测 |
| WinDynCalls.h | src/base/WinDynCalls.h | DWM API 动态加载 |
| SvgIcons.cpp | src/SvgIcons.cpp | SVG 源数据存在 |
| HwndScheduleRepaint | src/base/Win.h | 与动画框架集成 |
| HwndIsRtl / IsUIRtl | src/base/Win.h / Translations.cpp | RTL 支持 |
| DarkModeSubclass.h | ext/darkmodelib | 取消已迁移控件的子类化 |
| Layout.h (VBox/HBox) | src/gui/win/Layout.h | 直接用于全局布局树 |

---

## 七、总结

### 改造三大原则

1. **零额外运行时依赖** — 所有 API 都是 Windows 内置，不增加分发体积
2. **渐进式迁移** — 每个控件留有 GDI 回退，不搞「大爆炸」重构
3. **保留核心 DNA** — 极简主义、极速启动、低内存占用永远优先

### 目标效果（未承诺）

| 指标 | 当前 | 目标 |
|---|---|---|
| 二进制体积 | 以实际产物为准 | 记录构建增量后再评估 |
| 空白窗口内存 | 以实际测量为准 | 不因 UI 迁移显著回退 |
| 典型 PDF 阅读 | 以实际测量为准 | 保持当前体验 |
| Win11 外观 | 标准 Win32 + 系统圆角 | Mica/无边框仍待实现 |
| 触控板滚动 | WM_POINTER/惯性路径已接入 | 完善边界视觉反馈 |
| UI 动画 | 仅侧边栏 slide | 再评估通知、面板和翻页过渡 |
| Win7 兼容 | MSVC CRT shim；MinGW GDI | 保持回退路径 |


---

## 八、当前落地状态（截至 2026-09-25）

以下只记录当前工作树中能从代码和构建元数据核实的项目；未接入的功能不列为完成。

| 项目 | 当前状态 | 涉及文件 | 验证入口 |
|---|---|---|---|
| MainWindow 生命周期状态机 + WndProc 守卫 | ✅ 已接入 | src/WindowLifecycle.h, src/MainWindow.{h,cpp}, src/SumatraPDF.cpp, src/SumatraStartup.cpp | src/base/tests/WindowLifecycle_ut.cpp + tests/ad-hoc-startup-smoke.ts |
| RDP/VM 检测 + 极简降级决策 | ✅ 基本接入 | src/HardwareProfile.{h,cpp} | src/base/tests/HardwareProfile_ut.cpp |
| Renderer + GDI/D2D 抽象 | 🟡 基础设施已接入，迁移未完成 | src/gui/win/Renderer.{h,cpp}, src/gui/win/DWriteText.{h,cpp}, src/ThumbnailPanel.cpp | src/base/tests/Renderer_ut.cpp；MinGW 使用 GDI |
| 命令面板结果计数、清除按钮、分类图标 | ✅ 已接入 | src/CommandPalette*.{h,cpp} | tests/ad-hoc-command-palette-enhancements.ts |
| WM_POINTER 平移/捏合、惯性滚动、Overscroll | 🟡 基本路径已接入，完整视觉效果待补 | src/PointerInput.{h,cpp}, src/InertiaScrolling.{h,cpp}, src/OverscrollEffect.{h,cpp}, src/Canvas.cpp | src/base/tests/InputScrolling_ut.cpp |
| UI 过渡动画 | 🟡 仅侧边栏 slide 已接入 | src/MainWindow.cpp, src/gui/win/Animation.{h,cpp} | 仍需补充覆盖测试 |
| 通知弹性布局 | ✅ 已接入 | src/Notifications.cpp | tests/ad-hoc-notif-elastic.ts |
| MinGW 源文件清单 | ✅ 补齐并显式排除 MSVC-only 文件 | cmd/helper/mingw-build.ts, tests/lint-mingw-sources.ts | tests/lint-mingw-sources.ts |
| premake 生成入口与过期文件条目 | ✅ 已修正 | premake5.lua, premake5.files.lua | bun cmd/premake.ts |



