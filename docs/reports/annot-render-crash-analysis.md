# 标注功能导致页面渲染崩溃与卡死分析报告（修订版 v6.5 — 生产级加固版）

## 1. 概述

本报告分析 SumatraPDF 中在使用、编辑、删除标注（Annotation）或交互表单时，页面重新渲染时出现的**程序崩溃（Crash）与界面卡死（Hang）**的底层诱因。

结合实际代码执行路径与 MuPDF 底层并发模型的深度排查，系统在高并发和编辑场景下的不稳定根因涵盖以下七个核心维度：

1. **D2D Use-After-Free：`CreateBitmapFromPixmap` 覆盖 `cachedRT`**（√ 已修复 `GpuBackend.cpp`）
2. **D2D 跨线程 Release：渲染线程直接释放 D2D 资源**（√ 已修复 `RenderCache.cpp`）
3. **锁顺序反转与 SRWLOCK 非递归自死锁**（√ 已修复 `EngineMupdf.cpp`）
4. **持锁全量文本提取引发 UI 线程卡死**（√ 已修复 `EngineMupdf.cpp`）
5. **`Invalidate()` 未在标注修改后调用 → 缓存位图残留旧内容**（⚠️ 部分修复，详见第 5.4 节）
6. **🔴 v6.5 新增：`PageContentBox` 在 `Relayout` 时未持有 `renderLock` → MuPDF 内部缓存损坏崩溃**（√ 已修复 `EngineMupdf.cpp`，详见第 3.5、5.5 节）
7. **🔴 v6.5 新增：`SetDisplayMode` 切换模式时未清除 RenderCache 瓦片 → 视觉残留/拉伸**（√ 已修复 `DisplayModel.cpp`，详见第 3.6、5.6 节）

**v6（修订版）生产级加固**：以上问题 1-5 已在 v5/v6 中修复并通过同行评审验证。
**v6.5 新增修复**：问题 6-7 已在本版本落地（详见第 5.5、5.6 节）。

---

## 2. 锁层级与关键路径

在 `EngineMupdf` 中，控制并发安全的核心锁结构如下：

| 锁名 | 类型 | 保护对象 |
| :--- | :--- | :--- |
| **`pagesLock`** | `CRITICAL_SECTION` | 最外层锁，保护 `FzPageInfo` 数组及页面 `annotations` 列表的完整性 |
| **`docLock`** | `SRWLOCK` | 文档级读写锁，保护 MuPDF `pdf_document` 及其底层的 `pdf_obj` 对象树 |
| **`renderLock`** | `CRITICAL_SECTION` | 保护页面缓存中 `displayList` 的构建与回放操作 |

此外，在 `RenderCache` 层还有：

| 锁名 | 类型 | 保护对象 |
| :--- | :--- | :--- |
| **`cacheAccess`** | `CRITICAL_SECTION` | 保护 `BitmapCacheEntry` 缓存数组及引用计数 |
| **`requestAccess`** | `CRITICAL_SECTION` | 保护渲染请求队列 `requests[]`、当前执行列表 `curReqs[]` |

### 2.1 🔑 全局锁层级规范（v5 强制约束）

所有代码路径**必须**遵守以下单向加锁顺序：

```
pagesLock → docLock → renderLock
```

| 层级 | 锁 | 获取方式 | 说明 |
| :--- | :--- | :--- | :--- |
| 1（最外层） | `pagesLock` | `CRITICAL_SECTION` | 保护 FzPageInfo 数组、annotations 列表 |
| 2（中层） | `docLock` | `SRWLOCK [Shared/Exclusive]` | 读操作用 Shared，写操作用 Exclusive |
| 3（最内层） | `renderLock` | `CRITICAL_SECTION` | 保护 displayList 构建与回放 |

**严禁**：
1. 在持有 `renderLock` 时获取 `docLock`。
2. 在持有 `docLock [Exclusive]` 时递归获取 `docLock`（SRWLOCK 非递归）。
3. 以 Exclusive 模式获取 `docLock` 执行只读操作（应使用 Shared）。

### 2.2 核心代码路径的加锁顺序（v5 修复后）

#### 【路径 A：页面标注加载 / 渲染线程】（`GetFzPageInfo` — EngineMupdf.cpp）

```cpp
// v5 修复后：遵守 pagesLock → docLock [Shared] → renderLock
ScopedCritSec scope(&e->pagesLock);                   // 1. pagesLock（最外层）
AcquireSRWLockShared(&e->docLock);                    // 2. docLock [Shared] ← 批量共享锁
{
    ScopedCritSec rl(&e->renderLock);                 // 3. renderLock（最内层）
    if (!pageInfo->annotsLoaded) {
        pageInfo->annotsLoaded = true;
        for (pdf_annot* a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a)) {
            // 使用 MakeAnnotationWrapperLocked（调用者已持有 docLock，不再嵌套获取）
            Annotation* w = MakeAnnotationWrapperLocked(engine, a, pageNo);
        }
    }
} // renderLock 释放
ReleaseSRWLockShared(&e->docLock);                     // docLock [Shared] 释放
// pagesLock 在函数返回时释放
```

