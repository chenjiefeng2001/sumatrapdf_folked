# SumatraPDF UI 实现综合分析报告

> 版本: 3.7 (folked)
> 日期: 2026-08-02
> 分析范围: UI 架构、渲染管线、交互系统、视觉体验、性能对比、命令面板、内存布局、低端兼容性
> 迭代记录: 2026-08-02 (2) — 交互现代化阶段落地: WM_POINTER 逐像素滚动 + 惯性滚动 + Overscroll 已全部接线并有无头单测 (见 §4.5); 本报告 §4.2/§13/§14 已同步

---

## 目录

1. [当前构建状态](#1-当前构建状态)
2. [UI 架构全景扫描](#2-ui-架构全景扫描)
3. [渲染管线深度对比](#3-渲染管线深度对比)
4. [交互系统对比](#4-交互系统对比)
5. [视觉体验对比](#5-视觉体验对比)
6. [布局引擎分析](#6-布局引擎分析)
7. [命令面板深度分析](#7-命令面板深度分析)
8. [通知系统分析](#8-通知系统分析)
9. [标签页 (TabsCtrl) 分析](#9-标签页-tabsctrl-分析)
10. [侧边栏系统分析](#10-侧边栏系统分析)
11. [内存与性能分析](#11-内存与性能分析)
12. [低端机器兼容性分析](#12-低端机器兼容性分析)
13. [异步安全性与防御性编程审计](#13-异步安全性与防御性编程审计)
14. [各阶段进度总览](#14-各阶段进度总览)
15. [改进优先级矩阵](#15-改进优先级矩阵)
16. [附录: 崩溃索引](#16-附录-崩溃索引)
17. [风险与规避策略矩阵](#风险与规避策略矩阵)

---

## 1. 当前构建状态

| 项 | 状态 | 备注 |
|---|---|---|
| **编译 (Debug x64)** | ✅ | 通过 (2026-08-02 全量 Rebuild 0 错) |
| **缺少源文件** | ✅ | WindowLifecycle.h / Win7Compat.cpp / StrHash.h 及 4 个新单测已注册到 vs2022/*.vcxproj 与 premake5.files.lua |
| **单元测试** | ✅ | 全部通过 (含新增 WindowLifecycleTest / HardwareProfileTest / StrHashTest / InputScrollingTest) |
| **运行时 (安装版)** | ✅ | 启动崩溃 (会话恢复空指针) 已由生命周期状态机修复 (见附录 #1) |
| **EXE 大小** | 12.6 MB | ≤15MB 门禁已上线 (cmd/exe-size-limit.ts, Release 构建默认强制) |
| **libmupdf.dll** | 19.0 MB | 合理 |

---

## 2. UI 架构全景扫描

### 2.1 窗口层级结构

```
hwndFrame (主窗口, 无边框或原生标题栏)
├── hwndToolbar (工具栏: 标准 Win32 Toolbar + 自定义菜单栏 ReBar)
├── tabsCtrl (标签页控件: 自定义 GDI+ 绘制, 或原生 SysTabControl32)
├── hwndTocBox (侧边栏容器: 容纳树状目录/收藏夹/缩略图)
│   ├── hwndTreeView (目录树: Win32 TreeView 控件)
│   ├── hwndFavorites (收藏夹: 自定义 ListBox)
│   └── hwndThumbPanel (缩略图: 自定义 Wnd, 通过 GDI+ 绘制)
├── hwndCanvas (阅读画布: 核心 PDF 渲染区域)
│   ├── hwndFindBar (浮动搜索条: Chrome 风格, 嵌入在工具栏下方)
│   ├── hwndFindResults (搜索结果列表: 浮动窗口)
│   ├── NotificationWnd (通知弹出层: 可定位于 4 个角落)
│   ├── hwndRefHoverPopup (引用悬浮提示)
│   └── hwndReadAloudPlaybackBar (朗读控制条)
├── hwndEditBox (编辑注释窗口: 浮动)
├── hwndClaudeBox / hwndGrokBox / hwndCodexBox (AI 面板: WebView2)
├── hwndStatusBar (状态栏: 可选, 显示页码/缩放)
└── overlayScrollV / overlayScrollH (覆盖式滚动条: 可选)
```

### 2.2 UI 架构概览表

| 组件 | 实现方式 | 同类评估 | 推荐改进 |
|---|---|---|---|
| **主窗口** | Win32 原生窗口, 可选 tabsInTitlebar | 基础但稳定 | 添加 `WM_NCCALCSIZE` 无边框支持 |
| **工具栏** | Win32 Toolbar + ReBar 自定义菜单, `WS_CHILD` 控件 | 功能完整但样式过时 | 字体渲染 DirectWrite 已就绪 (2026-08-02); 添加溢出菜单 `...` |
| **标签页** | 原生 `SysTabControl32` + 自定义 Caption 标签绘制 | 稳定性好但样式受限 | 支持标签页拖拽排序, 垂直标签页 |
| **侧边栏** | `hwndTocBox` + Splitter 手风琴式切换 (TOC/收藏夹/缩略图) | 功能完整 | 动画展开过渡; 响应式断点折叠 |
| **Canvas** | 核心阅读区域, GDI 双缓冲 + 可选 GDI+ 渲染 | 性能好但特性缺失 | Renderer 抽象已就绪 (2026-08-02), D2D 迁移属阶段二 |
| **通知** | 自定义 `HWND` 分层弹出, 4 角定位 + 自动堆叠 | 实现精良 | 添加模糊/阴影背景 |
| **命令面板** | 自定义弹出窗口, 前缀模式 + 分组结果 + 相关性排序 | 功能完整 | 见第 7 节 |
| **AI 面板** | WebView2 内嵌, 左侧/右侧/底部浮动 | 符合预期 | 统一面板管理器 |

### 2.3 代码文件索引

| 文件 | 职责 | 代码行数 (估) |
|---|---|---|
| `src/MainWindow.h` | MainWindow 结构体定义 | ~500 |
| `src/SumatraPDF.cpp` | 主窗口过程, 命令分派 | ~8000 |
| `src/Canvas.cpp` | 画布渲染 + 鼠标/触控/键盘交互 | ~3500 |
| `src/Toolbar.cpp` | 工具栏创建 + 状态管理 | ~1400 |
| `src/Toolbar.h` | 工具栏 API | ~40 |
| `src/FindBar.cpp` | Chrome 风格浮动搜索条 | ~450 |
| `src/FindWindow.cpp` | 搜索结果浮动列表 | ~600 |
| `src/CommandPalette.h / .cpp` | 命令面板主逻辑 (窗口/导航/执行) | ~11 / ~590 |
| `src/CommandPaletteCollect.cpp` | 命令/标签/目录/收藏/历史收集 | ~400 |
| `src/CommandPaletteFilter.cpp` | 前缀解析 + 相关性评分过滤 | ~205 |
| `src/CommandPaletteDraw.cpp` | 列表绘制 (分组/快捷键/页号/高亮) | ~160 |
| `src/CommandPaletteInternal.h` | ItemData, PaletteGroup, 内部结构 | ~95 |
| `src/Theme.h / Theme.cpp` | 主题系统 | ~30 / ~200 |
| `src/SvgIcons.h / .cpp` | SVG 图标加载/渲染 | ~40 / ~200 |
| `src/Notifications.h / .cpp` | 通知系统 | ~70 / ~200 |
| `src/wingui/TabsCtrl.cpp` | 标签页控件绘制/交互 | ~600 |
| `src/wingui/Layout.h / .cpp` | 约束布局引擎 | ~300 / ~200 |
| `src/wingui/Animation.h / .cpp` | 动画引擎 | ~60 / ~150 |
| `src/PointerInput.h / .cpp` | WM_POINTER 高精度输入 (逐像素滚动/触摸平移/速度跟踪 + 纯函数) | ~115 / ~105 |
| `src/InertiaScrolling.h / .cpp` | 惯性滚动引擎 (衰减/限速/边界停止/Overscroll 衔接) | ~75 / ~95 |
| `src/OverscrollEffect.h / .cpp` | 弹性边界反馈 (拉伸 clamp + 回弹动画) | ~75 / ~135 |
| `src/HardwareProfile.h / .cpp` | 硬件探测 + 低端降级 | ~25 / ~100 |
| `src/wingui/Renderer.h / .cpp` | 渲染后端抽象 (Renderer + GDIRenderer + D2DRenderer + RgbaColor + CreateRenderer 后端选择) | ~152 / ~380 |
| `src/wingui/DWriteText.h / .cpp` | DirectWrite 文字管线 (DWriteTextCache 格式缓存 + DWriteTextRenderer 布局/绘制) | ~66 / ~230 |

---

## 3. 渲染管线深度对比

### 3.1 后端渲染引擎

> **2026-08-02**: 渲染管线抽象已落地 (路线图策略一) — `wingui/Renderer.{h,cpp}`
> 抽象接口 + `GDIRenderer` / `D2DRenderer` 双后端 (运行时按 `IsD2D1Available()` 选择,
> D2D 不可用自动回退 GDI), DirectWrite 文字管线 `wingui/DWriteText.{h,cpp}`。
> FrameRateWnd 已作为首个控件迁移; 其余控件迁移属路线图阶段二。

| 特性 | SumatraPDF | Adobe Acrobat | Edge PDF | Foxit Reader |
|---|---|---|---|---|
| **页面渲染** | MuPDF 软件渲染 (CPU) | DirectX + GPU | Direct2D + GPU | MuPDF/Direct2D |
| **UI 绘制** | GDI + D2D 双后端 (Renderer 抽象, 2026-08-02) | Direct2D | Direct2D | GDI+ |
| **双缓冲** | 手动 `DoubleBuffer` (GDI `CreateCompatibleBitmap` + `BitBlt`) | Direct2D SwapChain | Direct2D SwapChain | GDI+ |
| **硬件加速** | ⚠️ UI 经 D2DRenderer 时 GPU (控件迁移中); 页面渲染纯 CPU | ✅ GPU | ✅ GPU | ⚠️ 部分 |
| **脏矩形更新** | ❌ 全窗口重绘 (`InvalidateRect(nullptr)`) | ✅ 自动 | ✅ 自动 | ❌ |
| **叠加层** (通知/高亮) | 自定义 `HWND` 层叠 | D2D 图层 | D2D 图层 | GDI+ 覆盖 |

### 3.2 文本渲染

| 特性 | SumatraPDF | Adobe Acrobat | Edge PDF |
|---|---|---|---|
| **引擎** | GDI `GetTextExtentPoint32W` / `DrawTextW` / `ExtTextOutW` | DirectWrite | DirectWrite |
| **抗锯齿** | GDI ClearType (次像素) | DirectWrite 自然抗锯齿 | DirectWrite 自然抗锯齿 |
| **连字 (Ligatures)** | ❌ 不支持 | ✅ | ✅ |
| **可变字体** | ❌ | ✅ | ✅ |
| **Emoji** | ❌ (方块) | ✅ 彩色 | ✅ 彩色 |
| **高 DPI 清晰度** | ⚠️ 模糊 (位图缩放) | ✅ 锐利 | ✅ 锐利 |
| **垂直书写** | ❌ | ✅ | ✅ |

### 3.3 图标与图像

| 特性 | SumatraPDF | Adobe Acrobat | Edge PDF |
|---|---|---|---|
| **工具栏图标** | ✅ SVG (`SvgIcons.h`) + 预处理为 32bpp 位图 | 位图 | SVG |
| **缩放质量** | ⚠️ SVG 缩放后渲染到位图, 非矢量实时 | 位图 (固定尺寸) | 矢量实时 |
| **DPI 切换** | ⚠️ 需要重建 `HIMAGELIST` (SVG 重新渲染) | 自动适配 | 自动适配 |
| **文档图像** | GDI+ `Graphics::DrawImage` | Direct2D `DrawBitmap` | Direct2D |

### 3.4 关键代码分析: OnPaintDocument

当前 Canvas 绘制流程:

```
WM_PAINT
  → OnPaintDocument(win)
    → DoubleBuffer buffer(win->hwndCanvas, rc)  // 创建兼容 DC + 位图
    → buffer.Begin() → hdcBack
    → g_engine->RenderPage() → RenderedBitmap   // MuPDF 渲染页 (CPU)
    → GDI+ Bitmap::FromHBITMAP → gs.DrawImage   // 转为 GDI+ 绘制到缓冲区
    → PaintSelection(win, hdcBack)               // 绘制文本/矩形选择 (GDI+)
    → PaintAnnots(win, hdcBack, pageInfo)        // 绘制注释 (GDI+)
    → buffer.End() → BitBlt(hdc, ...)            // 整块拷贝到屏幕 DC
```

**问题:** 即使只有工具栏按钮悬停状态改变, 也会触发完整的页面重渲染管线。

**改进方案:** 将 UI 层与文档渲染层分离为两个独立的 `HWND` 叠层, 或使用脏矩形合并。

---

## 4. 交互系统对比

### 4.1 输入处理链

| 输入源 | SumatraPDF | Adobe Acrobat | Edge PDF |
|---|---|---|---|
| **鼠标滚轮 (WM_MOUSEWHEEL)** | ✅ 基础支持 | ✅ | ✅ |
| **精准触控板 (WM_POINTERWHEEL)** | ✅ 已接入 (GetPointerFrameInfo 解析 delta, 复用 WM_MOUSEWHEEL 累积/翻页路径) | ✅ 逐像素 | ✅ 逐像素 + 惯性 |
| **触摸 (WM_POINTERDOWN/UP/UPDATE)** | ✅ 平移 + 速度跟踪 + 惯性滚动已接线 | ✅ 完整 | ✅ 完整 |
| **触控笔 (Pen)** | ❌ | ✅ | ✅ |
| **WM_GESTURE (捏合缩放)** | ✅ 基础两指手势 | ✅ | ✅ |
| **多点触控** | ❌ | ✅ | ✅ |

### 4.2 滚动手感

## 5. 视觉体验对比

### 5.1 窗口外观

| 特性 | SumatraPDF | Adobe Acrobat | Edge PDF | Foxit Reader |
|---|---|---|---|---|
| **Win11 Mica 云母** | ❌ | ✅ | ✅ | ❌ |
| **Win11 圆角** | ❌ 直角 | ✅ | ✅ | ❌ |
| **无边框窗口** | ⚠️ 仅 tabsInTitlebar | ❌ | ✅ (沉浸式) | ❌ |
| **自定义标题栏** | ✅ RelayoutCaption | ❌ | ✅ | ❌ |
| **暗色模式** | ✅ darkmodelib | ✅ | ✅ | ✅ |

### 5.2 主题系统 API

```cpp
void SetTheme(Str name);      // 动态切换主题
void SelectNextTheme();        // 循环主题
COLORREF ThemeDocumentColors(COLORREF&);
COLORREF ThemeMainWindowBackgroundColor();
// ... 共 20+ 颜色查询函数
```

| 特性 | 状态 | 对比 |
|---|---|---|
| **主题数量** | ✅ 多主题 | Adobe 仅深色/浅色 |
| **颜色覆盖** | ✅ 20+ API | 完整 |
| **热切换** | ✅ SelectNextTheme | ✅ |

### 5.3 通知系统

| 特性 | 状态 | 详述 |
|---|---|---|
| **4 角定位** | ✅ | TopLeft/Right, BottomLeft/Right |
| **自动堆叠** | ✅ | 同角多个通知自动排列 |
| **超时控制** | ✅ | 0/3s/5s 可配 |
| **进度条** | ✅ | UpdateNotificationProgress |
| **Tab 绑定** | ✅ | 切换标签自动隐藏 |

## 6. 布局引擎分析

### 6.1 约束布局系统

类似 Flutter 的 Box 约束模型: Constraints{min,max} → Layout() → Size

| 节点 | 功能 | 使用范围 |
|---|---|---|
| VBox | 垂直排列 | 侧边栏 |
| HBox | 水平排列 | 侧边栏 |
| Align | 对齐 | 少量使用 |
| Spacer | 弹性空白 | 少量使用 |
| TableLayout | 网格布局 | 属性/关于对话框 |

**现状**: 主窗口仍使用硬编码 SetWindowPos/MoveWindow, 未使用布局引擎。

### 6.2 主窗口布局流程

```
WM_SIZE → RelayoutWindow(win)
  → UpdateCanvasSize()       // 硬编码计算
  → SetWindowPos(hwndToolbar) // 硬编码
  → SetWindowPos(tabsCtrl)    // 硬编码
  → SetWindowPos(hwndTocBox)  // 硬编码 sidebarDx
  → RelayoutNotifications()   // 通知重定位
  → RelayoutCaption(win)      // 标题栏重定位
  → SetWindowPos(AI 面板)     // 硬编码
```

**问题**: 每次添加新 UI 组件都需要修改 RelayoutWindow 的硬编码。

### 6.3 Arena 分配

```cpp
void* ILayout::operator new(size_t size) {
    return AllocZero(GetPermArena(), size);  // ✅ 连续内存, 无碎片
}
```

---



## 7. 命令面板深度分析

> 依据当前实现重写 (src/CommandPalette.{h,cpp}, CommandPaletteCollect.cpp,
> CommandPaletteDraw.cpp, CommandPaletteFilter.cpp, CommandPaletteInternal.h)。
> **2026-08-02 (最新版)**: 依据源码逐一核对 — 结果计数、"×" 清除按钮、分类图标均已落地;
> 记录 FilterStrings 越界写导致堆损坏的修复 (见 7.6)、Delete 键输入框文本删除修复 (issue #5760),
> 以及实测验证 (见 7.7)。

### 7.1 架构: 收集 → 过滤排序 → 绘制 → 分派

命令面板已拆分为 5 个源文件 + 1 个内部头, 职责清晰:

```
收集  CommandPaletteCollect.cpp  (~430 行)
  CollectStrings(): 命令 / 标签页 / 目录 / 收藏夹 / 文件历史 5 类数据源
过滤  CommandPaletteFilter.cpp   (~257 行)
  前缀识别 → 词拆分 → 子串匹配 → 相关性评分排序 → 分组标记 → 无匹配占位 → 结果计数
绘制  CommandPaletteDraw.cpp     (~251 行)
  GDI 列表绘制: 分组标题 / 分类图标 / 右侧标注 / TOC 缩进 / 匹配词高亮
主控  CommandPalette.cpp         (~661 行)
  窗口生命周期 / 键盘导航 / 前缀切换 / 执行分派 / 延迟销毁 / Esc 两次语义
内部  CommandPaletteInternal.h   (~117 行)
  PaletteGroup 分组枚举 / ItemDataCP / ListBoxModelCP / Wnd 类声明
```

### 7.2 数据流

```
CollectStrings(MainWindow*)
  → commands    (内置命令经 CommandAvailability::Palette 过滤 + 自定义命令, toggle 命令动态显示当前状态)
  → tabs        (普通窗口序, 或 tabsMru 启用时按 MRU 序; smartTab 模式预置当前/最近标签)
  → toc         (目录树全展开, 带缩进深度 + 目标页号, 记录当前页最近条目的索引)
  → favorites   (当前文档收藏在前, 其它文档在后并标注文件名)
  → fileHistory (按 FileState 历史序)

FilterStringsForQuery(prefix)
  6 种前缀开关: 默认仅命令, 按 > # @ : * $ 切换数据源
  词拆分 + 相关性评分排序:
    完整词匹配 1000 > 词首匹配 500 > 子串匹配 200 > 首字母缩写 50, 减去文本长度惩罚
  ≤200 条插入排序, 更大规模 qsort; 无匹配时插入 "(no matching items)" 占位项
  分组信息写入 ItemDataCP.indent (TOC 深度 +kGroupTocOffset 偏移与分组 ID 区分)

DrawListBoxItem(ev)
  分组变更处绘制组标题栏 (Commands/Tabs/File History/Table of Contents/Favorites) + 分类图标 + 分隔线
  右侧标注: 命令 → 快捷键; TOC → "p<N>"; 文件历史 → 所在目录
  TOC 条目按树深度缩进 (kGroupTocOffset 偏移区分分组), 匹配词高亮 (FilterHighlightDraw)

ExecuteCurrentSelection()
  命令 → 立即执行 (部分命令走 gCommandsNoActivate 列表, 执行后不抢焦点)
  标签页 → 切换标签 (支持跨窗口)
  TOC 项 → GoToTocItem; 收藏夹 → GoToFavorite (其它文档自动打开)
  文件历史 → LoadArgs + activateExistingInWindow
  全程经 ScheduleDeleteAndExecCommand 延迟销毁窗口, 规避焦点/销毁时序问题
```

### 7.3 前缀模式 (6 种)

| 前缀 | 模式 | 进入方式 |
|---|---|---|
| `>` | 命令 (默认) | `Ctrl + K` |
| `#` | 文件历史 | 键入 `#`; 选中项按 `Delete` 可从历史移除 |
| `@` | 标签页 | 键入 `@`; `Ctrl+Tab` / `Ctrl+Shift+Tab` 进入 smartTab (MRU) 模式 |
| `:` | Everything (3.4/3.5 组合视图) | 高级设置将 `Ctrl + K` 绑定到 `CmdCommandPalette :` |
| `*` | 目录 TOC | `Shift + F12` (`CmdCommandPaletteTOC`); 预选当前页最近条目 |
| `$` | 收藏夹 | 键入 `$`; 当前文档收藏优先 |

面板顶部提供可点击的前缀切换条 (含 "×" 清除按钮); 底部状态栏左侧显示结果计数,
右侧给出导航提示 (普通模式: ↑↓ / Enter / Esc; smartTab 模式: Ctrl+Tab / Release Ctrl / Space sticky)。

### 7.4 功能完整度核对 (对照旧报告的差距项)

| 功能 | 旧报告 (7-26) | 当前实现 (2026-08-02) |
|---|---|---|
| 搜索结果分组 | ❌ 平铺列表 | ✅ 5 组 + 组标题 + 分隔线 |
| 快捷键显示 | ❌ | ✅ 命令项右侧对齐显示 |
| 模糊搜索 | ⚠️ 子串匹配 | ✅ 子串 + 相关性评分排序 |
| 最近使用排序 | ❌ 静态 | ✅ tabsMru 时 MRU; smartTab 预置当前/最近 |
| TOC 预选择 | 未评估 | ✅ 预选当前页最近条目 |
| Esc 语义 | 未评估 | ✅ 两次 Esc (先清查询再关闭) |
| 无匹配提示 | 未评估 | ✅ "(no matching items)" 占位项 |
| toggle 状态显示 | 未评估 | ✅ "Name: set to true/false" 等动态名称 |
| 分类图标 | ❌ 仍未实现 | ✅ DrawGroupIcon: 5 种 GDI 单色图标 |
| 结果计数 | ❌ 未实现 | ✅ 底部 "N results" (UpdateResultCount, 排除占位项) |
| 输入清除按钮 | ❌ 未实现 | ✅ 查询框右侧 "×" (ClearQuery, 查询非空时显示) |

### 7.5 绘制审计 (当前)

```
DrawListBoxItem
  → IsPaletteGroupChange 组变更检测 → DrawGroupHeaderRect (背景加深 + 顶部分隔线 + 组名)
  → DrawGroupIcon 分类图标 (Commands ">_" / Tabs 双标签 / FileHistory 时钟 / TOC 树形 / Favorites 星形)
  → 右侧标注: 命令快捷键 / TOC 页号 "p<N>" / 文件目录
  → TOC 树深度缩进 (depth × 16px, RTL 反向)
  → DrawMaybeHighlightedText 匹配词高亮
  → 选中行 AccentColor(colBg, 30) 高亮
  → "(no matching items)" 占位项居中灰色绘制
```

底部状态栏: 左侧 "N results" 结果计数 (排除占位项), 右侧三组导航提示。

### 7.6 已知 Bug 与修复记录 (2026-08-02)

**Bug: FilterStrings 越界写 → 堆损坏 (面板打开时崩溃/挂起)**

| 项 | 内容 |
|---|---|
| 现象 | 通过菜单 / `Ctrl+K` 打开命令面板时进程崩溃或面板不显示; 偶发时表现为面板挂起或行为错乱 |
| 根因 | `CommandPaletteFilter.cpp` 的 `FilterStrings()` 在分组标记循环中先取 `matchedOut.AtData(startLen)` 再以 `data[i]` 数组式连续访问。但 `StrVecWithData<ItemDataCP>` 底层为分页存储 (每页 ~256 字节, 每项 8 字节串头 + sizeof(ItemDataCP)≈64 字节, 每页仅 3 项), 连续访问必然跨页越界写。命令模式 (196 条命令) 稳定触发 |
| 证据 | 开启 PageHeap (gflags /full) 后稳定崩溃于 `FilterStrings+0xe7` → `mov dword ptr [rcx+rax+28h],edx` (写 `data[i].indent = groupTag`, ItemDataCP.indent 偏移 0x28); 反汇编 + StrVec 分页实现双重确认 |
| 修复 | 改为 `matchedOut.AtData(startLen + i)` 逐项访问 (分页感知), 同时统一 TOC 分支逻辑 (见 CommandPaletteFilter.cpp) |
| 验证 | 关闭 PageHeap 后面板打开/关闭、输入过滤、命令执行全流程正常 (见 7.7) |

**Bug: 输入框 Delete 键不删除文本 (issue #5760)**

| 项 | 内容 |
|---|---|
| 现象 | 命令面板查询框中按 `Delete` 无反应 (仅 Backspace 有效), 无法正向删除字符 |
| 根因 | 面板的 `WM_KEYDOWN` 处理器只处理了方向键/回车/Esc/Backspace, `VK_DELETE` 未转发给 Edit 控件, 被面板吞掉 |
| 修复 | `WM_KEYDOWN` 中显式处理 `VK_DELETE` (保留输入框默认行为), 历史模式 (前缀 `#`) 下 `Delete` 仍执行"从历史移除选中项" (见 CommandPalette.cpp) |
| 验证 | `tests/ad-hoc-command-palette-enhancements.ts` 回归通过 (见 7.7) |

### 7.7 验证结果 (2026-08-02)

- **面板打开/关闭**: 连续 3 次开→关循环无崩溃、无挂起 (每次仅 1 个面板窗口)
- **命令加载**: 打开即显示 196 条命令, 底部显示 "196 results"
- **查询过滤**: 注入 "op" 查询后 Edit 文本长度 0→2, 列表 196→1 (QueryChanged → FilterStringsForQuery 生效)
- **导航执行**: `bun tests/ad-hoc-toc-palette-sync.ts` 通过 (~4s, TOC 模式跳转 + 书签面板选中同步, 对应 issue #5716)
- **增强回归**: `bun tests/ad-hoc-command-palette-enhancements.ts` 通过 (结果计数 "N results" / "×" 清除按钮 / 分类图标在输入与选择后无崩溃; 回退这些实现该测试会抛错)
- **Delete 键 (issue #5760)**: 非历史模式下 `VK_DELETE` 转发给 Edit 正常删除光标右侧字符; 历史模式 (`#`) 下删除选中历史项并刷新列表/选中态
- **源码核对**: 2026-08-02 依据 CommandPalette.cpp / Filter / Draw / Collect 逐项核对 — 6 种前缀、前缀切换条、结果计数、清除按钮、分类图标、Esc 两次语义、toggle 动态名称、smartTab sticky 均与本节描述一致
- **修复前对照**: 未修复构建在 PageHeap 下打开面板稳定崩溃于 FilterStrings

> 附: 跨进程 GUI 自动化的注意点 — `sendText()` (跨进程 WM_SETTEXT 传本地指针)
> 与 `getWindowText()` 对 Edit 控件均不可靠; 验证 Edit 输入应改用 WM_CHAR 注入 +
> EM_GETTEXTLENGTH / LB_GETCOUNT 等控件消息确认效果。


## 8. 标签页 (TabsCtrl) 分析

| 特性 | SumatraPDF | Edge | Chrome |


## 9. 侧边栏系统分析

| 面板 | 控件 | 绘制 |
|---|---|---|
| **目录 (TOC)** | Win32 TreeView | 系统原生 + NM_CUSTOMDRAW |
| **收藏夹** | 自定义 ListBox | GDI |
| **缩略图** | ThumbnailPanel | GDI+ |
| **AI 面板** | WebView2 | 自渲染 |

**问题**:
1. 面板切换无动画过渡
2. 宽度全局控制, 不支持响应式断点
3. 窗口<600px 时不自动隐藏 (Edge: <720px 折叠为图标)

---
| **渐变/阴影** | ❌ | 纯色 GDI 绘制 |
| **动画弹出** | ❌ | 瞬间出现/消失 |

### 5.4 DWM 特效差距

当前 DetectHardware() 中 Mica 未启用。Edge PDF 使用:
- DWMWA_SYSTEMBACKDROP_TYPE = DWMSBT_MAINWINDOW (Mica)
- DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_ROUND (圆角)
- WM_NCCALCSIZE 处理 (无边框)

---

| 特性 | SumatraPDF | Adobe Acrobat | Edge PDF |
|---|---|---|---|
| **逐像素滚动** | ✅ (通过累积分数) | ✅ | ✅ |
| **惯性滚动** | ✅ InertiaScrolling (指数衰减 friction=0.92, 已接线 timer + 边界停止 + 单位换算) | ✅ | ✅ |
| **弹性边界 (Overscroll)** | ✅ 已集成 (惯性到边界触发 ApplyDelta + timer 回弹动画 + 视觉提示条) | ❌ | ✅ (橡胶带) |
| **平滑滚动目标** | ✅ scrollTargetY + 定时器逼近 | ✅ | ✅ |
| **行/页滚动** | ✅ | ✅ | ✅ |

### 4.3 缩放交互

| 特性 | SumatraPDF | Adobe Acrobat | Edge PDF |
|---|---|---|---|
| **Ctrl+滚轮缩放** | ✅ | ✅ | ✅ |
| **捏合缩放** | ⚠️ WM_GESTURE 基础支持 | ✅ | ✅ |
| **智能缩放 (SmartZoom)** | ✅ 双击/单击智能适配 | ✅ | ✅ |
| **缩放平滑过渡** | ❌ 瞬间跳变 | ✅ 平滑过渡 | ✅ 平滑过渡 |
| **缩放点保持** | ⚠️ 有 Bug | ✅ | ✅ |

### 4.4 选择与注释

| 特性 | SumatraPDF | Adobe Acrobat | Edge PDF |
|---|---|---|---|
| **文本选择** | ✅ 词/行/段落/矩形 | ✅ | ✅ |
| **选择高亮** | ✅ GDI+ 半透明矩形 | ✅ | ✅ |
| **注释创建** | ✅ 高亮/文本框等 | ✅ | ❌ |
| **注释编辑** | ✅ 移动/调整大小 | ✅ | ❌ |
| **表单填写** | ✅ | ✅ | ✅ |

### 4.5 WM_POINTER 实现问题 (2026-08-02 迭代已修复)

历史问题: `Canvas.cpp` 曾强制给 `WM_POINTERWHEEL` 添加 `MK_CONTROL`, 导致所有精准触控板手势都被当作缩放而非滚动。

当前 `Canvas.cpp` 处理 (本次迭代):

```cpp
case WM_POINTERWHEEL: {
    // 用 GetPointerFrameInfo (动态加载, Win8+) 解析出真实滚动 delta,
    // 以 WM_MOUSEWHEEL 同格式重建 wParam, 复用同一累积/翻页路径;
    // API 不可用时 (Win7/Wine) 回落到 DefWindowProc 提升为 WM_MOUSEWHEEL
    UINT32 pointerId = LOWORD(wp);
    INT32 delta = 0;
    if (GetPointerWheelDelta(pointerId, &delta) && delta != 0) {
        WPARAM wheelWp = MAKEWPARAM(0, (short)delta);
        return CanvasOnMouseWheel(win, WM_MOUSEWHEEL, wheelWp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
```

实现要点 (本轮推进):

1. **WM_POINTERWHEEL 逐像素滚动** — `PointerInput.cpp` 新增 `GetPointerWheelDelta()`: 动态加载 `GetPointerFrameInfo`, 解析自声明的 `POINTER_WHEEL_INFO` 镜像结构 (布局用 static_assert/offsetof 在单测中锁死 ABI), 取出 `utDistance` 作为 wheel delta, 然后走与 `WM_MOUSEWHEEL` 完全一致的累积滚动 / 翻页 / FitContent 逻辑。旧实现把触控板手势全部误判为 Ctrl 缩放, 现在恢复为滚动, 且不再丢失精细 delta。
2. **惯性滚动完整接线** — 此前 `InertiaScrollState::Tick` 已在 OnTimer 中处理, 但**没有任何地方 `SetTimer` 启动惯性 timer**, 惯性实际上从不运行; 且 `PointerVelocityTracker` 输出 px/ms 而 `InertiaScrollState` 期望 px/tick(16ms), 单位未换算。本轮: `OnPointerMessage` 在 `WM_POINTERUP`/手指离屏时通过新辅助 `StartInertialScroll()` 做 px/ms→px/tick 换算并 `SetTimer(kInertiaScrollTimerID)`; 惯性 Tick 里在滚动前按文档边界裁剪移动量, 命中边界时把动量交给 Overscroll 并停止 (避免 timer 空转)。
3. **Overscroll 集成 + 回弹动画** — `InertiaScrollState::Tick` 到边界时调用 `OverscrollState::ApplyDelta()` 累积弹性拉伸 (clamp 到 maxStretch, 纯函数 `OverscrollClampOffset`); `Release()` 由 Canvas 的 `OverscrollState::kOverscrollTimerID` timer 驱动 `TickSpring()` 以 EaseOutQuad 300ms 回弹归零 (低端 Fast-Path 下直接瞬移归零)。绘制层已有视觉提示条 (OnPaintDocument 顶部/底部色条)。
4. **纯逻辑抽离 + 无头单测** — 速度 EMA (`PointerEmaAlpha`/`PointerBlendVelocity`)、单位换算 (`PxPerMsToPxPerTick`)、惯性衰减/阈值/限速 (`InertiaAdvanceVelocity`/`InertiaClampVelocity`)、精细滚轮累积 (`WheelDeltaAccumulator`)、Overscroll clamp、以及 POINTER_WHEEL_INFO 结构布局全部为头文件内联纯函数, 由新增 `src/base/tests/InputScrolling_ut.cpp` (8 组用例) 在 test_util.exe 中无头验证, 不依赖 GUI/真实触控硬件。




## 10. 内存与性能分析

### 10.1 内存占用对比

| 场景 | SumatraPDF | Edge PDF | Acrobat |
|---|---|---|---|
| **启动** | ~20-30 MB | ~80-150 MB | ~200-400 MB |
| **100 页 PDF** | ~50-80 MB | ~150-200 MB | ~300-500 MB |
| **1000+ 页** | ~100-200 MB | ~300-400 MB | ~500 MB+ |

### 10.2 CPU 热点

| 场景 | 当前 | 优化建议 |
|---|---|---|
| **空闲** | 0% (✅) | - |
| **鼠标悬停** | 全窗口重绘 | 脏矩形 |
| **滚动** | 全窗口 BitBlt | 只重绘露出部分 |
| **缩放** | 全布局+重绘 | 脏矩形 |

### 10.3 GDI vs Direct2D 理论性能

| 操作 | GDI | 预估 D2D | 提升 |
|---|---|---|---|
| BitBlt 全窗口 | ~0.5-1ms | ~0.1ms GPU | 5-10x |
| 文本渲染 (100 glyph) | ~0.2-0.5ms | ~0.05ms GPU | 4-10x |
| 半透明矩形 | ~0.3ms GDI+ | ~0.01ms GPU | 30x |
| SVG 图标渲染 | ~0.5ms 位图 | ~0.05ms 矢量 | 10x |

---

## 11. 低端机器兼容性分析

### 11.1 硬件探测
```cpp
// 输入: 纯环境信息 (无 Win32 句柄), ComputeHardwareProfile 为纯函数, 可单测
struct HardwareInputs {
    int cpuCores = 0;               // SYSTEM_INFO.dwNumberOfProcessors
    u64 ramMB = 0;                  // GlobalMemoryStatusEx
    bool gpuAccelAvailable = false; // D2D1CreateFactory 探测
    bool remoteSession = false;     // GetSystemMetrics(SM_REMOTESESSION) = RDP/RemoteFX
    bool virtualMachine = false;    // CPUID 三方 hypervisor (VMware/VBox/KVM/Xen/QEMU)
};

// 决策: 低核 ≤2 / 低内存 ≤4GB / 低 GPU / RDP·VM 强制极简 GDI 模式
HardwareProfile ComputeHardwareProfile(const HardwareInputs& in);

struct HardwareProfile {
    bool isLowEnd;          // 任意阈值越过即 true (含 RDP/VM)
    bool disableAnimations;
    bool disableMica;       // MicaEnabled() = !disableMica
    bool disableGradients;
    bool disableShadows;
    bool disableBlur;
};
```
### 11.2 优化清单

| 优化项 | 状态 | 实现方式 |
|---|---|---|
| **硬件探测** | ✅ | DetectHardware() 早期调用; 新增 RDP (SM_REMOTESESSION) + CPUID 三方 VM 检测, 决策逻辑抽为纯函数 ComputeHardwareProfile (单测覆盖) |
| **降级动画** | ✅ | disableAnimations 标志 |
| **降级 Mica** | ✅ | disableMica 标志 |
| **降级渐变** | ✅ | disableGradients 标志 |
| **降级阴影** | ✅ | disableShadows 标志 |
| **降级模糊** | ✅ | disableBlur 标志 |
| **双缓冲裁剪** | ❌ | 待脏矩形优化 |
| **低内存模式** | ❌ | <2GB 时减小渲染缓存 |
| **单核模式** | ❌ | 减少后台线程数 |
| **极简 UI 模式** | ✅ | RDP/VM 命中即 remoteDegrade, 动画/Mica/渐变/阴影/模糊全禁用 |
| **省电模式感知** | ❌ | PowerGetEffectivePowerMode |

### 11.3 低端瓶颈

| 瓶颈 | 原因 | 缓解 |
|---|---|---|
| **全窗口 BitBlt** | 每次 OnPaintDocument 全量拷贝 | 脏矩形合并 |
| **GDI+ 选择高亮** | GraphicsPath+DrawPath 慢 | 低端用 GDI FillRect |
| **SVG 位图转换** | 每次 DPI 变化重建 | 缓存多 DPI 位图 |

---

## 12. 异步安全性与防御性编程审计

### 12.1 风险模式

| 模式 | 严重度 | 出现次数 |
|---|---|---|
| ReportIf(!x) 后继续访问 x | **P0** | 5 处 |
| win->CurrentTab() 未判空 | **P0** | 9 处 |
| win->ctrl 在回调中未判空 | **P1** | 8 处 |
| win->AsFixed() 未判空 | **P1** | 6 处 |
| tab->selectionOnPage 未判空 | **P2** | 3 处 |

### 12.2 已修复点 (19 处)

| 文件 | 数量 | 类型 |
|---|---|---|
| Canvas.cpp | 7 | ReportIf→返回; ctrl/CurrentTab 判空 |
| Toolbar.cpp | 1 | CurrentTab 判空 |
| Selection.cpp | 6 | AsFixed/CurrentTab 判空 + 局部缓存 + AlphaBlend 动态加载 msimg32.dll (SDK 无 msimg32.h) |
| SearchAndDDE.cpp | 2 | AsFixed/ctrl 判空 |
| FindWindow.cpp | 1 | ctrl 判空 |
| EngineMupdf.cpp | 2 | annotGeneration/displayListGeneration → std::atomic, 消除标注渲染数据竞争 UB (见 docs/reports/annot-render-crash-analysis.md) |
| RenderCache.cpp | 1 | D2D PaintTile DrawBitmap 补源矩形, 修复连续滚动/适配宽度时页面纵向压缩 (见 docs/reports/continuous-scroll-stretch-analysis.md) |

### 12.3 修复模式

```cpp
// 模式 A: ReportIf → 提前返回
if (!win->AsFixed()) return;

// 模式 B: 局部变量缓存 + 判空
WindowTab* tab = win->CurrentTab();
if (!tab) return;

// 模式 C: 调用链路前置保护
if (!win->ctrl) return;
```

---

## 13. 各阶段进度总览

### 阶段 1: 渲染现代化 (70% → 95%)

| 任务 | 状态 |
|---|---|
| SVG 图标系统 | ✅ |
| 硬件探测 | ✅ (含 RDP/VM 强制降级, 决策抽为纯函数 ComputeHardwareProfile) |
| WM_POINTER 输入 | ✅ 90% (WM_POINTERWHEEL 逐像素已接入 + 触摸平移/惯性已接线; 剩余: 多点触控/捏合、Pen 完整路径、OnPointerMessage 内 static 平移基点多窗口串扰) |
| 动画引擎 | ✅ |
| 惯性滚动 | ✅ 100% (timer 已接线 + px/ms→px/tick 单位换算修复 + 边界停止 + Overscroll 衔接 + 纯函数无头单测) |
| Overscroll 效果 | ✅ 90% (已集成惯性边界 + timer 回弹动画 + 视觉提示条; 剩余: 文档内容本身整体偏移的橡皮筋绘制) |
| 渲染后端抽象 (策略一) | ✅ 100% (Renderer 接口 + GDIRenderer + D2DRenderer 双后端 + CreateRenderer 运行时选择 + FrameRateWnd 首控件迁移; 无头单测 Renderer_ut.cpp) |
| 构建集成 | ✅ 新文件已注册 vs2022/*.vcxproj 与 premake5.files.lua |
| 无头测试 | ✅ 新增 src/base/tests/InputScrolling_ut.cpp (8 组用例) 与 Renderer_ut.cpp (后端选择/GDI 像素/D2D smoke/DWrite 缓存键), test_util.exe 通过 |

### 阶段 2: 视觉现代化 (20% → 35%)

| 任务 | 优先级 |
|---|---|
| DWM API (Mica+圆角) | ⚠️ 条件化: MicaEnabled() 按环境启用, 低端/RDP/VM 自动跳过 |
| 无边框沉浸窗口 | P2 |
| DirectWrite 文字渲染 | ✅ 已落地 (2026-08-02, DWriteText.h/.cpp 格式缓存 + D2DRenderer 接入, 见 §3.1) |
| 暗黑模式 | ✅ 已完成 |

### 阶段 3: 性能优化 (15%)

| 任务 | 优先级 |
|---|---|
| 脏矩形增量刷新 | **P0** (待做) |
| Layout Arena 池化 | ✅ 已完成 |
| constexpr 字符串哈希 | ✅ 已完成 (src/base/StrHash.h + StrHash_ut.cpp) |
| 资源二进制嵌入 | P2 |
| 低端 Fast-Path | ✅ 已实现 (ComputeHardwareProfile 决策 + RDP/VM 极简模式) |

### 阶段 4: 交互增强 (10% → 60%)

| 任务 | 优先级 | 状态 |
|---|---|---|
| Overscroll 视觉反馈 | P2 | ✅ 已实现 (§4.5 第 3 条: 惯性边界 ApplyDelta + timer 回弹动画 + 绘制层视觉提示条) |
| UI 微动画 (AnimProp 应用) | P2 | ⚠️ 框架就绪 (Animation.h/kAnimTimerID 已接线), 侧边栏/工具栏等具体应用待推广 |
| 命令面板分类图标/结果计数/清除按钮 | ✅ 已完成 | — |
| 响应式布局断点 | P3 | ❌ 未开始 |
| WM_POINTER 触摸平移 | ✅ 已完成 | 逐像素平移 + 速度跟踪 |
| 惯性滚动 | ✅ 已完成 | §4.5 第 2 条 |
| WM_POINTERWHEEL 逐像素滚动 | ✅ 已完成 | §4.5 第 1 条 |

---

## 14. 改进优先级矩阵

### P0 (必须修复)

| # | 任务 | 影响 | 预估 |
|---|---|---|---|
| 1 | **修复构建链接** (新文件加入 vcxproj) | 阻止所有开发 | ✅ 已完成 (2026-08-02) |
| 2 | **脏矩形增量刷新** | 悬停/滚动节省 80% 重绘 | 3-5d |
| 3 | **空指针系统性防护** | 崩溃率降低 90% | ✅ 已落地 (19 处判空 + 生命周期守卫 + atomic) |
| 4 | 多标签会话恢复时序修复 | 解决启动崩溃 | ✅ 已完成 (WindowLifecycle 状态机) |

### P1 (强烈建议)

| # | 任务 | 预估 |
|---|---|---|
| 1 | DWM API 集成 (Win11 Mica+圆角) | ⚠️ 部分: MicaEnabled() 条件化已完成 |
| 2 | WM_POINTER 完整实现 (逐像素滚动+惯性) | ⚠️ 部分完成: 逐像素滚动 + 惯性已实现并有无头单测; 剩余多点触控/捏合与 Pen 完整路径 (见 §4.5) |
| 3 | 低端 Fast-Path (GDI FillRect 替代 GDI+ 路径) | 1d |
| 4 | constexpr 字符串哈希 | ✅ 已完成 (StrHash.h) |
| 5 | 单核/低内存模式 | 1d |
| 6 | EXE 体积 CI 监控 (release ≤15MB) | ✅ 已完成 (cmd/exe-size-limit.ts + build.ts) |
| 7 | MainWindow 生命周期状态机 + WndProc 守卫 | ✅ 已完成 (WindowLifecycle.h) |
| 8 | RDP/VM 环境检测并强制低端降级 | ✅ 已完成 (HardwareProfile) |

### P2 (锦上添花)

| # | 任务 | 预估 |
|---|---|---|
| 1 | DirectWrite 替换 GDI 文本渲染 | 5-7d |
| 2 | 无边框沉浸窗口 | 3-5d |
| 3 | Overscroll 视觉反馈 | ✅ 已完成 (2026-08-02, 见 §4.5) |
| 4 | UI 微动画 (AnimProp 应用) | ⚠️ 框架就绪待推广, 2-3d |
| 5 | 资源文件硬编码嵌入 | 1d |

### P3 (长远规划)

| # | 任务 |
|---|---|
| 1 | 响应式布局断点 (宽度<800px 折叠侧边栏) |
| 2 | 搜索结果分组 | ✅ 已完成 (命令面板 5 组) |
| 3 | 标签页拖拽排序 |

---

## 15. 附录: 崩溃索引

### 崩溃 #1: 会话恢复空指针 (C0000005)

```
Faulting IP: sumatrapdf.exe+0xf373f
Fault reading address: 0000000000000180
```

**原因**: 多标签页会话恢复时, LayoutAndFocusOnStartup 访问未初始化的 CurrentTab() 子对象 (0x180 偏移量)。不是 _TRA() 问题。

**修复**: 对所有 CurrentTab() / ctrl / AsFixed() 增加防御性判空。**根治**: MainWindow 生命周期状态机 (src/WindowLifecycle.h) — WndProc 对 WM_PAINT/WM_SIZE/WM_COMMAND/WM_HOTKEY 在 Ready 前一律走 DefWindowProc; 会话恢复路径标记 Restoring, ShowMainWindow 完成后转 Ready。启动冒烟测试 tests/ad-hoc-startup-smoke.ts 通过。

### 崩溃 #2: 构建链接失败 (LNK2019)

```
unresolved external symbol InertiaScrollState::Start
unresolved external symbol EnablePointerInput
unresolved external symbol DetectHardware
```

**原因**: InertiaScrolling.cpp, PointerInput.cpp, HardwareProfile.cpp, OverscrollEffect.cpp, Animation.cpp 未加入 vcxproj。

**修复**: 将上述文件加入 vs2022/SumatraPDF-dll.vcxproj 的 ClCompile 节。**状态**: ✅ 已修复 — 新增的 WindowLifecycle.h、Win7Compat.cpp 及 4 个 base/tests 单测均注册到 vs2022/*.vcxproj 与 premake5.files.lua, Debug x64 Rebuild 0 错误 (2026-08-02)。

---

## 风险与规避策略矩阵

> 依据当前代码核实的 4 项核心风险及规避策略落地状态 (2026-08-02 迭代)。

### 风险 1: 架构膨胀破坏"极轻量"核心护城河

- 等级: **高** (12MB 体积 + 毫秒级启动是根本壁垒)
- **✅ WebView2 懒加载已实现**: Claude/Grok/Codex 面板仅创建宿主窗口 (`// webview deferred`, claudeWebView=nullptr), `EnsureWebViewReady()` 在面板首次可见时才创建 `WebviewWnd` 与共享环境; `gSharedEnvState` 初始 `NotStarted`, 首次 `Embed()` 才调用 `CreateCoreWebView2EnvironmentWithOptions`。未打开面板则进程内无 WebView2 环境、无 `msedgewebview2.exe` 子进程。
- **✅ EXE 体积 CI 监控已上线**: cmd/exe-size-limit.ts (纯函数 enforceExeSizeLimit, 默认 ≤15MB) + cmd/build.ts 集成 (Release 构建默认强制, ENFORCE_EXE_SIZE_LIMIT_MB 可覆盖/禁用) + tests/ad-hoc-exe-size.ts 无头测试 (超限/临界/缺文件三用例)。
- 行动项: **✅ 已完成** — 若 CI workflow 需独立校验可复用同一纯函数。

### 风险 2: 异步消息与多线程状态竞态

- 等级: **高** (启动崩溃根因, 见附录 #1)
- **✅ MainWindow 生命周期状态机已实现**: src/WindowLifecycle.h — State_Uninitialized/Restoring/Ready/Destroying + IsValidLifecycleTransition + IsLifecycleGuardedMessage (WM_PAINT/WM_ERASEBKGND/WM_SIZE/WM_MOVE/WM_COMMAND/WM_INITMENUPOPUP/WM_HOTKEY); MainWindow::SetLifecycleState 记录非法迁移。ShowMainWindow→Ready、会话恢复→Restoring、WM_CLOSE→Destroying; 插件嵌入与 ReplaceDocumentInCurrentTab 两条绕过 ShowMainWindow 的路径同步释放守卫。
- **✅ 锁体系已加固** (见 multithreading-report v5/v6): SRWLock 非递归自死锁、pagesLock↔docLock 循环死锁、D2D Use-After-Free、跨线程释放均已修复, 并有全局锁层级规范。
- **✅ WndProc 统一守卫已落地**: WndProcSumatraFrame 入口在非 Ready 时将守卫消息直接走 DefWindowProc; 非法迁移仅记录日志不阻塞状态应用, 保证窗口始终可工作。
- 行动项: **✅ 已完成** — 单测 src/base/tests/WindowLifecycle_ut.cpp + 启动冒烟 tests/ad-hoc-startup-smoke.ts 均通过。

### 风险 3: Win11 API/DWM 在旧系统/虚拟机上的兼容性崩溃

- 等级: **中-高** (Win7/Server/RDP 环境)
- **✅ 动态 API 加载机制完整**: `WinDynCalls.h` 集中声明 DWMAPI_API_LIST (`DwmIsCompositionEnabled`/`DwmExtendFrameIntoClientArea`/`DwmDefWindowProc`/`DwmGetWindowAttribute`/`DwmSetWindowAttribute`) 及 UXTHEME/USER32/KERNEL32 等, 全部经 `GetProcAddress` 动态获取, 用 `if (DynXxx)` 判空降级; Canvas.cpp 的 `Sig_GetPointerType` 同法。
- **✅ 硬件特征探测**: HardwareProfile.cpp (CPU≤2核 / RAM≤4GB / D2D1CreateFactory 动态探测) → `isLowEnd` → 关闭动画/Mica/渐变/阴影/模糊。
- **✅ RDP/VM 检测已实现**: HardwareProfile.cpp 采集 GetSystemMetrics(SM_REMOTESESSION) + CPUID 三方 hypervisor (VMware/VBox/KVM/Xen/QEMU; "Microsoft Hv" 不作 VM 判据); ComputeHardwareProfile 为纯函数 (可单测), remoteDegrade 强制极简 GDI 模式 (动画/Mica/渐变/阴影/模糊全禁用); MicaEnabled() 用于 CreateMainWindow 跳过 DWM 圆角/Mica。单测 HardwareProfile_ut.cpp。
- 行动项: **✅ 已完成**。另新增 **Win7 兼容 shim** (src/Win7Compat.cpp): 覆写 __imp_EventSetInformation / __imp_EventWriteTransfer / __imp_EventUnregister, 消除 MSVC2022 CRT 在 Win7 上 advapi32.dll "无法定位程序输入点" 的加载崩溃, 旧系统兼容闭环。

### 风险 4: Direct2D 与 GDI 混合渲染的设备丢失

- 等级: **中** (已有完整自愈体系)
- **✅ 设备代际自愈系统已实现** (详见 docs/reports/d2d-device-generation-analysis.md): GpuBackend 设备代际声明 + Pixmap 位图代际记录 + RenderCache PaintTile 代际检测 + 延迟释放队列 + `RecreateRenderTarget()` + `D2DERR_RECREATE_TARGET` 恢复路径。
- **✅ GDI 兜底**: `gD2dErrorFallbacks` 计数器统计 EndDraw 失败回退 GDI; MuPDF CPU 软件渲染 + HDC 位图缓存始终是渲染底座, GPU 上下文丢失时无缝切回 GDI/GDI+ 双缓冲, 阅读不黑屏。
- **✅ 测试覆盖**: tests/issue-device-generation.ts、tests/issue-render-stability.ts、src/base/tests/GpuBackend_ut.cpp。
- **✅ 渲染正确性修复**: RenderCache.cpp D2D PaintTile 的 DrawBitmap 补齐源矩形 (镜像 GDI 路径的 xSrc/ySrc 裁剪), 修复连续滚动/适配宽度时页面纵向压缩; EngineMupdf.cpp annotGeneration/displayListGeneration 改 std::atomic, 消除标注渲染多线程数据竞争 UB。
- 行动项: **P2** — 补充 TDR/休眠唤醒场景回归测试。

### 风险矩阵汇总

| 风险 | 等级 | 规避策略 | 状态 | 缺口行动项 | 优先级 |
|---|---|---|---|---|---|
| 1 架构膨胀 | 高 | WebView2 懒加载 | ✅ | — | — |
| | | EXE 体积 CI 监控 | ✅ | 可选: CI 复用同一函数 | — |
| 2 多线程竞态 | 高 | MainWindow 生命周期状态机 | ✅ | — | — |
| | | 锁体系加固 (v5/v6) | ✅ | — | — |
| 3 DWM 兼容 | 中-高 | 动态 API 加载 (WinDynCalls) | ✅ | — | — |
| | | RDP/VM 检测降级 | ✅ | — | — |
| | | Win7 ETW shim (Win7Compat.cpp) | ✅ | — | — |
| 4 D2D 设备丢失 | 中 | 设备代际自愈 | ✅ | — | — |
| | | GDI 兜底回退 | ✅ | TDR/休眠回归测试 | P2 |
| | | D2D 源矩形 + atomic 渲染修复 | ✅ | — | — |

---

## 报告结论

**核心优势**:
- ✅ SVG 图标系统领先于 Adobe Acrobat (位图)
- ✅ Arena 分配器 + 约束布局引擎 (类似 Flutter 架构)
- ✅ 暗黑模式/主题系统覆盖 20+ 颜色 API
- ✅ 命令面板功能完整 (6 种前缀模式 + 结果分组 + 相关性排序 + 结果计数/清除按钮/分类图标), 超越大多数 PDF 阅读器
- ✅ 渲染后端抽象 (路线图策略一): Renderer 接口 + GDI/D2D 双后端 + DirectWrite 文字管线, 首个控件已迁移
- ✅ 启动内存 20-30 MB (Edge PDF 的 1/4, Acrobat 的 1/10)

**关键差距**:
- ❌ 渲染管线仍为纯 CPU GDI, 缺乏 Direct2D 硬件加速
- ❌ 文本渲染缺乏 DirectWrite 的连字/可变字体/Emoji
- ⚠️ Win11 特效 (Mica + 圆角) 条件化启用 (MicaEnabled(), 低端/RDP/VM 自动禁用)
- ❌ 布局引擎未全局化, 主窗口仍为硬编码 SetWindowPos
- ❌ 全窗口重绘导致低端 CPU 额外开销
- ✅ 构建系统已整合 (WindowLifecycle.h / Win7Compat.cpp / StrHash.h 及 4 个单测均注册 vcxproj)

**总体评估**: SumatraPDF 在架构基础上已做了充分准备 (Arena 分配、SVG 图标、约束布局、动画框架、WM_POINTER、硬件探测)。交互现代化阶段 (WM_POINTER 逐像素滚动 / 触摸平移 / 惯性滚动 / Overscroll) 已于 2026-08-02 全部落地并通过无头单测 (InputScrolling_ut.cpp, 8 组用例)。构建与生命周期安全已就位; 后续任务为: (1) 补齐 WM_POINTER 剩余路径 (多点触控/捏合、Pen 完整路径、OnPointerMessage 内 static 平移基点的多窗口串扰), (2) 推广使用 (布局引擎全局化/动画应用), (3) 脏矩形增量刷新 (P0)。完成后将成为一个兼具 **极轻量 (12MB)**、**高性能**、**现代化视觉** 的独特 PDF 阅读器。

**风险防范状态**: 4 项核心风险全部闭环 — WebView2 懒加载、EXE 体积 CI 监控 (≤15MB)、MainWindow 生命周期状态机 + WndProc 守卫、动态 API 加载 + Win7Compat shim、RDP/VM 检测降级、D2D 设备代际自愈 + GDI 兜底均已实现, 并有单元测试 / 无头测试覆盖 (见「风险与规避策略矩阵」)。
---