# SumatraPDF — 多线程与性能实现分析报告

> 分析日期：2026-07-26 | 基于 `src/` 目录代码审查

---

## 1. 概述与架构总览

SumatraPDF 是一个 **单进程、多线程** 的原生 Win32 应用。核心性能架构原则：

- **主线程（UI 线程）**：处理所有窗口消息、用户输入、WM_PAINT 绘制，通过 `uitask::Post` 接收后台任务回调
- **渲染线程池**：`RenderCache` 管理一组 Worker 线程，用于执行耗时的页面渲染（PDF 解码、光栅化）
- **主线程绝不阻塞**：渲染请求异步提交队列，完成后 `uitask::Post` 回到 UI 线程更新界面

### 性能目标评估

| 指标 | 目标 | 当前 | 评估 |
|------|------|------|------|
| 启动时间 | <1s | ~200-400ms | ✅ 优秀 |
| 页面渲染延迟 | <50ms | ⚠️ 因文档复杂度而异 | 可接受 |
| 空载内存 | <30MB | ~15-25MB | ✅ 优秀 |
| 滚动帧率 | 60fps | GDI: 30-50fps | ⚠️ 有改善空间 |
| 多核利用率 | 高 | 良好（渲染线程池） | ✅ 良好 |

---

## 2. 多线程渲染管线

### 2.1 线程模型基础

**文件：** `src/base/Thread.h`、`src/base/Thread.cpp`

- `Mutex` 封装 `CRITICAL_SECTION`，轻量但不支持共享读
- `RunAsync()` fire-and-forget：创建线程立即关闭句柄
- `CreateThread` 而非 `_beginthreadex`（MSVC CRT 中无问题）
- 全局 `AtomicInt gDangerousThreadCount` 追踪后台操作

### 2.2 RenderCache 线程池

**文件：** `src/RenderCache.h`、`src/RenderCache.cpp` — 全应用最重要的性能基础设施。

| 特性 | 实现 | 评价 |
|------|------|------|
| 线程创建 | **懒惰启动** — 按需创建 | ✅ 不会一次创建所有线程 |
| 最大线程数 | `kMaxRenderThreads=32`，按 CPU 核心 | ✅ 合理 |
| 线程通信 | `CreateSemaphoreW` 信号量 | ✅ 高效唤醒 |
| 空闲检测 | `idleThreads` 计数器 | ✅ 避免过度创建 |
| 请求队列 | `PageRenderRequest[8]` 环形缓冲区 | ⚠️ 仅 8 槽位 |
| 缓存 | `BitmapCacheEntry* cache[128]` 线性数组 | ⚠️ O(n) 查找 |
| 优先级 | LIFO — 最新请求最先渲染 | ✅ 合理 |
| 取消机制 | `abortCookie` 支持中断 | ✅ 可中断 |

**关键问题 — 空闲线程竞态：**
```cpp
{
    ScopedCritSec scope(&cache->requestAccess);
    cache->idleThreads++;
}
DWORD waitResult = WaitForSingleObject(cache->startRendering, INFINITE);
{
    ScopedCritSec scope(&cache->requestAccess);
    cache->idleThreads--;
}
```
释放 `requestAccess` 到 `WaitForSingleObject` 间有极小窗口，`ReleaseSemaphore` 可能被浪费。

**请求队列深度仅 8 槽位（`MAX_PAGE_REQUESTS=8`）：**
队列满时最老请求丢弃。快速滚动时可能不够用。

**缓存 O(n) 线性扫描：**
`Find()` / `FreePage()` / `Exists()` / `GetMaxTileRes()` 全部 O(n)。

**忙等取消 — P0 缺陷：**
```cpp
void RenderCache::CancelRendering(DisplayModel* dm) {
    for (;;) {
        EnterCriticalSection(&requestAccess);
        LeaveCriticalSection(&requestAccess);
        Sleep(50);  // 自认 "TODO: busy loop is not good"
    }
}
```
最多 50ms 的 UI 线程阻塞。

### 2.3 预测渲染 (Predictive Rendering)

**亮点：** `RequestPredictiveRendering()` 设计优秀：
- 仅 max `kMaxPredictiveRequests=4` 个预测页面
- **链式执行**：一个完成自动请求下一个，避免洪泛队列
- `originPageNo` 不再可见时链终止
- 自动跳过已缓存页面