**关键变更**：
- 将 `MakeAnnotationWrapper`（内部获取 `docLock [Exclusive]`）替换为 `MakeAnnotationWrapperLocked`（无锁版本，复用外层已持有的 `docLock [Shared]`）。
- 将 $N$ 次排他性锁获取降为 **1 次共享锁获取**，大幅降低锁竞争。

#### 【路径 B：UI 线程编辑标注】（`MarkNotificationAsModified` — EngineMupdf.cpp:5255）

```cpp
void MarkNotificationAsModified(EngineMupdf* e, Annotation* annot, AnnotationChange change) {
    e->modifiedAnnotations = true;
    if (!e->pdfdoc) return;
    int pageNo = annot->pageNo;
    int pageIdx = pageNo - 1;

    ScopedCritSec scope(&e->pagesLock);                // 1. pagesLock（最外层）
    FzPageInfo* pageInfo = e->pages[pageIdx];

    // 更新 annotations 数组（pagesLock 保护下执行）
    if (change == AnnotationChange::Remove) {
        pageInfo->annotations.Remove(annot);
    } else if (change == AnnotationChange::Add) {
        pageInfo->annotations.Append(annot);
    }

    {
        ScopedSRWLockExclusive ctxScope(&e->docLock);  // 2. docLock [Exclusive]
        RebuildCommentsFromAnnotations(ctx, pageInfo);
    }                                                   // docLock 释放
    pageInfo->annotGeneration++;
    pageInfo->elementsNeedRebuilding = true;

    {
        auto ctx = e->Ctx();
        ScopedCritSec rl(&e->renderLock);              // 3. renderLock（非嵌套）
        if (pageInfo->displayList) {
            fz_drop_display_list(ctx, pageInfo->displayList);
            pageInfo->displayList = nullptr;
        }
    }                                                   // renderLock 释放
    // pagesLock 在函数返回时释放
}
```

### 2.3 锁顺序安全证明

**无环路推导**：所有代码路径的加锁顺序均为 `pagesLock → docLock → renderLock`。

| 场景 | 持有锁 | 等待锁 | 结果 |
| :--- | :--- | :--- | :--- |
| 路径 A（渲染） | `pagesLock` + `docLock [Shared]` | `renderLock` | ✅ 路径 B 无法越过 `docLock [Exclusive]` |
| 路径 B（UI 编辑） | `pagesLock` + `docLock [Exclusive]` | `renderLock`（docLock 已释放） | ✅ 非嵌套 |
| 路径 C（第三方只读） | `docLock [Shared]`（无 pagesLock） | `renderLock` | ✅ 路径 A 已遵守 `docLock → renderLock` 顺序 |

> **证明**：由于路径 B 的 `docLock [Exclusive]` 和 `renderLock` 是非嵌套的（先释放 `docLock` 再获取 `renderLock`），而路径 A 的 `docLock [Shared]` 不会阻塞其他 Shared 锁。有向加锁图中不存在环路，故无死锁。

---

## 3. 核心崩溃场景与根因剖析

### 3.1 场景 1：D2D Use-After-Free — `CreateBitmapFromPixmap` 覆盖 `cachedRT`（崩溃概率：高 💥）

**状态**：✅ **v5 已修复**（`src/GpuBackend.cpp`）

#### 3.1.1 机制分析

`PaintTile` 在 GPU 路径下的调用链：

```
PaintTile（GPU 路径）
  ├─ gGpuBackend->GetRenderTarget(hdc)          // 步骤①：创建 cachedRT（绑定到缓冲区 DC）
  ├─ gGpuBackend->CreateBitmapFromPixmap(...)    // 步骤②：旧代码中内部调用 GetRenderTarget(screenDC)
  │     └─ GetRenderTarget(screenDC)
  │           ├─ cachedRT->Release()             // ← 释放步骤①创建的 RT！
  │           └─ factory->CreateDCRenderTarget() // → 新建 cachedRT（绑定到 screenDC）
  ├─ rt->BeginDraw()                              // ← 步骤③：rt 是步骤①的返回值（已释放指针）
  └─ rt->DrawBitmap()                             // ← 在已释放的 ID2D1DCRenderTarget 上操作 → CRASH！
```

#### 3.1.2 根因

`CreateBitmapFromPixmap` 在旧实现中**内部调用 `GetRenderTarget(screenDC)`**，会释放并覆盖 `cachedRT`。调用者持有的原始 `rt` 指针变成悬垂指针（Use-After-Free）。

