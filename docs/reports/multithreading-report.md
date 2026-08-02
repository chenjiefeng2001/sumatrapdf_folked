# 多线程与异步并发架构及性能优化报告（修订版 v6 — 生产级加固版）

## 1. 概述

SumatraPDF 采用基于后台工作线程池（`RenderCache`）和异步 UI 任务队列（`uitask`）的并发架构，实现了界面的高帧率滚动与标注的平滑编辑。

本报告详细阐述该并发架构的微观实现、核心锁层级安全边界，剖析系统中存在的死锁隐患、高延迟操作阻塞主线程等并发缺陷，并记录本版本（v6）已落地的全部修复措施及 v5 评审发现的 3 个关键技术漏洞的加固方案。

## 2. 线程池与异步任务模型

```
                      ┌────────────────┐
                      │  主 UI 线程    │◄────────────────────────┐
                      └───────┬────────┘                         │
                              │ (1) RequestRendering             │ (3) renderFinishedCb
                              ▼                                  │     (PostMessage)
                      ┌────────────────┐                         │
                      │  RenderCache   ├──────────────┐          │
                      └────────────────┘              │          │
                                                      ▼          │
                                             ┌─────────────────┐ │
                                             │ 渲染线程 (1...N) ├─┘
                                             │  RenderPage()   │
                                             └─────────────────┘
```

### 2.1 主 UI 线程

负责所有 Win32 窗口消息处理（`WndProc`）、用户交互响应（鼠标点击、滚轮缩放、标注创建与拖拽）及窗口绘制合成。当检测到页面可见区域发生变化或标注编辑完成需要重绘时，向 `RenderCache` 异步提交渲染请求。

### 2.2 RenderCache 线程池（懒加载机制）

由 `RenderCache` 类管理后台线程生命周期。任务队列采用 **LIFO（后进先出）调度策略**，确保优先渲染最新呈现在可视区内的页面。通过 `PageVisibleNearby` 机制自动过滤并废弃已被快速滚出可视区的过期请求。

**关键参数：**
- 最大线程数：`max(gMaxRenderThreads, CPU核心数)`，上限 `kMaxRenderThreads = 8`
- 请求队列容量：`MAX_PAGE_REQUESTS = 8`
- Bitmap 缓存容量：`MAX_BITMAPS_CACHED = 128`
- 线程创建策略：仅在队列有积压任务且无空闲线程时懒加载创建
- 同步唤醒：使用 `CreateSemaphoreW` 信号量进行跨线程同步

### 2.3 渲染线程核心工作循环

```cpp
static DWORD WINAPI RenderCacheThread(LPVOID data) {
    CreateTempArena();
    for (;;) {
        if (cache->ClearCurrentRequest(threadIdx)) {
            cache->idleThreads++;
            WaitForSingleObject(cache->startRendering, INFINITE);
            cache->idleThreads--;
        }
        if (AtomicBoolGet(&cache->shouldExit)) break;

        if (!cache->GetNextRequest(&req, threadIdx)) continue;

        if (!req.dm->PageVisibleNearby(req.pageNo)
            && !req.renderFinishedCb.IsValid()) continue;
        if (AtomicBoolGet(&req.dm->pauseRendering)) continue;

        EngineBase* engine = req.dm->GetEngine();
        MaskFpExceptions();
        bmp = engine->RenderPage(args);

        if (AtomicBoolGet(&req.abort)) { FreePixmap(bmp); continue; }
        cache->Add(req, bmp);
        req.renderFinishedCb.Call(&req);
        ResetTempArena();
    }
    DestroyTempArena();
}
```

## 3. 多线程安全锁体系

### 3.1 核心锁结构

| 锁名称 | 类型 | 保护目标与应用场景 |
| :--- | :--- | :--- |
| **`pagesLock`** | `CRITICAL_SECTION` | 保护 `FzPageInfo` 数组和页面关联的 `annotations`/`widgets` 列表 |
| **`docLock`** | `SRWLOCK` | 保护 MuPDF `pdf_document` 树（Shared=并发只读，Exclusive=标注/表单写入） |
| **`renderLock`** | `CRITICAL_SECTION` | 保护 `FzPageInfo` 内部缓存的 `displayList` 构建与回放逻辑 |
| **`cacheAccess`** | `CRITICAL_SECTION` | 保护 `RenderCache` 的 Bitmap 缓存及引用计数状态 |
| **`requestAccess`** | `CRITICAL_SECTION` | 保护工作队列 `requests[]`、当前执行列表 `curReqs[]` 及空闲线程计数 |