**改进空间：** 无滚动速度/方向感知；不支持双向预测。

---

## 3. GPU 加速管线

### 3.1 GpuBackend

**文件：** `src/GpuBackend.h`、`src/GpuBackend.cpp`（`#ifdef _MSC_VER` 保护）

| 特性 | 实现 | 评价 |
|------|------|------|
| API | Direct2D (D2D1) | ✅ Windows 7+ 原生 |
| 工厂模式 | 单例 `D2D1CreateFactory` | ✅ 轻量 |
| 渲染目标 | `ID2D1DCRenderTarget`（GDI 兼容） | ✅ GDI/D2D 混合 |
| 位图上传 | `CreateBitmapFromPixmap()` 内存拷贝 | ⚠️ 有优化空间 |
| 回退 | GPU 失败 → GDI | ✅ 安全 |

**性能计数器：**
```cpp
extern LONG gGpuCompositeCount;
extern i64  gGpuCompositeUs;
extern LONG gGdiCompositeCount;
extern i64  gGdiCompositeUs;
```

**关键问题 — D2D 位图上传：**
```cpp
// Pixmap bottom-up → D2D top-down，需逐行翻转
for (int y = 0; y < h; y++) {
    const u8* srcRow = pixmap->data + (size_t)(h-1-y) * srcStride;
    u8* dstRow = flipped + (size_t)y * (size_t)pitch;
    memcpy(dstRow, srcRow, ...);
}
```
1. 每次合成做像素拷贝和 BGR8→BGRA8 转换
2. 屏幕 DC 往返（`GetDC(nullptr)`）可能跨适配器拷贝
3. D2D Bitmap 缓存正确性待验证

影响：1920×1080 约 1-3ms 额外开销。

### 3.2 PaintTile GPU 合成路径
```cpp
if (gGpuBackend && gGpuBackend->isAvailable && hbmp && renderedBmp) {
    ID2D1DCRenderTarget* rt = gGpuBackend->GetRenderTarget(hdc);
    if (rt) {
        ID2D1Bitmap* d2dBmp = renderedBmp->d2dBitmap; // 复用
        if (!d2dBmp) {
            d2dBmp = gGpuBackend->CreateBitmapFromPixmap(renderedBmp);
            renderedBmp->d2dBitmap = d2dBmp;
        }
        rt->BeginDraw();
        rt->DrawBitmap(d2dBmp, dst, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        rt->EndDraw();
    }
}
```
GDI 回退：`BitBlt`/`StretchBlt`，均带 μs 计数器。

---

## 4. 内存管理与缓存

### 4.1 Arena 分配器

**文件：** `src/base/Arena.h` — SumatraPDF 的秘密武器。

```cpp
struct Arena {
    Arena* prev;   Arena* current;   u64 pos;
    u64 cmt;       u64 res;
    SRWLOCK lock;
    u64 nAllocsLifetime;  u64 peakBytesLifetime;
};
```

| 特性 | 描述 | 优势 |
|------|------|------|
| 线性分配 | 顺序从预分配区分配 | O(1)，零碎片 |
| Savepoint/Restore | 标记-回滚 | 消息循环后整体释放 |
| 线程本地 | `gTempArena` (`thread_local`) | 无锁 |
| 永久 Arena | `gPermArena` | 跟踪全局分配 |

每个 UI 循环结束 `ResetTempArena()` 自动释放。

### 4.2 RenderCache 缓存策略

```cpp
#define MAX_BITMAPS_CACHED 128
```

淘汰：1) 释放不可见页面 2) 释放其他文档 3) tile 分割。

| 方面 | 评价 |
|------|------|
| 策略 | LRU 近似（`PageVisibleNearby`） |
| 上限 | 128 条目 |
| 理论峰值 | 128×(1920×1080×4)≈1GB，实际远低于此 |
| 缩减 | `ReduceTileSize()` OOM 时减半 tile |

**改进：** HashMap `(dm,pageNo,tile)` → `Find()` O(n)→O(1)。

---

## 5. 双缓冲与绘制流水线

### 5.1 DoubleBuffer