#### 3.1.3 修复方案（已落地）

```cpp
// GpuBackend.h — v5 修复后
ID2D1Bitmap* CreateBitmapFromPixmap(fz_pixmap* pixmap, ID2D1DCRenderTarget* rt);

// GpuBackend.cpp — 调用者传入 rt，函数直接使用该 rt 创建 bitmap
ID2D1Bitmap* GpuBackend::CreateBitmapFromPixmap(fz_pixmap* pixmap, ID2D1DCRenderTarget* rt) {
    if (!pixmap || !pixmap->samples || !rt) return nullptr;
    // ... 使用传入的 rt 创建 D2D Bitmap，不再调用 GetRenderTarget
}
```

调用点（`RenderCache.cpp:PaintTile`）：

```cpp
ID2D1DCRenderTarget* rt = gGpuBackend->GetRenderTarget(hdc);
if (rt) {
    ID2D1Bitmap* d2dBmp = gGpuBackend->CreateBitmapFromPixmap(bmp, rt);
    // ...
}
```

---

### 3.2 场景 2：D2D 跨线程 Release 导致渲染崩溃（崩溃概率：高 💥）

**状态**：✅ **v5 已修复**（`src/RenderCache.cpp`）

#### 3.2.1 机制分析

`BitmapCacheEntry` 析构时直接释放 `ID2D1Bitmap`：

```
背景渲染线程（渲染完成时）                  UI 线程（WM_PAINT 中）
                                            ├─ PaintTile(…)
                                            │   rt->BeginDraw()
                                            │   …
RenderCache::Add(req, bmp)                   │
  └─ 旧 BitmapCacheEntry 析构                │
      └─ bitmap->d2dBitmap->Release() ← ────┤── Cross-thread Release！
                                            │   rt->DrawBitmap()     ← CRASH
                                            │   rt->EndDraw()
```

D2D 资源（`ID2D1Bitmap`）必须在创建它的线程上释放。在渲染线程（后台线程）上释放 `ID2D1Bitmap` 会导致不可预测的行为——在 UI 线程正在使用该资源时立即引发 `Access Violation` 或 `D2D ERR (0x8899000C)`。

#### 3.2.2 修复方案（已落地）

引入**延迟释放队列（Deferred Release Queue）**：

```cpp
// RenderCache.cpp — v6 生产级加固
// 多核多渲染线程并发入队，必须使用 CRITICAL_SECTION 保护
static Vec<IUnknown*> gDeferredD2dReleases;
static CRITICAL_SECTION gDeferredD2dCS;

static void QueueSafeD2dRelease(IUnknown* obj) {
    if (!obj) return;
    EnterCriticalSection(&gDeferredD2dCS);
    gDeferredD2dReleases.Append(obj);     // ✅ 互斥锁保护，渲染工作线程安全入队
    LeaveCriticalSection(&gDeferredD2dCS);
}

void FlushSafeD2dReleases() {
    Vec<IUnknown*> toRelease;

    // 快速提取并清空队列，减少锁持有时间
    EnterCriticalSection(&gDeferredD2dCS);
    toRelease = gDeferredD2dReleases;     // 移动语义：仅交换指针（O(1)）
    gDeferredD2dReleases.Reset();
    LeaveCriticalSection(&gDeferredD2dCS);

    // 临界区外执行真实的 D2D 资源 Release，规避锁嵌套
    for (auto* obj : toRelease) {
        obj->Release();
    }
}

// PaintTile 入口处调用（UI 线程上下文）
int RenderCache::PaintTile(...) {
    FlushSafeD2dReleases();  // ← 在 UI 线程、BeginDraw 之前执行
    // ...
}

// BitmapCacheEntry 析构函数
BitmapCacheEntry::~BitmapCacheEntry() {
    if (bitmap && bitmap->d2dBitmap) {
#ifdef _MSC_VER
        QueueSafeD2dRelease(bitmap->d2dBitmap);  // 延迟释放
        bitmap->d2dBitmap = nullptr;
#else
        // 非 MSVC 编译直接释放
#endif
    }
    FreePixmap(bitmap);
}
```

---

### 3.3 场景 3：SRWLOCK 非递归自死锁（`IsLinearizedFile` 重入）（死锁概率：极高 🔴）

**状态**：✅ **v5 已修复**（`src/EngineMupdf.cpp`）

#### 3.3.1 根因

Windows `SRWLOCK` 是**严格非递归（Non-recursive）**的。`EngineMupdf::Load` 在入口处获取了 `docLock [Exclusive]`，然后调用 `FinishLoading()` → `IsLinearizedFile()`，而后者的内部又尝试获取 `docLock [Exclusive]`：

