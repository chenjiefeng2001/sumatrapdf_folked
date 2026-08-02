# 连续缩放/滚动时页面压缩与拉伸分析报告
# 渲染管道架构分析（Rendering Pipeline Analysis）

## 修订记录

| 版本 | 日期 | 描述 | 作者 |
| :--- | :--- | :--- | :--- |
| v1 | 2026-07 | 初始 Bug 分析报告 | AI |
| v2 | 2026-07 | 升级为渲染管道架构设计文档，引入 Geometry Version、Immutable Frame Snapshot、根因树、Frame Timeline、Queue Saturation 分析、Waste Rendering 分析及 Render Pipeline Redesign 架构建议 | AI |

---

## 1. 概述

本报告分析 SumatraPDF 在连续缩放（滚轮缩放）或连续滚动（快速翻页）时，页面出现**垂直压缩（Squashed）**、**拉伸（Stretched）**或**上下页互相挤压**的视觉异常现象。

### 1.1 问题的本质

这是一个**渲染管道（Rendering Pipeline）中几何信息（Geometry）与位图缓存（Bitmap Cache）生命周期分离**引发的系统性问题，而非单一的代码 Bug。

### 1.2 复现场景

- 快速滚动鼠标滚轮缩放（如从 150% 快速滚到 154%）
- 在 TouchPad 上两指缩放
- 连续滚动浏览含大量图片的 PDF 文档

---

## 2. 根因树（Root Cause Tree）

以下根因树揭示了问题的**因果层级结构**。四个"根因"并非并列独立的关系，而是从**根本因 → 派生因 → 触发因 → 放大因**的递进链条：

```
页面压缩 / 拉伸（视觉异常）
 │
 ├── [根本因] Geometry 与 Bitmap 生命周期分离
 │     │
 │     ├── Layout（几何布局）已更新至新缩放
 │     │     └─ DisplayModel::Relayout() 立即响应 zoom 变化
 │     │
 │     └── Bitmap（栅格化位图）尚未更新至新缩放
 │           └─ 渲染线程需要数十毫秒才能完成新位图生成
 │
 ├── [派生因] Bitmap Fallback 回退机制（kInvalidZoom）
 │     │
 │     └─ PaintTile 未命中当前 zoom 的缓存时
 │           └─ 回退到 Find(dm, ..., kInvalidZoom, &tile)
 │                 └─ 使用旧缩放位图填充新布局矩形
 │
 ├── [触发因] Stretch Rendering 强行拉伸
 │     │
 │     ├─ GDI 路径：StretchBlt(factor ≠ 1.0)
 │     └─ GPU 路径：DrawBitmap(dst 基于新 zoom，d2dBmp 基于旧 zoom)
 │
 └── [放大因] 异步管道的固有延迟（Async Pipeline Gap）
       │
       ├─ UI 线程以 120Hz 频率绘制
       └─ Render 线程以 30-60fps 频率生成新位图
             └─ 永远存在 1-3 帧的"滞后窗口"
```

**核心结论**：整个问题的**唯一根本因**是 **Geometry 与 Bitmap 生命周期分离**。其余三个均是此根本因派生出的代码表现。


---

## 3. 渲染管道生命周期分析（Render Pipeline Lifecycle）

### 3.1 完整生命周期图