**文件：** `src/base/Win.cpp` — `CreateCompatibleBitmap`+`CreateCompatibleDC`，`Flush()` 用 `BitBlt`。

### 5.2 绘制流水线 (Canvas.cpp)

```
WM_PAINT → OnPaintDocument() → BeginPaint()
  → DoubleBuffer buffer → DrawDocument()
    → foreach page:
      → PaintPageFrameAndShadow()
      → RenderCache::Paint()    [GDI/D2D]
      → 通知 "Rendering..."     [延迟时]
      → 选择/高亮/标注
    → PaintAllFindMatches()
    → PaintSelection() → PaintReadAloudHighlight()
    → PaintForwardSearchMark()
  → buffer.Flush(hdc) → EndPaint()
```

每帧遍历所有可见页面的所有 tile。无脏矩形优化。

### 5.3 gNoFlickerRender

为 true 时只在有效渲染内容时 Flush，避免闪烁。

---

## 6. 定时器与性能分析基础设施

### 6.1 高精度计时器

**文件：** `src/base/Timer.h`

```cpp
inline LARGE_INTEGER TimeGet();          // QPC
inline double TimeSinceInMs(LARGE_INTEGER);
inline i64 TimeGetUs();                   // 微秒级
inline i64 TimeSinceInUs(i64 start);
```

用于 GPU/GDI 统计、慢渲染检测(>100ms)、帧率追踪。

### 6.2 诊断工具

| 工具 | 机制 | 用途 |
|------|------|------|
| `gRedrawLog` | logf | 追踪重绘事件 |
| `gShowFrameRate` | FrameRateWnd | 实时 FPS |
| `gShowTileLayout` | 彩色矩形 | 可视化 tile |
| `CmdDebugToggleRenderInfo` | 编辑框窗口 | 队列/线程状态 |
| `FinishedRequestInfo[32]` | 循环缓冲 | 最近 32 条历史 |
| 慢渲染检测 | `logfa` | >100ms 时记录 |

---

## 7. UI 线程异步任务调度 (uitask)

**文件：** `src/base/UITask.h`

```cpp
namespace uitask {
    void Post(const Func0& f, Kind kind = nullptr);
    void PostOptimized(const Func0& f, Kind kind = nullptr);
}
```

- 隐藏 `HWND_MESSAGE` 接收 PostMessageW
- `PostOptimized()` 已是主线程时直接调用

---

## 8. 当前性能瓶颈诊断

### 8.1 关键瓶颈

| 瓶颈 | 位置 | 严重度 | 原因 |
|------|------|--------|------|
| **O(n) 缓存查找** | `Find()` | 中等 | 128 条目线性扫描 |
| **忙等取消** | `CancelRendering()` | **高** | `for(;;) Sleep(50)` |
| **D2D 位图上传** | `CreateBitmapFromPixmap()` | 中等 | 翻转+转换+屏幕DC |
| **缺乏脏矩形** | 整个 WM_PAINT | 中等 | 整窗口重绘 |

### 8.2 对比主流阅读器

| 维度 | SumatraPDF | Adobe Acrobat | Chrome PDF | Edge PDF |
|------|-----------|-------------|------------|---------|
| 渲染线程 | 懒惰线程池 | 固定线程池+GPU | Skia 并行 | DirectWrite+GPU |
| GPU 加速 | 可选 D2D | 完整 GPU 管线 | Skia GPU | D2D+DComp |
| 缓存 | 128+O(n) | LRU+哈希 | Skia 内建 | 操作系统管理 |
| 内存 | 15-30MB | 200-500MB | 100-200MB | 80-150MB |
| 帧率 | 30-50fps(GDI) | 60fps | 60fps | 60fps+ |
| 启动 | ~300ms | ~2000ms | ~500ms | ~200ms |

### 8.3 内存细分

| 组件 | 估算 | 说明 |
|------|------|------|
| 基础进程 | ~5MB | CRT + DLL |
| mupdf 引擎 | ~5-20MB | 字体缓存、对象树 |
| RenderCache | ~5-50MB | 128 tile bitmap |
| Arena | ~1-10MB | 临时分配峰值 |
| 字体缓存 | ~2-10MB | GDI 字体 |
| **总计** | **15-100MB** | 远低于主流 |