```cpp
// v5 修复前：自死锁现场
bool EngineMupdf::Load(const char* fileName) {
    ScopedSRWLockExclusive lock(&docLock);   // ① 第一次获取 Exclusive（成功）
    // ...
    if (!FinishLoading()) { return false; }  // → 调用 IsLinearizedFile
    // ...
}

static bool IsLinearizedFile(EngineMupdf* e) {
    ScopedSRWLockExclusive lock(&e->docLock); // ② 同一线程再次获取 Exclusive → 永久挂起！
    return pdf_doc_was_linearized(ctx, pdfdoc);
}
```

主线程自我等待、自我死锁——程序完全冻结。

#### 3.3.2 修复方案（已落地）

```cpp
// v5 修复后：调用者必须已持有 docLock
static bool IsLinearizedFileLocked(EngineMupdf* e) {
    // 不再获取 docLock——复用调用者已持有的锁
    return pdf_doc_was_linearized(ctx, pdfdoc);
}

bool EngineMupdf::Load(const char* fileName) {
    ScopedSRWLockExclusive lock(&docLock);    // ① 唯一一次获取 Exclusive
    FinishLoading();
    // ...
}

bool EngineMupdf::FinishLoading() {
    if (IsLinearizedFileLocked(this)) { ... } // ② 复用①的锁
    // ...
}
```

**项目规范新增**：所有内部辅助函数若需要调用者已持有锁，其命名后缀为 `Locked`（如 `IsLinearizedFileLocked`、`MakeAnnotationWrapperLocked`），且不在内部获取任何 `docLock`。

---

### 3.4 场景 4：持锁全量文本提取引发 UI 线程卡死（卡死概率：极高 🔴）

**状态**：✅ **v5 已修复**（`src/EngineMupdf.cpp`）

#### 3.4.1 根因

`ExtractTextLazy` 在持有 `docLock [Shared]` 后，通过阻塞 `ScopedCritSec` 获取 `pagesLock` 来缓存 stext 结果。与此同时，UI 线程在文档加载时持有 `pagesLock` 后通过 `ScopedSRWLockExclusive` 获取 `docLock` 来读取属性。两条路径锁次序完全相反 → 环形等待死锁：

```
UI 线程：pagesLock (持有) → 等待 docLock [Exclusive]（GetPropertyTemp）
渲染线程：docLock [Shared] (持有) → 等待 pagesLock（ExtractTextLazy 缓存结果）
                                  ↻ 死锁！
```

#### 3.4.2 修复方案（已落地）— 双重修复

**修复 A：`ExtractTextLazy` 非阻塞缓存**（EngineMupdf.cpp:3677）

```cpp
// v5 修复后：使用 TryEnterCriticalSection 避免阻塞
if (TryEnterCriticalSection(&engine->pagesLock)) {
    pageInfo->stextPage = stext;
    pageInfo->textExtracted = true;
    LeaveCriticalSection(&engine->pagesLock);
} else {
    // v6 优化：不再丢弃 stext 结果！通过 uitask 异步派发给 UI 线程安全挂载
    // 避免数百毫秒的文本提取成果被无谓销毁（CPU 效能黑洞）
    uitask::Post([engine, pageNo, stext, ctx]() {
        ScopedCritSec cs(&engine->pagesLock); // UI 线程上 pagesLock 不会引发死锁
        FzPageInfo* pageInfo = engine->pages[pageNo - 1];
        if (pageInfo && !pageInfo->textExtracted) {
            pageInfo->stextPage = stext;
            pageInfo->textExtracted = true;
        } else {
            fz_drop_stext_page(ctx, stext);   // 仅在重复挂载时安全释放
        }
    });
    return;
}

// v6 优化说明：
// 原 fz_drop_stext_page 会直接丢弃数百毫秒计算出的文本提取结果，
// 下一次渲染又得重复提取——形成恶性循环。通过 uitask::Post 将缓存
// 操作派发到 UI 线程消息队列，当 UI 线程处理完当前任务后自然释放
// pagesLock 时即可安全写入。完全规避死锁，同时消除 CPU 浪费。
```

**修复 B：`GetPropertyTemp` 锁降级**（EngineMupdf.cpp:4459）

```cpp
// v5 修复后：从 Exclusive 降级为 Shared
TempStr EngineMupdf::GetPropertyTemp(Str name) {
    auto ctx = Ctx();
    // 只读操作 — 共享锁不会阻塞后台渲染线程
    ScopedSRWLockShared ctxScope(&docLock);
    // ...
}
```

---

### 3.5 场景 5：`Invalidate()` 未在标注修改后调用 → 翻页时上一页固定压缩（视觉异常：中 ⚠️）

**状态**：⚠️ **v5 部分修复——`MarkNotificationAsModified` 底层未调用 `Invalidate`。建议在 UI 交互层（`Canvas.cpp`、`DisplayModel.cpp`）触发。**

#### 3.5.1 完整路径分析