```
 ┌─────────────────────────────────────────────────────────────────┐
 │                        UI 线程 (120Hz)                          │
 │                                                                 │
 │  Zoom=150%                                                      │
 │     │                                                            │
 │     ▼                                                            │
 │  SetZoomVirtual(150%)                                            │
 │     │                                                            │
 │     ▼                                                            │
 │  CalcZoomReal(150%)  ─────────►  zoomReal = 150%                 │
 │     │                                                            │
 │     ▼                                                            │
 │  Relayout(150%)                                                  │
 │     │                                                            │
 │     ▼                                                            │
 │  Page[0].pos.dx = (int)(pageSize.dx * 1.50 + 0.499)             │
 │  Page[0].pos.dy = (int)(pageSize.dy * 1.50 + 0.499)             │
 │  Page[0].pageOnScreen = ...                                      │
 │     │                     ▲                                      │
 │     │                     │ 几何信息已更新                        │
 │     ▼                     │                                      │
 │  Paint()                  │                                      │
 │     │                     │                                      │
 │     ▼                     │                                      │
 │  PaintTile()              │                                      │
 │     │                     │                                      │
 │     ├─ GetTileOnScreen() ─┘   ← 使用最新几何                     │
 │     │                                                            │
 │     ├─ Find(zoom=150%) → MISS                                    │
 │     ├─ Find(kInvalidZoom) → HIT (zoom=148%)                      │
 │     │                                                            │
 │     └─ StretchBlt(148% → 150%)  ← 几何失配！                     │
 │           │                                                      │
 │           ▼                                                      │
 │  RequestRendering(150%)                                          │
 │           │                                                      │
 └───────────┼─────────────────────────────────────────────────────┘
             │
             │  异步消息 (PostMessage / Semaphore)
             │
             ▼
 ┌─────────────────────────────────────────────────────────────────┐
 │                     渲染线程 (30-60fps)                          │
 │                                                                 │
 │  RenderPage(args)                                                │
 │     │                                                            │
 │     ├─ AcquireSRWLockShared(&docLock)                            │
 │     ├─ GetOrBuildPageDisplayList()                               │
 │     ├─ fz_run_display_list()                                     │
 │     ├─ fz_new_pixmap_from_page()                                 │
 │     └─ 生成 150% 的 Pixmap                                       │
 │           │                                                      │
 │           ▼                                                      │
 │  cache->Add(req, bmp)  ───────► 缓存可命中                       │


---

## 4. Geometry Version：缺失的架构概念

### 4.1 当前系统的状态追踪

当前系统追踪以下几何状态：

```cpp
// DisplayModel.h — 现有的状态变量
float zoomReal;       // 缩放系数
float zoomVirtual;    // 虚拟缩放
int rotation;         // 旋转角度
```

缓存查找基于这些显式值：

```cpp
// RenderCache.cpp — 缓存查找
entry = Find(dm, pageNo, dm->GetRotation(), zoom, &tile);
```

### 4.2 几何信息清单

影响页面几何的全部因素包括：

| 因素 | 当前是否追踪 | 影响维度 |
| :--- | :--- | :--- |
| Zoom（缩放） | ✅ 显式追踪 | 宽、高 |
| Rotation（旋转） | ✅ 显式追踪 | 宽、高交换 |
| DPI | ✅ 隐式含于 zoomReal | 宽、高 |
| Viewport Width | ❌ 未追踪 | 连续模式下影响 FitWidth |
| Continuous Mode | ❌ 未追踪 | 页面间距、滚动行为 |
| Page Gap（页面间距） | ❌ 未追踪 | 垂直位置 |
| Display Mode | ❌ 未追踪 | 单页/双页/书脊模式 |

### 4.3 建议引入 Geometry Version

现代渲染引擎（PDFium、Chromium）的核心设计之一是**几何版本号**：

```cpp
// 建议的设计
struct GeometrySnapshot {
    uint64_t version;        // 全局自增版本号
    float zoomReal;
    int rotation;
    float dpiFactor;
    int viewportWidth;
    DisplayMode displayMode;
    Size pageSpacing;
};

// 每次几何变化时版本递增
uint64_t g_nextGeometryVersion = 1;