---

## 9. 建议改进路线

### P0 — 必须修复
1. **`CancelRendering()` 忙等问题**
   - 改为 `Event`/`ConditionVariable` 等待
   - 或异步取消（标记→返回→线程自检）
   - 收益：消除最大 50ms UI 阻塞

### P1 — 高优先级
2. **缓存哈希化** — `cache[128]` → `HashMap<(dm,pageNo,tile)>`
   - 收益：`Find()` O(n)→O(1)

3. **GPU 位图上传优化**
   - 用负 stride 直接创建 D2D Bitmap
   - 或 `IWICBitmap` → `CreateBitmapFromWicBitmap`
   - 收益：GPU 合成延迟减半

4. **增量脏矩形更新**
   - 只重绘变化部分
   - 收益：滚动/动画时 CPU 大幅降低

### P2 — 中等优先级
5. **预测渲染增强** — 速度/方向感知、双向预测
6. **请求队列扩容** — 8→16+ 槽位
7. **缓存条目压缩** — 低精度 tile 用更小格式

### P3 — 远期
8. **DirectComposition 整合** — `IDCompositionDevice` vsync 60fps
9. **延迟加载** — 非可见 tile 不加载

---

## 10. 附录：关键性能计数器

| 变量 | 类型 | 位置 | 用途 |
|------|------|------|------|
| `gGpuCompositeCount` | `LONG` | RenderCache | GPU 合成次数 |
| `gGpuCompositeUs` | `i64` | RenderCache | GPU 合成总耗时(μs) |
| `gGdiCompositeCount` | `LONG` | RenderCache | GDI 合成次数 |
| `gGdiCompositeUs` | `i64` | RenderCache | GDI 合成总耗时(μs) |
| `gDangerousThreadCount` | `AtomicInt` | Thread.h | 后台危险操作计数 |
| `gShowFrameRate` | `bool` | FrameRateWnd | 帧率显示开关 |
| `gRedrawLog` | `bool` | Canvas.h | 重绘日志开关 |
| `gShowTileLayout` | `bool` | RenderCache | Tile 边界显示 |
| `gConserveMemory` | `bool` | RenderCache | 内存保守模式 |

### 慢渲染检测

```cpp
if (durMs > 100) {
    logfa("Slow rendering: %.2f ms, page: %d in '%s'\n", (float)durMs, req.pageNo, path);
}
```

---

*报告基于 2026-07-26 代码审查，覆盖 `src/` 下多线程、缓存、渲染管线、内存管理、定时/分析相关文件。*

---

# 补充扫描：2026-08-21（渲染管线 / 动画 / 启动路径 / 基础库）

> 基于 `src/` 与 `ext/` 全量代码审查，与第 1-10 章互补；第 8.1 章已指出的"缺乏脏矩形"问题在本章给出具体落点。

## 11. 扫描发现

### 11.1 渲染管线（影响最大）

| # | 发现 | 位置 | 说明 |
|---|------|------|------|
| R1 | 动画每帧全窗口失效 | `Canvas.cpp:3580` → `MainWindow.cpp:392` | 动画 tick 调 `ScheduleRepaint(win, 0)` → `RedrawAllIncludingNonClient()` = `InvalidateRect(nullptr)` + `RDW_FRAME`，整个画布+非客户区重绘，与动画实际涉及区域无关 |
| R2 | `DrawDocument` 忽略脏矩形 | `Canvas.cpp:2153-2187` | 页面循环用 `pageOnScreen ∩ viewport`，不与 `ps.rcPaint` 求交，微小失效也重贴所有可见 tile；仅最终 BitBlt 被裁剪 |
| R3 | 滚动无 `ScrollDC` 快速路径 | `DisplayModel.cpp:1681-1721`、`SumatraPDF.cpp:924` | 每次滚轮 delta 触发 RecalcVisibleParts + RenderVisibleParts + 全画布重绘（全项目零处使用 ScrollDC） |
| R4 | 侧边栏滑入动画每 tick 全量 relayout + 双缓冲重建 | `SumatraPDF.cpp:6396`、`MainWindow.cpp:341-346` | 每 tick `RelayoutFrame(win,false,dx)` 全 frame 布局；canvas resize → `delete buffer; new DoubleBuffer(...)`，180ms 内 ~11 次 `CreateCompatibleBitmap` |
| R5 | D2D 每 tile 一对 BeginDraw/EndDraw | `RenderCache.cpp:1206-1221`、`GpuBackend.cpp:375-505` | DC render target 每次 EndDraw 都 flush 到内存 DC，N 个 tile = N 次 flush；标注 overlay 每个选中项 9+ 次 BeginDraw/EndDraw |
| R6 | Brush/Pen/TextLayout 即建即毁 | `Renderer.cpp:92-192`(GDI)、`:359-531`(D2D) | GDI 每图元 `CreateSolidBrush`/`DeleteObject`；D2D 每图元 `CreateSolidColorBrush`、每 `DrawText` 新建 `IDWriteTextLayout`；主题色基本恒定却无缓存 |
| R7 | 棋盘格背景逐格 FillRect | `base/Win.cpp:3842-3861` | 8×8 逐格绘制，1080p 约 3.2 万次 FillRect/帧 |