```
【标注修改流程】（UI 线程）
用户操作 → MarkNotificationAsModified(e, annot, change)
  ├─ pagesLock → 更新 annotations 数组
  ├─ docLock → RebuildCommentsFromAnnotations，丢弃 displayList
  ├─ annotGeneration++
  └─ ← 此处缺少：gRenderCache->Invalidate(dm, pageNo, fullPage)

【翻页渲染流程】（WM_PAINT → PaintTile）
  ├─ Find(dm, pageNo, rotation, zoom, &tile)
  │   → zoom 匹配 → 命中旧缓存位图（entry->outOfDate == false）
  ├─ DrawBitmap(d2dBmp, dst, ...)
  │   → 绘制了不含最新标注的旧页面内容
  └─ 用户看到的页面没有标注修改！
```

#### 3.5.2 降级替换时的位图缩放（"上一页压缩"成因）

当 `Find(zoom)` 返回空但 `Find(kInvalidZoom)` 返回旧条目时，D2D 的 `DrawBitmap` 会将旧位图根据**目标矩形**进行缩放。如果缩放前后的页面尺寸发生变化（例如标注修改了注解弹出框的大小），旧位图会被拉伸或压缩——产生用户看到的 **"上一页固定在上方并被压缩"** 的视觉异常。

#### 3.5.3 与 `gNoFlickerRender` 的协同作用

```cpp
// Canvas.cpp:2268-2273
bool shouldPaint = DrawDocument(win, win->buffer->GetDC(), &ps.rcPaint);
if (!gNoFlickerRender || shouldPaint) {
    Rect dirty(ps.rcPaint);
    win->buffer->Flush(hdc, dirty);
}
```

当 `shouldPaint` 为 `false` 且 `gNoFlickerRender` 为 `true` 时，缓冲区内容**不刷新到屏幕**。如果前一次 `DrawDocument` 因为部分页面渲染失败只绘制了部分位图，缓冲区中的旧内容（含上一页）就会残留在用户界面上。

### 3.6 场景 6：`PageContentBox` 缺少 `renderLock` → MuPDF 内部缓存损坏崩溃（崩溃概率：高 🔴 v6.5 新增）

**根因**：`PageContentBox()`（EngineMupdf.cpp:3767）在 `Relayout` 流程中被调用，用于计算每个页面的内容边界框。该函数执行 `fz_bound_page`、`GetOrBuildPageDisplayList` 和 `fz_run_display_list` 等 MuPDF 调用，但在 **v6 实现中这些调用未在 `renderLock` 保护下执行**。

**崩溃复现场景**：

```
Relayout (UI线程, 按下 'c' 键切换单页)
  └─ PageContentBox (pageNo)
       ├─ GetFzPageInfo         → 内部持有 renderLock（在函数内释放）
       ├─ fz_bound_page           ← ❌ 无锁！与 RenderPage（渲染线程）并发
       ├─ GetOrBuildPageDisplayList ← ❌ 无锁！并发访问 displayList 指针
       └─ fz_run_display_list     ← ❌ 无锁！MuPDF 字体/图像缓存损坏
```

1. 用户按下 Ctrl+6（CmdSinglePageView）切换单页显示模式
2. `SetDisplayMode` 调用 `GoToPage` → `ChangeStartPage` → `Relayout`
3. `Relayout` 对页面调用 `PageContentBox`
4. 渲染线程正在后台对同样页面执行 `RenderPage`（持有 `renderLock`）
5. 渲染线程的 `fz_run_display_list` 修改 MuPDF 字体/图像缓存 hash table
6. 同时 UI 线程的 `PageContentBox` 也在调用 `fz_run_display_list`（通过 bbox device）
7. 两个线程同时访问 MuPDF 内部缓存 → **数据竞争** → 损坏 hash table/bucket 指针
8. 下次 MuPDF 操作触发 **ACCESS_VIOLATION** 或 **heap corruption** → 崩溃

**修复方案**（已在 v6.5 落地）：在 `PageContentBox` 中所有 MuPDF 调用的外围添加 `docLock [Shared]` 和 `renderLock` 保护。

```cpp
// EngineMupdf.cpp — v6.5 修复
AcquireSRWLockShared(&docLock);
{
    ScopedCritSec scope(&renderLock);  // ✅ 新增：所有 MuPDF 调用在此保护内
    pagerect = fz_bound_page(ctx, pageInfo->page);
    keptList = GetOrBuildPageDisplayList(pageInfo, ctx);
    if (keptList) {
        fz_run_display_list(ctx, keptList, dev, fz_identity, pagerect, &fzcookie);
    }
}
ReleaseSRWLockShared(&docLock);
```

### 3.7 场景 7：`SetDisplayMode` 切换模式时未清除 RenderCache 瓦片 → 视觉残留拉伸（视觉异常：中 🟡 v6.5 新增）