### 3.2 全局锁层级规范（v5 强制约束）

```
pagesLock → docLock → renderLock （单向，严禁逆序）
```

| 路径 | 锁链 | 说明 |
| :--- | :--- | :--- |
| 渲染（GetFzPageInfo） | `pagesLock → docLock [Shared] → renderLock` | 批量标注加载，一次共享锁 |
| UI 编辑（MarkNotificationAsModified） | `pagesLock → docLock [Exclusive] → (docLock释放) → renderLock` | 非嵌套 |
| 只读读取（GetPropertyTemp） | `docLock [Shared]` | 无需 pagesLock 或 renderLock |

## 4. 已知多线程缺陷与修复记录（v5）

### 4.1 ✅ 缺陷 1：SRWLOCK 非递归自死锁（已修复）

**文件**：`src/EngineMupdf.cpp`

**根因**：Windows `SRWLOCK` 严格非递归。`EngineMupdf::Load` 在持有 `docLock [Exclusive]` 时，调用 `IsLinearizedFile`，后者内部再次获取 `docLock [Exclusive]`。同一线程第二次获取独占锁，操作系统将其永久挂起。

**修复**：重命名为 `IsLinearizedFileLocked`，移除内部锁获取，调用者 `Load` 已持有 `docLock`。

```cpp
// 修复前
static bool IsLinearizedFile(EngineMupdf* e) {
    ScopedSRWLockExclusive lock(&e->docLock); // ← 重入死锁！
    return pdf_doc_was_linearized(ctx, pdfdoc);
}

// 修复后
static bool IsLinearizedFileLocked(EngineMupdf* e) {
    // 调用者必须已持有 docLock —— 复用锁上下文，不再重新获取
    return pdf_doc_was_linearized(ctx, pdfdoc);
}
```

### 4.2 ✅ 缺陷 2：pagesLock ↔ docLock 循环等待死锁（已修复）

**文件**：`src/EngineMupdf.cpp`

**根因**：
```
UI 线程（LoadDocumentFinish）：pagesLock (持有) → 等待 docLock [Exclusive]（GetPropertyTemp）
后台线程（ExtractTextLazy）：docLock [Shared] (持有) → 等待 pagesLock（ScopedCritSec 阻塞缓存）
                                                          ↻ 环形死锁
```

**修复方案**（双重修复）：

1. **`ExtractTextLazy` 非阻塞缓存**：使用 `TryEnterCriticalSection` 替代 `ScopedCritSec`，无法获取 `pagesLock` 时放弃本次缓存，下次渲染重新提取。

```cpp
// EngineMupdf.cpp:3677 — v5
if (TryEnterCriticalSection(&engine->pagesLock)) {
    pageInfo->stextPage = stext;
    pageInfo->textExtracted = true;
    LeaveCriticalSection(&engine->pagesLock);
} else {
    fz_drop_stext_page(ctx, stext); // 放弃结果，下次重新提取
    return;
}
```

2. **`GetPropertyTemp` 锁降级**：从 `ScopedCritSec(&docLock)`（CRITICAL_SECTION）降级为 `ScopedSRWLockShared(&docLock)`，避免阻塞持有 Shared 锁的后台渲染线程。

```cpp
// EngineMupdf.cpp:4459 — v5
TempStr EngineMupdf::GetPropertyTemp(Str name) {
    auto ctx = Ctx();
    ScopedSRWLockShared ctxScope(&docLock); // 只读：共享锁
    // ...
}
```

### 4.3 ✅ 缺陷 3：D2D Use-After-Free（cachedRT 覆盖 — 已修复）

**文件**：`src/GpuBackend.cpp`

**根因**：`CreateBitmapFromPixmap` 内部调用 `GetRenderTarget(screenDC)`，释放并覆盖了 `PaintTile` 调用者持有的 `cachedRT`。调用者随后在已释放的 RT 上执行 `BeginDraw`/`DrawBitmap` → 崩溃。

**修复**：将 `CreateBitmapFromPixmap` 改为接收外部传入的 `ID2D1DCRenderTarget* rt`，不再内部调用 `GetRenderTarget`。

### 4.4 ✅ 缺陷 4：D2D 跨线程释放（已修复）

**文件**：`src/RenderCache.cpp`

**根因**：`BitmapCacheEntry` 析构时在后台渲染线程释放 `ID2D1Bitmap`，违反 D2D 线程关联性。

**修复**：引入 `QueueSafeD2dRelease` / `FlushSafeD2dReleases` 延迟释放机制。在 `PaintTile` 入口（UI 线程）排空延迟释放队列。