void DisplayModel::Relayout(float newZoom, int newRotation) {
    geometryVersion = g_nextGeometryVersion++;


---

## 5. StretchBlt 的正确性分析

### 5.1 StretchBlt 本身不是 Bug

`StretchBlt`（以及 GPU 路径的 `DrawBitmap`）是 Windows GDI 提供的**标准位图缩放函数**。其设计目的就是在不同分辨率之间进行图像缩放。

在 PDF 查看器中，`StretchBlt` 的作用是：

```
旧 Bitmap
  │
  ▼
旧 Geometry (zoom=148%)
  │
  ▼
StretchBlt
  │
  ▼
旧 Geometry (zoom=148%)

✅ 正确：StretchBlt 在同一几何内使用时完全正确
```

### 5.2 什么情况下 StretchBlt 成为问题

```


---

## 6. 帧时间线分析（Frame Timeline）

### 6.1 连续缩放的帧序列

```
Frame 0                              Frame 1
┌─────────────────────┐              ┌─────────────────────┐
│ Layout: 150%        │              │ Layout: 151%        │
│ Bitmap: 150% ✓      │              │ Bitmap: 150% ✗      │
│ Render: cached      │              │ Render: requesting  │
│ Paint: ✅ direct    │              │ Paint: ⚠️ Stretch    │
└─────────────────────┘              └─────────────────────┘

Frame 2                              Frame 3
┌─────────────────────┐              ┌─────────────────────┐
│ Layout: 152%        │              │ Layout: 153%        │
│ Bitmap: 150% ✗      │              │ Bitmap: 151% ✗      │
│ Render: queued      │              │ Render: queued      │
│ Paint: ⚠️ Stretch   │              │ Paint: ⚠️ Stretch   │
└─────────────────────┘              └─────────────────────┘

Frame 4                              Frame 5 (稳定)
┌─────────────────────┐              ┌─────────────────────┐
│ Layout: 154%        │              │ Layout: 154%        │
│ Bitmap: 152% ✗      │              │ Bitmap: 154% ✓      │
│ Render: queued      │              │ Render: cached      │
│ Paint: ⚠️ Stretch   │              │ Paint: ✅ direct    │
└─────────────────────┘              └─────────────────────┘
```

### 6.2 关键指标

| 指标 | 值 |


---

## 7. 渲染队列饱和与浪费分析（Queue Saturation & Waste Rendering）

### 7.1 队列积压过程

```
Zoom Event           Render Queue State           Visible Frame
─────────           ──────────────────           ─────────────
150%                [150%]                        150% ✅ 命中
151%                [150%, 151%]                  150% ⚠️ Stretch
152%                [150%, 151%, 152%]            150% ⚠️ Stretch
153%                [150%, 151%, 152%, 153%]      151% ⚠️ Stretch
154%                [150%, 151%, 152%, 153%,      151% ⚠️ Stretch
                      154%]
155%                [150%, 151%, 152%, 153%,      152% ⚠️ Stretch
                      154%, 155%]
```

### 7.2 问题分析

当 `MAX_PAGE_REQUESTS = 8` 时：

1. **队列积压**：快速缩放事件产生大量请求，队列迅速填满
2. **过期请求占用**：150%、151% 等旧请求占据队列位置，阻止新请求入队
3. **渲染线程浪费**：线程依次处理 150%、151%、152%... 但这些结果在产出时 UI 早已进入下一缩放

### 7.3 浪费渲染（Waste Rendering）分析

```
渲染线程实际生成的位图：
  [150%] → [151%] → [152%] → [153%] → [154%] → [155%]
     │         │        │        │        │        │
     ▼         ▼        ▼        ▼        ▼        ▼
UI 实际显示的位图：
  [150%] → [155%] (缩放停止后)
     │         │
     ▼         ▼
  有效      有效

  中间缩放 (151%-154%)：全部浪费！
              ↓
  浪费率：4/6 = 66.7%


---

## 8. 修复方案（升级版）

### 8.1 P0 — Geometry Version 机制（架构级修复）

#### 8.1.1 引入 Geometry Version

在 `EngineBase.h` / `DisplayModel.h` 中增加：

```cpp
// 全局几何版本计数器
extern uint64_t g_nextGeometryVersion;

// 位图缓存条目增加几何版本
struct BitmapCacheEntry {
    Pixmap* bitmap;
    uint64_t geometryVersion;  // 位图生成时的几何版本
    int pageNo;
    int rotation;
    // ...
};
```

#### 8.1.2 版本校验与拒绝

```cpp
// RenderCache.cpp — PaintTile 几何版本校验
uint64_t currentGeoVersion = dm->GetGeometryVersion();

if (entry->geometryVersion != currentGeoVersion) {
    // 几何版本不匹配 → 不能直接绘制
    // 解码：Layout 已更新但 Bitmap 尚未更新
    return renderDelay;  // 跳过绘制，强制等待新位图
}
```

#### 8.1.3 SetZoomVirtual 时递增版本

```cpp
void DisplayModel::SetZoomVirtual(float zoom, Point* fixPt) {
    geometryVersion = g_nextGeometryVersion++;  // 几何变更，版本递增
    CalcZoomReal(zoom);
    Relayout(zoomVirtual, rotation);
    // ...
}
```

### 8.2 P1 — Latest-Only 渲染队列（消除 Waste Rendering）

#### 8.2.1 合并策略

```cpp
void RenderCache::RequestRendering(PageRenderRequest& req) {
    ScopedCritSec scope(&requestAccess);

    // 查找是否已有同一页面的请求在队列中
    for (int i = 0; i < requestCount; i++) {
        if (requests[i]->dm == req.dm &&
            requests[i]->pageNo == req.pageNo) {
            // 更新为最新请求（丢弃旧缩放请求）
            requests[i]->zoom = req.zoom;
            requests[i]->geometryVersion = req.geometryVersion;
            return;  // 不新增队列条目
        }
    }

    // 仅当队列未满时追加
    if (requestCount < MAX_PAGE_REQUESTS) {
        requests[requestCount++] = req.Clone();
        ReleaseSemaphore(startRendering, 1, nullptr);
    }
}
```

#### 8.2.2 预期效果

```
Before (队列积压):
  [150%] [151%] [152%] [153%] [154%] [155%]  ← 6 个请求
  ├── 只有 155% 最终可见 ──┤ 浪费率 83%

After (Latest-Only):
  [155%]  ← 1 个请求（始终保持最新）
  ├── 100% 有效 ──┤ 浪费率 0%
```

### 8.3 P2 — Progressive Rendering（渐进式渲染）

```cpp
void RenderCache::RequestRendering(PageRenderRequest& req) {


---

## 9. 修复排期总表

| 优先级 | 修复项 | 类型 | 难度 | 效果 | 依赖 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **P0** | Geometry Version 机制 | 架构 | 中 | ✅ 从架构上消除几何失配 | 无 |
| **P0** | Version Match 校验 | 防御 | 低 | ✅ 阻止跨几何绘制 | 依赖 Geometry Version |
| **P1** | Latest-Only 渲染队列 | 架构 | 中 | ✅ 消除 83% 的浪费渲染 | 无 |
| **P1** | 限制 StretchBlt 阈值 | 治标 | 低 | ✅ 消除视觉可感知拉伸 | 无 |
| **P2** | Progressive Rendering | 架构 | 高 | ✅ 消除白屏 + 渐进清晰 | 依赖 Latest-Only |
| **P2** | 缩放结束延迟渲染 | 优化 | 中 | ⚠️ 减少中间帧 | 无 |
| **P3** | Predictive Rendering | 架构 | 高 | ✅ 减少滚动时等待 | 依赖几何版本 |
| **P3** | GPU 高质量插值 | 优化 | 低 | ⚠️ 改善 D2D 拉伸质量 | 无 |


---

## 10. 渲染管道重构建议（Render Pipeline Redesign）

### 10.1 当前架构：Bitmap-First

```
Layout Update
     │
     ▼
Bitmap Cache Lookup
     │
     ├─ HIT → Paint immediately
     │
     └─ MISS → Stretch old bitmap + Request async render
                │
                ▼
           Async Render → Cache Update
```

**问题**：Bitmap 缓存的可用性决定了绘制行为，而非几何一致性。

### 10.2 建议架构：Geometry-First (Immutable Frame)

```
Geometry Snapshot
     │
     ▼
Immutable Frame
     │
     ├─ geometryVersion = N
     │
     ├─ Render Task (线程安全的异步任务)
     │      │
     │      └─ 在 Worker Thread 上执行 MuPDF 渲染
     │            │
     │            └─ 产出 bitmap[N]
     │
     └─ Present (严格在 UI 线程)
            │
            ├─ 检查 geometryVersion 是否仍为 N
            │


---

## 11. 参考

- `src/RenderCache.cpp`: `PaintTile()` (第 1069 行), `Paint()` (第 1310 行)
- `src/DisplayModel.cpp`: `Relayout()` (第 845 行), `SetZoomVirtual()` (第 1791 行)
- `src/DisplayModel.h`: `PageInfo` 结构体
- `src/RenderCache.h`: `BitmapCacheEntry` 结构体
- `docs/reports/annot-render-crash-analysis.md`: 标注导致页面渲染崩溃与卡死分析报告（修订版 v5）
- `docs/reports/multithreading-report.md`: 多线程与异步并发架构及性能优化报告（修订版 v4）
            ├─ [匹配] → DrawBitmap
            │
            └─ [不匹配] → Discard (几何已变更，位图过时)
```

### 10.3 关键设计原则

| 原则 | 说明 |
| :--- | :--- |
| **Immutable Frame** | 每一帧的几何信息在帧开始时被"快照"，帧期间不再变化 |
| **Geometry Version** | Bitmap 与 Layout 通过版本号严格配对 |
| **No Cross-Geometry Rendering** | 禁止将旧几何的位图用于新几何的绘制 |
| **Latest-Only Queue** | 渲染队列中同一页面只保留最新请求，消除浪费 |
| **Progressive Resolution** | 先出低分辨率，再出高分辨率，而非拉伸旧位图 |

### 10.4 与主流 GPU UI 框架的对比

| 框架 | 机制 | 类似设计 |
| :--- | :--- | :--- |
| **Chrome / Chromium** | cc::LayerTreeHost → 每一帧是独立快照 | Immutable Frame |
| **Skia** | SkCanvas → SkSurface 快照 + GrContext 提交 | Geometry Snapshot |
| **Flutter** | Layer Tree → Scene → GPU | Immutable Scene |
| **Avalonia** | Composition Target → Render Loop | Versioned Render |
| **Qt Quick** | QSGNode → QSGRenderer | Scene Graph |
| **SumatraPDF（当前）** | DisplayModel → RenderCache → Stretch | Bitmap-First ❌ |
| **SumatraPDF（建议）** | Geometry Snapshot → Render Task → Present | Geometry-First ✅ |
    // 阶段 1：低分辨率（快速产出，消除白屏）
    req.renderQuality = RENDER_LOW_DPI;   // 0.5x
    PostRenderRequest(req);

    // 阶段 2：中分辨率
    req.renderQuality = RENDER_MED_DPI;   // 0.75x
    PostRenderRequest(req);

    // 阶段 3：完整分辨率
    req.renderQuality = RENDER_FULL_DPI;  // 1.0x
    PostRenderRequest(req);
}
```

### 8.4 P3 — Predictive Rendering（预判渲染）

```cpp
void DisplayModel::RenderVisibleParts() {
    int currentPage = CurrentPageNo();

    // 渲染当前页面（最高优先级）
    RequestPageRender(currentPage, PRIORITY_HIGH);

    // 预判渲染下一页（中优先级）
    if (currentPage < PageCount()) {
        RequestPageRender(currentPage + 1, PRIORITY_MED);
    }

    // 预判渲染上一页（中优先级）
    if (currentPage > 1) {
        RequestPageRender(currentPage - 1, PRIORITY_MED);
    }
}
```
```

**关键结论**：在 6 个渲染请求中，只有 **2 个（150% 和 155%）** 最终对用户可见，其余 4 个（66.7%）的渲染计算被完全浪费。
| :--- | :--- |
| **Stretch 帧占比** | 80%（Frame 1-4 中 4/5 帧为 Stretch） |
| **Direct 帧占比** | 20%（仅 Frame 0 和 Frame 5） |
| **几何偏差累计** | 从 150% → 154%，最大偏差 4% |
| **收敛时间** | 约 5 帧（~80ms，缩放停止后） |

### 6.3 结论

连续缩放期间，**绝大多数帧都在执行 Stretch**，真正命中精确缓存的比例极低。这是渲染管道设计上"允许回退"与"几何更新过快"之间的根本冲突。
旧 Bitmap
  │
  ▼
旧 Geometry (zoom=148%)
  │
  ▼
StretchBlt
  │
  ▼
新 Geometry (zoom=150%)

❌ 错误：StretchBlt 的对象已不属于当前几何
```

### 5.3 核心区分

| 场景 | StretchBlt 行为 | 结论 |
| :--- | :--- | :--- |
| 同几何缩放（如 148% → 148%） | 无缩放，恒等映射 | ✅ 正确 |
| 微小几何偏差（148% → 149%） | 极轻微拉伸，肉眼不易察觉 | ⚠️ 可接受 |
| 显著几何偏差（148% → 155%） | 明显拉伸导致压缩/锯齿 | ❌ 不可接受 |

**真正的 Bug** 不是 `StretchBlt` 本身，而是**允许对不属于当前几何的旧位图执行 StretchBlt**。
    CalcZoomReal(newZoom);
    // ... 更新布局 ...
}