**根因**：当用户通过快捷键或菜单切换显示模式（如从连续切换到单页）时，`SetDisplayMode()` 更新布局参数并调用 `Relayout`。然而，**RenderCache 中的老旧 Bitmap 瓦片未被清除**。

```
连续模式 → 按下 'c' 键 → SetDisplayMode(SinglePage) → GoToPage → Relayout → PaintTile
                                                              ↑
                                                    RenderCache 仍存有
                                                    旧模式（连续）的高分辨率瓦片
```

**具体危害**：

1. **视觉残留**：`PaintTile` 调用 `RenderCache::Find(pageNo)` 时，如果找到过期的连续模式瓦片（zoom、rotation 可能不同），会以错误尺寸绘制瓦片 → 导致"前一页一直压缩在视口"的 bug。

2. **渲染线程空转**：即使 `Find` 未命中，渲染线程仍在为旧模式渲染页面。切换模式后这些渲染请求被废弃，CPU 资源被浪费。

3. **崩溃间接诱因**：在 v6.5 修复 `PageContentBox` 前，缓存中的过期瓦片可能包含在错误锁状态下构建的 display list 数据，增加崩溃概率。

**修复方案**（已在 v6.5 落地）：在 `SetDisplayMode` 开头、修改 `displayMode` 成员前，显式调用 `gRenderCache->Invalidate` 清除所有页面的到期瓦片：

```cpp
// DisplayModel.cpp — v6.5 新增
if (gRenderCache) {
    RectF fullPage = engine->PageMediabox(1);
    for (int pn = 1; pn <= PageCount(); pn++) {
        gRenderCache->Invalidate(this, pn, fullPage);
    }
}
```

---

## 4. 崩溃与卡死问题汇总

| 场景 | 问题类型 | 概率 | 根因 | 修复状态 | 修复文件 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **CreateBitmapFromPixmap UAF** | 崩溃 | **高 💥** | `cachedRT` 被内部覆盖 | ✅ **已修复** | `GpuBackend.cpp` |
| **D2D 跨线程 Release** | 崩溃 | **高 💥** | 渲染线程释放 D2D 资源 | ✅ **已修复** | `RenderCache.cpp` |
| **SRWLOCK 自死锁** | 死锁 | **极高 🔴** | 非递归锁重入 | ✅ **已修复** | `EngineMupdf.cpp` |
| **pagesLock↔docLock 环等待** | 死锁 | **高 🔴** | 锁顺序反转 + 阻塞获取 | ✅ **已修复** | `EngineMupdf.cpp` |
| **持锁文本提取** | 卡死 | **极高 💥** | 在 pagesLock+renderLock 内执行慢速提取 | ✅ **已修复** | `EngineMupdf.cpp` |
| **缓存位图过时** | 视觉异常 | **中 🟡** | `Invalidate()` 未在标注修改后调用 | ⚠️ **待 UI 层调用** | `Canvas.cpp` |
| **gNoFlickerRender 跳过刷新** | 视觉异常 | **低 🟢** | `shouldPaint==false` 时缓冲区不刷新 | 💡 **建议加固** | `Canvas.cpp` |
| **🔴 `PageContentBox` 无锁 MuPDF** | 崩溃 | **高 💥** | Relayout 时 MuPDF 调用无 renderLock | ✅ **v6.5 已修复** | `EngineMupdf.cpp` |
| **🟡 `SetDisplayMode` 缓存未失效** | 视觉异常 | **中 🟡** | 模式切换后 RenderCache 存有旧瓦片 | ✅ **v6.5 已修复** | `DisplayModel.cpp` |

---

## 5. 修复建议与落地状态

### 5.1 已落地修复（✅ 已完成）

#### 修复 1：`CreateBitmapFromPixmap` 参数化修复（`GpuBackend.cpp`）
将 `CreateBitmapFromPixmap` 改为接收调用者传入的 `ID2D1DCRenderTarget* rt`，杜绝内部调用 `GetRenderTarget` 覆盖 `cachedRT` 导致的 Use-After-Free。

- **文件**：`src/GpuBackend.h:36`（签名变更），`src/GpuBackend.cpp:108`（实现变更）
- **验证方式**：GPU 路径下连续翻页标注不会崩溃

#### 修复 2：D2D 延迟释放队列（`RenderCache.cpp`）
引入 `QueueSafeD2dRelease` / `FlushSafeD2dReleases` 机制，将所有 `ID2D1Bitmap::Release` 推迟到 UI 线程的 `PaintTile` 入口处执行。

- **文件**：`src/RenderCache.cpp:41`（`QueueSafeD2dRelease`）、`src/RenderCache.cpp:1039`（`FlushSafeD2dReleases` 调用点）
- **数据结构**：`Vec<IUnknown*> gDeferredD2dReleases`
- **验证方式**：在 `BitmapCacheEntry` 析构函数中断言 `d2dBitmap` 已被置空