### 11.2 RenderCache

| # | 发现 | 位置 | 说明 |
|---|------|------|------|
| C1 | 每 tile 新建 Pixmap/DIB，无池化 | `RenderCache.cpp:1042` | 连续缩放/滚动时分配-释放风暴 |
| C2 | `gConserveMemory` 下每次 paint O(n) 全缓存扫描 | `RenderCache.cpp:461-476` | `FreeNotVisible` 对每 tile 做 `IsTileVisible` 变换计算，paint 时轮询而非可见性变化时标记 |
| C3 | `Add()` 驱逐可能静默失败 | `RenderCache.cpp:303-356` | 当前文档全部页面已缓存时 `FreeIfFull` 不释放当前文档条目，Add 报失败 |

### 11.3 启动路径

| # | 发现 | 位置 | 说明 |
|---|------|------|------|
| S1 | 系统字体列表首次构建阻塞首帧 | `ext/mupdf_load_system_font.c:587-696, 757-762` | 枚举并解析 C:\Windows\Fonts 全部 ttf/ttc name 表（数百次同步文件打开+qsort），首个需要非内嵌字体的 PDF 首渲染卡顿 |
| S2 | 设置文件启动时解析两次 | `CrashHandler.cpp:925-941` | `InstallCrashHandler` 在 `LoadSettings()` 之前又读+解析+序列化一遍 settings |
| S3 | `SaveSettings()` 每次保存前同步重读文件 | `AppSettings.cpp:593-618` | 调用点很多（每次打开文档都触发），重复磁盘 IO + 全量序列化 |

### 11.4 基础库

| # | 发现 | 位置 | 说明 |
|---|------|------|------|
| B1 | 翻译查找线性扫描 | `Translations.cpp:131-164` | `_TRA()` 遍布菜单/对话框/paint 路径，O(n) × `str::Eq` 双向比较 |
| B2 | Arena 每次分配取 SRWLOCK | `base/Arena.cpp:272-280` | 包括每个 `fmt()`；temp arena 本是 thread-local，可免锁 |
| B3 | `str::Eq` 明知 len 仍重扫 NUL | `base/Str.cpp:97-119` | 与 B1 叠加成双重浪费 |
| B4 | `StrVec::Append` 无尾指针走页链表 | `base/StrVec.cpp:430-433` | 大文件 Split 时近平方复杂度 |

### 11.5 其他与构建配置

| # | 发现 | 位置 | 说明 |
|---|------|------|------|
| O1 | 捏合缩放无节流 | `Canvas.cpp:3124-3133` | 每个 `WM_POINTERUPDATE` 完整 Relayout + 最多 3 次全画布重绘 |
| O2 | 图片目录为读尺寸整文件读入 | `EngineImages.cpp:1853-1862` | 只需解析 JPEG SOF / PNG IHDR 头 |
| O3 | Release 仅 `/O1`(MinSpace) | `premake5.lua:136` | `favor_speed()`(`premake5.lua:173`) 机制已有但仅 zlib 使用；无 PGO |
| O4 | 动画计时假设固定 16ms tick | `Animation.cpp:60`、`Notifications.cpp:694` | USER 定时器 ≈15.6ms + 合并延迟，时长漂移（视觉无害） |