// Bitmap 缓存在创建时记录当前几何版本
struct BitmapCacheEntry {
    Pixmap* bitmap;
    uint64_t geometryVersion;  // 记录该位图对应的几何版本
    int pageNo;
    // ...
};
```

### 4.4 Geometry Version 校验规则

```
Bitmap 可用于绘制 ⇔ Bitmap.geometryVersion == Layout.geometryVersion
```

而不是当前的：

```
Bitmap 可用于绘制 ⇔ Bitmap.zoom == Layout.zoom
                  && Bitmap.rotation == Layout.rotation
```

**优势**：
- 避免漏掉任何几何维度变更
- 版本号比较是 O(1) 整数比较，比浮点 zoom 比较更可靠
- 版本号递增自然形成 happens-before 关系
 │           │                                                      │
 │           ▼                                                      │
 │  renderFinishedCb.Call(&req)  (PostMessage 通知 UI)              │
 │                                                                  │
 └─────────────────────────────────────────────────────────────────┘
          ▲                                                    │
          │          同时，UI 线程已经进入了下一帧：            │
          │          Zoom=151% → 新的几何 → 新的 Stretch       │
          └──────── UI 永远领先 Render ────────────────────────┘
```

### 3.2 关键观察

**UI 永远领先 Render**——这是所有异步渲染 PDF 查看器的固有特征。SumatraPDF 当前的问题不在于"有延迟"，而在于延迟期间没有提供**几何安全的占位策略**。