**v6 加固**：初版 `Vec<IUnknown*>::Append` 在多个渲染工作线程并发入队时无锁保护，导致堆破坏。v6 引入 `CRITICAL_SECTION` + **"移动-释放"模式**——临界区内仅交换指针，实际 `Release` 在锁外执行，规避锁嵌套。

```cpp
// RenderCache.cpp — v6 生产级加固
static Vec<IUnknown*> gDeferredD2dReleases;
static CRITICAL_SECTION gDeferredD2dCS;

static void QueueSafeD2dRelease(IUnknown* obj) {
    if (!obj) return;
    EnterCriticalSection(&gDeferredD2dCS);
    gDeferredD2dReleases.Append(obj);     // ✅ 互斥锁保护，渲染线程安全入队
    LeaveCriticalSection(&gDeferredD2dCS);
}

void FlushSafeD2dReleases() {
    Vec<IUnknown*> toRelease;

    // 快速提取并清空队列，减少锁持有时间
    EnterCriticalSection(&gDeferredD2dCS);
    toRelease = gDeferredD2dReleases;
    gDeferredD2dReleases.Reset();
    LeaveCriticalSection(&gDeferredD2dCS);

    // 临界区外执行真实的 D2D 资源 Release
    for (auto* obj : toRelease) {
        obj->Release();
    }
}

// PaintTile 入口处 — UI 线程上执行实际 Release
int RenderCache::PaintTile(...) {
    FlushSafeD2dReleases(); // ← UI 线程
    // ...
}
```

### 4.5 ⚠️ 缺陷 5：标注修改后未调用 Invalidate（部分修复）

**根因**：`MarkNotificationAsModified` 修改标注后丢弃了 `displayList`，但未使 `RenderCache` 中的位图缓存失效。下次 `PaintTile` 的 `Find()` 命中旧缓存，渲染不含最新标注的页面。

**状态**：⚠️ 引擎层修复已完成（`displayList` 已丢弃），`Invalidate()` 调用需在 UI 层（`Canvas.cpp` / `DisplayModel.cpp`）实现。

**v6 推荐方案 — Canvas.cpp 标注保存通路集成**：

```cpp
// Canvas.cpp — 标注修改/添加/删除完成事件
void OnAnnotationModified(Canvas* win, Annotation* annot, AnnotationChange change) {
    EngineBase* engine = win->dm->GetEngine();
    if (!engine || !annot) return;

    // 1. 提交底层修改——丢弃过时的 displayList
    engine->MarkNotificationAsModified(annot, change);

    // 2. 显式使 RenderCache 中的位图缓存失效
    extern RenderCache* gRenderCache;
    if (gRenderCache) {
        RectF fullPage = engine->PageMediabox(annot->pageNo);
        gRenderCache->Invalidate(win->dm, annot->pageNo, fullPage);
        // 🔄 fz_cookie.abort 被 Invalidate 内部置位，渲染线程安全中止
    }

    // 3. 触发重绘
    win->RepaintAsync();
}
```

### 4.6 ✅ 缺陷 6：ExtractTextLazy 文本解析结果无谓丢弃（v6 已优化）

**文件**：`src/EngineMupdf.cpp`

**根因**：v5 修复中，当 `TryEnterCriticalSection(&pagesLock)` 失败时，直接调用 `fz_drop_stext_page` 销毁文本提取结果。数百毫秒的计算成果被丢弃，下次渲染需重新提取，形成 CPU 恶性循环。

**修复（v6 优化）**：通过 `uitask::Post` 将缓存操作异步派发给 UI 线程：

```cpp
// EngineMupdf.cpp — v6 优化
if (TryEnterCriticalSection(&engine->pagesLock)) {
    pageInfo->stextPage = stext;
    pageInfo->textExtracted = true;
    LeaveCriticalSection(&engine->pagesLock);
} else {
    // v6: 不丢弃 stext，通过 uitask 异步派发给 UI 线程安全挂载
    uitask::Post([engine, pageNo, stext, ctx]() {
        ScopedCritSec cs(&engine->pagesLock);
        FzPageInfo* pageInfo = engine->pages[pageNo - 1];
        if (pageInfo && !pageInfo->textExtracted) {
            pageInfo->stextPage = stext;
            pageInfo->textExtracted = true;
        } else {
            fz_drop_stext_page(ctx, stext);
        }
    });
    return;
}
```

### 4.7 ✅ 缺陷 7：调试断言逻辑倒置（v6 已修正）