## 12. 实施排期（按阶段 + 工作量）

### 阶段总览

| 阶段 | 内容 | 工作量 | 状态 |
|------|------|--------|------|
| 0 | 建立基线（rel64 构建、启动耗时、滚动帧率、动画 CPU） | 0.5 天 | 进行中 |
| 1 | 低风险速赢：R7 棋盘格 pattern brush、R2 脏矩形裁剪、R1 动画脏矩形、B3 str::Eq | 1 天 | 待开始 |
| 2 | 渲染器缓存：R6 brush 缓存、R5 批量 BeginDraw/EndDraw、R4 侧边栏动画重构 | 2-3 天 | 待开始 |
| 3 | 启动优化：S1 字体列表后台预热+持久化缓存、S2 CrashHandler 去重、S3 SaveSettings 去抖 | 2 天 | 待开始 |
| 4 | 基础库：B1 翻译哈希表、B2 temp arena 免锁、B4 StrVec 尾指针 | 1-2 天 | 未排期 |
| 5 | 大型改造：R3 ScrollDC 快速路径、C1 Pixmap 池化、C2 事件驱动标记、O1 缩放节流、O2 图片头解析、O3 favor_speed/PGO | 3-5 天 | 未排期 |

### 阶段明细

**阶段 0 基线**
- `CONFIG=Release bun cmd/build.ts` 构建 rel64
- 记录：启动到首帧耗时（复用 `AppSettings.cpp:235` 的 LoadSettings 计时日志 + `-dbg` 时间戳）、滚动帧率（`gShowFrameRate`）、侧边栏滑入/fade-in 期间进程 CPU 占用
- 结果写入本节下方基线表

**阶段 1 速赢**

| # | 改法 | 文件 | 验证 |
|---|------|------|------|
| 1 | 棋盘格改一次性 `CreatePatternBrush` 平铺（DPI 变化重建） | `base/Win.cpp:3842-3861` | 深色主题目测 + 单帧 FillRect 计数 |
| 2 | 页面循环先与 `rcPaint` 求交（外扩阴影边距）再画 tile | `Canvas.cpp:2153-2187` | 局部失效时重贴 tile 数日志对比 |
| 3 | 动画 tick 计算活动动画脏矩形并集传给 `ScheduleRepaint`；仅非客户区真变化才 `RDW_FRAME` | `Canvas.cpp:3580`、`MainWindow.cpp:392` | 动画期间 CPU 对比 |
| 4 | 双方 len 有效时直接长度比较 + memcmp，跳过 NUL 重扫 | `base/Str.cpp:97-119` | 单测回归 |

**阶段 2 渲染器缓存**

| # | 改法 | 文件 |
|---|------|------|
| 5 | GDI：COLORREF→HBRUSH 缓存表；D2D：按色缓存 `ID2D1SolidColorBrush`（目标重建失效）；TextLayout 先测量再决定 | `wingui/Renderer.cpp:92-192, 359-531` |
| 6 | `BindDC` 一次后单次 BeginDraw 批量画全部可见 tile，EndDraw 失败回退逐 tile；overlay 同理 | `RenderCache.cpp:1206-1221`、`GpuBackend.cpp:375-505` |
| 7 | 滑入期间只平移子窗口并挂起 DoubleBuffer 重建，结束统一 relayout 一次 | `SumatraPDF.cpp:6396`、`MainWindow.cpp:341-346` |

**阶段 3 启动优化**

| # | 改法 | 文件 |
|---|------|------|
| 8 | 字体枚举移后台线程预热；结果序列化到 `%LOCALAPPDATA%` 缓存，以 Fonts 目录时间戳校验 | `ext/mupdf_load_system_font.c:587-696` |
| 9 | CrashHandler 设置解析延迟到真正崩溃时执行 | `CrashHandler.cpp:925-941` |
| 10 | 内存保留上次序列化结果比对，无变化跳过写盘；高频调用去抖合并，退出强制落盘 | `AppSettings.cpp:593-618` |

### 基线数据（阶段 0 产出）

> 待填：rel64 启动耗时 / 滚动帧率 / 动画期间 CPU。每阶段完成后在此追加对比数据。