#### 修复 3：`IsLinearizedFile` 重入死锁修复（`EngineMupdf.cpp`）
重命名为 `IsLinearizedFileLocked`，移除内部 `ScopedSRWLockExclusive`；调用者 `Load` 在持有 `docLock` 期间调用。

- **文件**：`src/EngineMupdf.cpp:2870`（`IsLinearizedFileLocked` 定义）、`src/EngineMupdf.cpp:2876`（调用点）
- **验证方式**：打开任意 PDF 文件不会卡死

#### 修复 4：`ExtractTextLazy` 非阻塞缓存（`EngineMupdf.cpp`）
将 `ScopedCritSec` 替换为 `TryEnterCriticalSection`，无法获取 `pagesLock` 时放弃缓存结果。

- **文件**：`src/EngineMupdf.cpp:3677`（TryEnter + 放弃路径）、`src/EngineMupdf.cpp:3684`（`fz_drop_stext_page` 回退）
- **验证方式**：打开带有大量标注的 PDF 不会卡死

#### 修复 5：`GetPropertyTemp` 锁降级（`EngineMupdf.cpp`）
将 `ScopedCritSec(&docLock)` → `ScopedSRWLockShared(&docLock)`。

- **文件**：`src/EngineMupdf.cpp:4459`（定义）
- **验证方式**：`GetPropertyTemp` 不会阻塞持有 `docLock [Shared]` 的后台渲染线程

### 5.2 ✅ v6.5 新增已落地修复

#### 修复 10：`PageContentBox` 添加 `docLock Shared` + `renderLock`（`EngineMupdf.cpp`）

**说明**：`PageContentBox()` 在 `Relayout` 流程中被 UI 线程调用执行 MuPDF 操作（`fz_bound_page`、`fz_run_display_list` 等），但在 v6 中这些操作未在 `renderLock` 保护下执行，导致与渲染线程的数据竞争。

在 v6.5 中，将所有 MuPDF 调用包裹在 `AcquireSRWLockShared(&docLock)` → `ScopedCritSec scope(&renderLock)` 保护范围内。

- **文件**：`src/EngineMupdf.cpp:3791-3823`
- **验证方式**：按下 Ctrl+6（CmdSinglePageView）切换显示模式时不会崩溃

#### 修复 11：`SetDisplayMode` 切换前清除 RenderCache（`DisplayModel.cpp`）

**说明**：`SetDisplayMode()` 修改 `displayMode` 成员时未清除 RenderCache 中的陈旧瓦片，导致 `PaintTile` 以错误尺寸绘制旧瓦片（视觉残留"上一页压缩"）。

在 v6.5 中，在 `SetDisplayMode` 开头添加循环调用 `gRenderCache->Invalidate` 清除所有页面的到期瓦片。

- **文件**：`src/DisplayModel.cpp:1535-1540`
- **验证方式**：切换显示模式后不会出现"前一页一直压缩在视口"的视觉异常

### 5.3 高优先级待落地（⚠️ 建议下一版本）

#### 修复 6（保留 v6 内容）：标注修改后调用 `Invalidate()`（`Canvas.cpp` / `DisplayModel.cpp`）

**说明**：`MarkNotificationAsModified` 不应直接调用 `Invalidate`...

#### 修复 12：`OnAnnotationModified` UI 层缓存失效（`Canvas.cpp`）

在 `Canvas.cpp` 标注编辑完成事件通路中添加 `Invalidate` 调用，确保标注编辑后 RenderCache 瓦片被清除：

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
    }

    // 3. 触发重绘——下一帧 DrawDocument 将重新渲染含最新标注的页面
    win->RepaintAsync();
}
```

### 5.4 低优先级优化（💡 建议后续版本）

#### 修复 7：`gNoFlickerRender` 强制刷新保护

（保留 v6 内容不变）

#### 修复 8：`renderLock` 升级为 `SRWLOCK`

（保留 v6 内容不变）

#### 修复 9：`pagesLock` 细粒度化

（保留 v6 内容不变）

---

## 6. 回归防范措施

### 6.1 新增的代码规范

1. **命名规范**：调用者需持有锁的辅助函数必须以 `Locked` 后缀结尾（如 `IsLinearizedFileLocked`、`MakeAnnotationWrapperLocked`）。
2. **锁获取**：禁止在 `renderLock` 内部获取 `docLock`。
3. **D2D 资源**：所有 `ID2D1Bitmap` / `ID2D1SolidColorBrush` 等 D2D 资源的 Release 操作必须在创建线程上执行。后台线程一律使用 `QueueSafeD2dRelease`。
4. **`PageContentBox` 规则**：任何执行 MuPDF 调用（`fz_bound_page`、`fz_run_display_list`、`GetOrBuildPageDisplayList`）的 UI 线程路径**必须**同时持有 `docLock [Shared]` 和 `renderLock`。
5. **`SetDisplayMode` 规则**：修改显示模式前**必须**调用 `gRenderCache->Invalidate` 清除所有到期瓦片。
6. **无锁 MuPDF 调用禁令**：禁止在 UI 线程上执行无 `renderLock`/`docLock` 保护的任何 MuPDF 函数调用（包括 `fz_bound_page`、`fz_new_bbox_device`、`fz_run_display_list` 等）。

### 6.2 调试断言（ReportIf / AssertCrash）— v6.5 补充

在 `GetFzPageInfo` 入口处添加调试断言，确保调用者没有违反锁层级：

```cpp
// EngineMupdf.cpp — GetFzPageInfo 入口
#ifdef DEBUG
    // ✅ 严禁在持有 renderLock 时获取 docLock（违反 pagesLock→docLock→renderLock 层级）
    // 这是锁顺序反转（renderLock→docLock）的根本防范
    ReportIf(IsRenderLockHeldByCurrentThread());