**根因**：v5 中的 `ReportIf(IsRenderLockHeld() && !IsDocLockHeld())` 逻辑错误——持有 `renderLock` 而不持有 `docLock` 是合法场景（如纯 D2D 回放）。真正的死锁风险是在持有 `renderLock` 时尝试获取 `docLock`。

**修复（v6 修正）**：将断言放置在获取 `docLock` 的入口点：

```cpp
// ScopedSRWLockShared/Exclusive 构造函数中：
#ifdef DEBUG
    // 严禁在持有 renderLock 时获取 docLock
    ReportIf(IsRenderLockHeldByCurrentThread());
#endif
```

## 5. 🔑 锁层级重整证明

### 5.1 无死锁证明

v5 强制 `pagesLock → docLock → renderLock` 单向加锁：

- **渲染线程**（路径 A）：`pagesLock [CS] → docLock [Shared] → renderLock [CS]`
- **UI 线程**（路径 B）：`pagesLock [CS] → docLock [Exclusive] → (释放) → renderLock [CS]`
- **第三方只读**（路径 C）：`docLock [Shared] → renderLock [CS]`

**环路检测**：所有路径从左向右。路径 A 持有 `docLock [Shared]` 时请求 `renderLock`；路径 B 若想获取 `renderLock` 必须通过 `docLock [Exclusive]`，但被路径 A 的 Shared 锁阻塞。路径 C 遵守 `docLock → renderLock`。有向加锁图无环。

### 5.2 批量加锁优化

`GetFzPageInfo` 循环加载页面标注时，由每次 `MakeAnnotationWrapper` 内部分别获取 `docLock [Exclusive]`（$N$ 次），优化为在循环外一次性获取 `docLock [Shared]`（1 次共享锁）。显著降低锁竞争——标注密集型页面初始化性能提升显著。

## 6. 重构路线图

### 6.1 ✅ v6 已落地修复（含 v5 评审加固）

| 修复项 | 文件 | 优先级 | 说明 |
| :--- | :--- | :--- | :--- |
| SRWLOCK 非递归自死锁 | `EngineMupdf.cpp` | 🔴 最高 | `IsLinearizedFile` → `IsLinearizedFileLocked` |
| pagesLock↔docLock 环死锁 | `EngineMupdf.cpp` | 🔴 最高 | TryEnter 非阻塞缓存 + GetPropertyTemp 锁降级 |
| D2D Use-After-Free | `GpuBackend.cpp` | 🔴 最高 | CreateBitmapFromPixmap 参数化 rt |
| D2D 跨线程 Release | `RenderCache.cpp` | 🔴 最高 | 延迟释放队列 + CRITICAL_SECTION 保护 + "移动-释放"模式（v6 加固） |
| ExtractTextLazy uitask 优化 | `EngineMupdf.cpp` | 🟡 中 | 不丢弃 stext，异步派发给 UI 线程挂载（v6 新增） |
| 调试断言反方向修正 | — | 🟢 低 | `IsRenderLockHeldByCurrentThread()` 替代（v6 修正） |
| 标注修改后 Invalidate | `Canvas.cpp` | 🟡 中 | UI 层集成方案已设计（待落地） |

### 6.2 💡 待落地优化

| 优化项 | 目标 | 难度 |
| :--- | :--- | :--- |
| `Invalidate()` UI 层最终调用 | 消除标注修改后的过时缓存 | 低 |
| `gNoFlickerRender` 强制刷新保护 | 消除缓冲区残留 | 低 |
| `renderLock` → `SRWLOCK` | 多页并发回放 | 中 |
| `pagesLock` 细粒度化 | 降低竞争密度 | 中 |
| 同步排空与汇合模式（Drain & Join） | 消除 EngineBase 悬垂指针 | 高 |

## 7. 参考

- RenderCache.h: `RenderCache` 类、`PageRenderRequest` 结构体
- RenderCache.cpp: `RenderCacheThread()`、`QueueSafeD2dRelease()`、`FlushSafeD2dReleases()`
- EngineMupdf.h: 锁定义、`FzPageInfo` 结构体
- EngineMupdf.cpp: `GetFzPageInfo()`、`RenderPage()`、`MarkNotificationAsModified()`、`IsLinearizedFileLocked()`、`ExtractTextLazy()`
- GpuBackend.cpp: `CreateBitmapFromPixmap()`、`GetRenderTarget()`
- Canvas.cpp: `OnPaintDocument()`、`DrawDocument()`
- base/ScopedWin.h: `ScopedCritSec`、`ScopedSRWLockShared`、`ScopedSRWLockExclusive`
- docs/reports/annot-render-crash-analysis.md: 标注导致页面崩溃与卡死分析报告（修订版 v6）