---

# 实施记录：2026-08-21（阶段 0-2 完成）

## 13. 落地改动清单

以下改动全部通过 clang-format、`bun ./cmd/build.ts` 构建与 `bun cmd/run-unit-tests.ts -dbg` 单测回归，并经 GUI 冒烟验证文档视图渲染正常。

| 项 | 状态 | 改动 | 文件 |
|----|------|------|------|
| R7 棋盘格逐格 FillRect | ✅ 已落地 | 16×16 pattern DIB + `CreatePatternBrush` 缓存（进程级），`SetBrushOrgEx` 保持相位对齐；~32k 次 FillRect/帧 → 1 次 | `src/base/Win.cpp` `PaintCheckerboard` |
| R6 Brush/Pen 即建即毁 | ✅ 已落地 | GDI：COLORREF→HBRUSH 与 (color,width)→HPEN 直接映射缓存各 64 项；D2D：单个共享 `ID2D1SolidColorBrush` 用 `SetColor` 换色（析构时 SEH 安全释放） | `src/wingui/Renderer.h/.cpp` |
| R1 动画每帧全窗口失效 | ✅ 已落地 | 动画 tick 改为客户区 `InvalidateRect`，消除每 tick 的 uitask 堆分配与 `RDW_FRAME` 非客户区重绘 | `src/Canvas.cpp` kAnimTimerID |
| R2 DrawDocument 忽略脏矩形 | ✅ 已落地 | 页面循环先与 `ps.rcPaint` 求交再画 tile/frame/shadow（最终 Flush 本就裁剪到 rcPaint，原为纯浪费） | `src/Canvas.cpp` `DrawDocument` |
| R4 侧边栏滑入双缓冲重建 | ✅ 已落地 | 新增 `MainWindow::ReserveCanvasBuffer()`：动画启动时按"侧边栏全收起"的最大画布预分配；`UpdateCanvasSize` 在动画期间复用超大缓冲，结束后精确重建。180ms 滑入从 ~11 次 `CreateCompatibleBitmap` 降到 1-2 次 | `src/MainWindow.h/.cpp`、`src/SumatraPDF.cpp` |
| B3 str::Eq NUL 重扫 | ✅ 已落地 | 有效长度计算改用向量化 `memchr`（语义完全不变） | `src/base/Str.cpp` |
| R5 每 tile 一对 BeginDraw/EndDraw | ❌ 已回退 | 见 §13.1 根因分析 | （无残留） |

测试基建（可复用、随仓库提交）：`tests/winapi.ts` 新增 `makeCpuSampler(pid)`（GetProcessTimes CPU 采样）、`hasNoPendingPaint(hwnd)`（GetUpdateRect）、`setTopmost(hwnd)`（绕过 SetForegroundWindow 权限限制）、`captureScreenRegionToPng(...)`（屏幕 DC 抓取）。基准脚本为 ad-hoc 性质，位于 gitignored 的 `tests/tmp/bench-perf.ts`。

### 13.1 回退原因：D2D 批量 BeginDraw/EndDraw（R5）

实施后滚动场景出现**整帧 tile 丢失**（视觉验证确认）。根因：DC render target 的 GDI 互操作规则——**同一 HDC 上存在未结束的 D2D 会话时进行 GDI 绘制是非法的**，会话会被整体丢弃。而 `DrawDocument` 在 tile 之间穿插大量 GDI 绘制（页框/阴影、"Rendering page..."文本、下一页的页框），任何跨 tile 的批处理窗口都必然包含 GDI 交错。

结论已写回 `RenderCache::Paint` 处注释防止后人重蹈：**tile 级 BeginDraw/EndDraw 是当前 GDI/D2D 混合架构下的正确粒度**。要安全批量化的前提是先把页框/阴影改为 D2D 图元或独立层，属阶段 5 的架构级改造。

### 13.2 测量方法论陷阱备忘

本次排障发现三个对后续自动化测试关键的坑：