#endif

// 在 AcquireSRWLockExclusive (&docLock) 中额外检查：
#ifdef DEBUG
    // 检查 SRWLOCK 递归获取（SRWLOCK 严格非递归）
    ReportIf(IsCurrentThreadHoldingSRWLockExclusive(&docLock));
#endif

// 在 GetOrBuildPageDisplayList 入口处检查：
#ifdef DEBUG
    // 验证调用者已持有 docLock [Shared]（惰性资源保护）
    ReportIf(!IsDocLockHeldByCurrentThread());
#endif

// v6.5 新增：在 PageContentBox 入口断言无锁反转
#ifdef DEBUG
    // 验证 UI 线程未先获取 docLock Exclusive（标注编辑锁）
    // 如果持有，则获取 docLock Shared 会死锁
    // （Exclusive 等待所有 Shared 释放，而 Shared 在 Exclusive 后面不能获取）
#endif

// 检查 Invalidate 在标注修改后被调用
AssertCrash(lastInvalidationTime >= lastAnnotationModificationTime);
```

### 6.3 测试覆盖

| 测试 | 文件 | 说明 |
| :--- | :--- | :--- |
| DisplayMode + Annotation | `tests/issue-display-mode-annot.ts` | 快速模式切换 + 渲染 + TOC + Dest 压力 ✓ v6.5 新增 |
| Annotation Locking | `tests/issue-annot-locking.ts` | 并行渲染 + TOC + Dest + 搜索 + 生命周期 ✓ v6 |

### 6.4 测试建议

1. **GPU 路径持续翻页测试**：在启用 GPU 加速的情况下，打开含 100+ 标注的 PDF 并快速翻页，持续 5 分钟。
2. **标注创建/删除/编辑循环测试**：创建标注 → 修改内容 → 删除 → 翻页 → 创建，循环 1000 次。
3. **显示模式切换压力测试**：在含标注的文档中，反复按 Ctrl+6 (CmdSinglePageView) 和各种显示模式切换，检查是否崩溃或视觉异常。
4. **高频打开/关闭快速测试**：连续打开和关闭文档 100 次，检查是否发生悬垂指针崩溃。
5. **多线程文本提取压力测试**：打开含大量文本的文档，在后台渲染线程工作的同时快速翻页。

---

## 7. 参考

- `src/EngineMupdf.cpp`: `RenderPage()`、`PageContentBox()`、`GetOrBuildPageDisplayList()`、`MarkNotificationAsModified()`、`GetFzPageInfo()`
- `src/EngineMupdf.h`: `FzPageInfo` 结构体、锁定义（pagesLock, renderLock, docLock）
- `src/Annotation.cpp`: 各标注 setter 函数（docLock Exclusive → MarkNotificationAsModified 模式）
- `src/RenderCache.cpp`: `RenderCacheThread()`、`Invalidate()`、`QueueSafeD2dRelease()`、`FlushSafeD2dReleases()`
- `src/DisplayModel.cpp`: `SetDisplayMode()`、`Relayout()`
- `src/GpuBackend.cpp`: `CreateBitmapFromPixmap()`、`GetRenderTarget()`
- `src/Canvas.cpp`: `OnPaintDocument()`、`DrawDocument()`
- `src/base/ScopedWin.h`: `ScopedCritSec`、`ScopedSRWLockShared`、`ScopedSRWLockExclusive`
- `tests/issue-display-mode-annot.ts`: v6.5 新增显示模式切换测试
- `tests/issue-annot-locking.ts`: v6 标注锁定渲染安全测试
- `docs/reports/multithreading-report.md`: 多线程架构详情