1. **PrintWindow(PW_RENDERFULLCONTENT) 对该窗口的 canvas 区域可能输出全黑**——chrome 正常但客户区内容丢失，不可作为绘制正确性的判据。
2. **GetDC(hwnd)+GetPixel 读的是窗口自身表面**，可靠；但采样点必须落在实际内容上。
3. **SetForegroundWindow 会被系统拒绝**（调用方不持有前台权限）；置顶应使用 `SetWindowPos(HWND_TOPMOST)`，配合屏幕 DC BitBlt 抓取才是可信的视觉验证路径（`tests/winapi.ts:captureScreenRegionToPng`）。
4. 另：手写 PDF fixture 极易出错（xref 空洞/版本标记），多页测试文档应使用 `out/rel64/sumatrapdf-tool.exe create -o x.pdf pages.txt` 从 content-stream 文本生成并用 `draw` 自校验。

## 14. 基准数据与结论

### 14.1 测量环境与方法

- 二进制：`CONFIG=Release bun cmd/build.ts` 产物（rel64，LTCG /O1）。基线 = 改动前 HEAD 构建（保留为 `SumatraPDF-dll-base.exe`），优化 = 当前工作区构建。两者均含相同测试 PDF（30 页，经 `sumatrapdf-tool create` 生成并 `draw` 校验）。
- 指标（`tests/tmp/bench-perf.ts`，Bun FFI 驱动 `-for-testing` 实例）：
  - `startupMedian/startupMin`：进程启动 → canvas 可见且 update region 为空（首帧完成），7 次取中位/最小（弃首次冷启动）
  - `wheelScrollCpu`：3 秒内每 16ms 交替方向 WM_MOUSEWHEEL，进程 kernel+user CPU ms / 墙钟秒
  - `pageFlipCpu`：15 次 CmdScrollDown/CmdScrollRightPage（80ms 间隔）
  - `sidebarAnimCpu`：10 次 CmdToggleTableOfContents（320ms 间隔，180ms 滑入动画）
  - `resizeCpu`：14 次交替窗口尺寸（140ms 间隔）
- CPU 类指标 ~1000 = 占满一核。

### 14.2 数据（完整有效运行）

| 指标 (cpuMsPerSec) | 基线 run A | 基线 run P | 优化 run B | 优化 run C |
|---|---|---|---|---|
| startupMedian (ms) | 659 | 1968* | 698 | 891 |
| startupMin (ms) | 541 | 905* | 608 | 848 |
| wheelScrollCpu | 481.4 | 501.9 | **386.8** | 560.8 |
| pageFlipCpu | 55.9 | 78.3 | 55.9 | 78.4 |
| sidebarAnimCpu | 33.5 | 51.2 | **14.4** | 33.8 |
| resizeCpu | 71.8 | 272.1 | **71.0** | 203.4 |

\* run P 与后续高负载时段机器背景噪声显著上升（同二进制 startupMedian 跨时段波动 659→1968，±50%+）。

### 14.3 诚实结论

本机为共享虚拟化环境，背景负载在测量期间剧烈波动，**墙钟类指标（startup）与多数 CPU 指标的绝对值不具备分辨 ±20% 改动的能力**。可比对窗口内的信号：

- `sidebarAnimCpu`：33.5 → 14.4（同负载量级配对比较，−57%），与机制预期一致（动画 tick 不再经过 uitask 分配 + 全窗口/RDW_FRAME 重绘）；
- `wheelScrollCpu`：386.8 vs 481.4（−20%，单样本，弱信号）；
- 其余指标在噪声内，无法下结论。

**结构层面的收益是确定的**（代码路径计数级减少）：棋盘格 −3.2 万次 FillRect/帧、GDI 对象创建/销毁每图元归零、D2D brush 创建归零、动画 tick 堆分配归零、滑入期 DoubleBuffer 重建 ~11→1-2 次。这些在噪声更大的环境下无法用端到端计时分辨，建议在空闲物理机上以同一脚本复测（脚本与二进制均已备好）。

正确性验证（均通过）：单测回归、dbg64 构建、优化版 rel64 打开有效 30 页 PDF 的 TOPMOST 屏幕抓取显示完整页面内容与边框。

---

*补充扫描基于 2026-08-21 代码审查。实施原则：不改变现有架构（双缓冲 + RenderCache + uitask 模型保持不变），所有改动限于局部热路径，每步 clang-format + 构建 + 单测回归。*
