# Direct2D 设备代际（Device Generation）自愈合系统分析与实现报告

## 1. 概述

在 SumatraPDF 的 Direct2D 混合渲染架构中，`ID2D1DCRenderTarget` 所管理的 GPU 纹理（`ID2D1Bitmap`）具有**资源域亲和性（Resource Domain Affinity）**：一个位图只能在其创建的 RenderTarget 上进行绘制。当 HDC 切换、窗口拉伸、或显卡驱动重置导致 RenderTarget 被销毁重建时，旧位图成为跨域资源，Direct2D 抛出 `D2DERR_WRONG_RESOURCE_DOMAIN`（`0x88990015`），导致 GPU 渲染路径退化到 GDI 慢速路径，引发画面撕裂、卡顿。

本报告分析该问题的技术根因，评估当前代码实现状态，并提出"设备代际（Device Generation）自愈合系统"的完整解决方案。

---

## 2. 技术背景：0x88990015 病理学分析

### 2.1 错误码定义

| 属性 | 值 |
| :--- | :--- |
| **符号名** | `D2DERR_WRONG_RESOURCE_DOMAIN` |
| **十六进制** | `0x88990015` |
| **官方释义** | *"The resource used was created by a render target in a different resource domain."* |

### 2.2 触发机制推演

```
[T0] GPU 初始化：
  GetRenderTarget(hdc1) → CreateDCRenderTarget() → deviceGeneration = 1

[T1] 页面渲染：
  CreateBitmapFromPixmap(rt, pixmap) → rt_A->CreateBitmap(Bmp_A)
  → pixmap->d2dDeviceGeneration = 1

[T2] PaintTile 绘制（同一设备域 → 正常）：
  rt_A->BeginDraw() → DrawBitmap(Bmp_A) → EndDraw() → S_OK

[T3] 窗口拉伸 / HDC 切换：
  GetRenderTarget(hdc2) → 释放 RT_A → 创建 RT_B
  → ❌ deviceGeneration 未递增（仍为 1）

[T4] 跨域绘制（崩溃）：
  rt_B->DrawBitmap(Bmp_A, ...) → EndDraw() → ❌ 0x88990015
  → 退化到 GDI 慢速路径 → 卡顿、闪烁
```

### 2.3 实际表现

| 场景 | 现象 | 程度 |
| :--- | :--- | :--- |
| 连续缩放窗口 | 页面闪烁，每缩放一次触发一次 fallback | 高 |
| 全屏/普通切换 | 首帧空白，GDI 重新渲染 | 高 |
| 双屏切换 | 页面变白，数秒恢复 | 中 |
| 显卡驱动重置(TDR) | 应用假死 2-5 秒，GPU 路径永久失效 | 极高 |

---

## 3. 设备代际自愈合系统架构

### 3.1 核心设计原则

1. **无侵入版本管理**：通过轻量级版本号在绘制时自检，不重构缓存生命周期。
2. **按需懒重建**：仅在 `PaintTile` 引用时检测并重建，避免设备切换时全量遍历。
3. **零锁开销**：版本号单调递增，读取无锁。仅延迟释放队列使用 CRITICAL_SECTION。
4. **优雅降级**：GPU 丢失时触发 `RecreateRenderTarget()`，旧位图自动失效。

### 3.2 版本生命周期

```
deviceGeneration = 0  (GpuBackend 构造)
  → GetRenderTarget() 首次调用
deviceGeneration = 1  (首次创建 D2D RT)
  → CreateBitmapFromPixmap + PaintTile 上传
Pixmap.d2dDeviceGeneration = 1
  → 窗口拉伸 → GetRenderTarget(new HDC)
deviceGeneration = 2  (设备域变更)
  → PaintTile 检测 d2dDeviceGeneration(1) != deviceGeneration(2)
    → QueueSafeD2dRelease(old d2dBitmap)  ← 旧位图安全释放
    → CreateBitmapFromPixmap(newRT, pixmap)  ← 在新设备域重建
    → d2dDeviceGeneration = 2  ← 记录新代际
  → EndDraw() → S_OK  (版本一致，无跨域错误)
```

---

## 4. 当前代码库实现状态评估

### 4.1 已实现的组件

#### 4.1.1 `GpuBackend.h` — 设备代际声明 ✅

```cpp
// GpuBackend.h 第 98-100 行
int deviceGeneration = 1;

// GpuBackend.h 第 43 行
int GetDeviceGeneration() const { return deviceGeneration; }
```

#### 4.1.2 `Pixmap.h` — 位图代际记录 ✅

```cpp
// Pixmap.h 第 54-59 行
struct ID2D1Bitmap* d2dBitmap = nullptr;
int d2dDeviceGeneration = 0;  // 初始 0，与 GpuBackend 初始值 1 不同
```

#### 4.1.3 `RenderCache.cpp` — PaintTile 检测逻辑 ✅

```cpp
// RenderCache.cpp 第 1152-1162 行
int currentDevGen = gGpuBackend->GetDeviceGeneration();
if (renderedBmp->d2dBitmap && renderedBmp->d2dDeviceGeneration != currentDevGen) {
    QueueSafeD2dRelease(renderedBmp->d2dBitmap);
    renderedBmp->d2dBitmap = nullptr;
    renderedBmp->d2dDeviceGeneration = 0;
}
```

#### 4.1.4 上传后更新代际 ✅

```cpp
// RenderCache.cpp 第 1171-1175 行
d2dBmp = gGpuBackend->CreateBitmapFromPixmap(rt, renderedBmp);
if (d2dBmp) {
    renderedBmp->d2dBitmap = d2dBmp;
    renderedBmp->d2dDeviceGeneration = gGpuBackend->GetDeviceGeneration();
}
```

#### 4.1.5 延迟释放队列 ✅

`QueueSafeD2dRelease`（CRITICAL_SECTION 保护入队）和 `FlushSafeD2dReleases`（UI 线程执行 Release）完全实现。

### 4.2 已修复的历史缺口（🟢 已验证）

> **说明**：以下三个缺口在本次代码审查中验证全部已修复。标记为 ❌ 的原始报告已过时。

#### 4.2.1 ✅ `GetRenderTarget()` 已递增 `deviceGeneration`

**文件**：`src/GpuBackend.cpp` 第 84 行

**修复状态**：创建新 `ID2D1DCRenderTarget` 后，紧接 `deviceGeneration++`。

```cpp
// GpuBackend.cpp:84 (已验证)
deviceGeneration++;
#ifdef DEBUG
    ReportIf(deviceGeneration <= 0); // 2026-07 新增调试断言
#endif
```

#### 4.2.2 ✅ `RecreateRenderTarget()` 方法已实现

**文件**：`src/GpuBackend.cpp` 第 128-155 行（含新增断言和计数器）

**修复状态**：完全实现，释放旧 RT → 递增代际 → 记录日志。

```cpp
void GpuBackend::RecreateRenderTarget() {
    if (cachedRT) {
        cachedRT->Release();
        cachedRT = nullptr;
        cachedHDC = nullptr;
    }
    deviceGeneration++;
    ReportIf(deviceGeneration <= 0); // 调试断言
    InterlockedIncrement(&gDeviceGenRecreations); // 性能计数器
    logfa("[RenderCache Diagnostic] ...");
}
```

#### 4.2.3 ✅ `D2DERR_RECREATE_TARGET` 恢复路径完整

**文件**：`src/RenderCache.cpp` 第 1197-1210 行

**修复状态**：捕获 `D2DERR_RECREATE_TARGET` 后触发 `RecreateRenderTarget()` 并递增诊断计数器。

```cpp
if (hrEnd == D2DERR_RECREATE_TARGET) {
    logfa("[RenderCache Diagnostic] GPU Device Lost detected! ...");
    if (gGpuBackend) {
        InterlockedIncrement(&gDeviceGenRecreations);
        gGpuBackend->RecreateRenderTarget();
    }
}
```

---

## 5. 修复实现总览

### 5.1 三项核心修复（已完成，本节为历史快照）

以下三项核心修复已在本次审查之前的开发周期中全部实现。本节记录其实现位置，作为追溯参考。

#### 5.1.1 `GetRenderTarget()` 新建 RT 时递增代际

**文件**：`src/GpuBackend.cpp` 第 76-84 行

```cpp
// GpuBackend.cpp:84
deviceGeneration++;
```

#### 5.1.2 `RecreateRenderTarget()` 方法

**文件**：`src/GpuBackend.h` 第 62-63 行（声明），`src/GpuBackend.cpp` 第 128-155 行（实现）

```cpp
void GpuBackend::RecreateRenderTarget();
```

#### 5.1.3 `D2DERR_RECREATE_TARGET` 恢复路径

**文件**：`src/RenderCache.cpp` 第 1197-1210 行

### 5.2 新增额外修复（2026-07 补充） 🆕

#### 5.2.1 调试断言 `ReportIf(deviceGeneration <= 0)` 🆕

在 `GetRenderTarget()` 和 `RecreateRenderTarget()` 中每次 `deviceGeneration++` 后添加：

```cpp
#ifdef DEBUG
    ReportIf(deviceGeneration <= 0); // 代际必须为正整数
#endif
```

#### 5.2.2 D2D Overlay 辅助函数的 EndDraw 错误处理 🆕

**问题**：`DrawOverlayRects`、`DrawDashedBorder`、`DrawResizeHandle`、`DrawFillRect`、`DrawSolidBorder` 五个 overlay 辅助函数之前不对 `EndDraw()` 的返回值做检查。当 `EndDraw()` 返回 `D2DERR_RECREATE_TARGET` 时，函数原封不动返回 `true`，使得调用者（`Selection.cpp` 和 `Canvas.cpp`）跳过 GDI+ 回退路径，且不触发 `RecreateRenderTarget()`，导致 GPU 设备丢失后所有后续 D2D 操作无效。

**修复**：每个辅助函数的 `EndDraw()` 后增加 `D2DERR_RECREATE_TARGET` 检测 + `RecreateRenderTarget()` 调用 + 性能计数器递增。失败时返回 `false` 以触发 GDI+ 回退。

```cpp
// 修复模式 (DrawOverlayRects 示例):
HRESULT hrEnd = rt->EndDraw();
if (FAILED(hrEnd)) {
    logfa("[RenderCache Diagnostic] D2D EndDraw failed in DrawOverlayRects HRESULT=0x%08X\n", (unsigned)hrEnd);
    if (hrEnd == D2DERR_RECREATE_TARGET && gGpuBackend) {
        InterlockedIncrement(&gDeviceGenRecreations);
        gGpuBackend->RecreateRenderTarget();
    }
    return false; // 触发 GDI+ fallback
}
return true;
```

#### 5.2.3 性能诊断计数器 🆕

**声明**：`src/RenderCache.h` 第 30-36 行

```cpp
extern LONG gDeviceGenEvictions;   // 代际不匹配触发的位图重建次数
extern LONG gDeviceGenRecreations; // RecreateRenderTarget 调用次数
extern LONG gD2dErrorFallbacks;    // D2D EndDraw 失败后 fallback 次数
```

**定义**：`src/RenderCache.cpp` 第 83-86 行

**注入点**：

| 计数器 | 注入位置 | 操作 |
| :--- | :--- | :--- |
| `gDeviceGenEvictions` | `RenderCache.cpp` 第 1167 行 | 代际检测不匹配 + 旧位图释放后 |
| `gDeviceGenRecreations` | `GpuBackend.cpp` 第 154 行 | `RecreateRenderTarget()` 方法内 |
| `gDeviceGenRecreations` | `GpuBackend.cpp` 第 356/400/435/462/489 行 | 各 overlay 辅助函数 EndDraw 失败时 |
| `gD2dErrorFallbacks` | `RenderCache.cpp` 第 1195 行 | PaintTile 中 EndDraw 失败后 |

---

## 6. 修复后的自愈流程

### 6.1 HDC 变更场景

```
用户拉伸窗口 → WM_PAINT 携带新 HDC
  → PaintTile() → GetRenderTarget(newHDC)
    → cachedHDC != newHDC → 释放旧 RT，创建新 RT
    → deviceGeneration++  (1 → 2)  ← 修复 1

  → 检测到 d2dDeviceGeneration(1) != deviceGeneration(2)
    → QueueSafeD2dRelease(old d2dBitmap)
    → CreateBitmapFromPixmap(newRT, pixmap) → d2dDeviceGeneration = 2

  → DrawBitmap + EndDraw → ✅ S_OK  (设备域匹配)
```

### 6.2 显卡驱动重置（TDR）场景

```
用户翻页 → PaintTile → BeginDraw/DrawBitmap/EndDraw
  → ❌ D2DERR_RECREATE_TARGET

恢复路径：
  → gGpuBackend->RecreateRenderTarget()  ← 修复 2
    → 释放旧 RT → deviceGeneration++ (2 → 3)
  → QueueSafeD2dRelease(old d2dBitmap) → d2dBitmap = nullptr
  → fallthrough 到 GDI 路径（当前帧无感降级）

下一帧 PaintTile：
  → GetRenderTarget(newHDC) → 创建新 RT → deviceGeneration++ (3 → 4)
  → d2dDeviceGeneration(0/2) != deviceGeneration(4)
  → 重建位图 → 在新设备域上正常 D2D 绘制
  → ✅ 用户无感知恢复
```

---

## 7. 现有测试覆盖

| 测试名称 | 文件 | 测试内容 | 状态 |
| :--- | :--- | :--- | :--- |
| `PixmapDeviceGenerationTest` | `GpuBackend_ut.cpp` | d2dDeviceGeneration 字段 | ✅ |
| `TestPixelConversionBgr8ToBgra8` | `GpuBackend_ut.cpp` | BGR8→BGRA8 像素转换 | ✅ |
| `TestPixelSwizzleRgba8ToBgra8` | `GpuBackend_ut.cpp` | RGBA8→BGRA8 通道重排 | ✅ |
| `TestDeferredReleaseQueue` | `GpuBackend_ut.cpp` | 延迟释放队列线程安全 | ✅ |
| `TestDeviceGenerationCounter` | `GpuBackend_ut.cpp` | 设备代际递增/匹配逻辑 | ✅ |
| 渲染稳定性测试(8项) | `issue-render-stability.ts` | GPU/GDI 翻页、缩放、OOM | ✅ |
| 设备代际压力测试 | `issue-device-generation.ts` | 版本循环、文档切换 | ✅ |

---

## 8. 回归防范

### 8.1 调试断言（2026-07 实施）🆕

**位置**：`src/GpuBackend.cpp` 第 85-87 行（`GetRenderTarget`）、第 147-149 行（`RecreateRenderTarget`）

```cpp
// 在 deviceGeneration++ 后
#ifdef DEBUG
    ReportIf(deviceGeneration <= 0);  // 代际必须为正整数
#endif
```

### 8.2 性能计数器（2026-07 实施）🆕

| 计数器 | 说明 | 异常阈值 | 文件位置 |
| :--- | :--- | :--- | :--- |
| `gDeviceGenEvictions` | 因代际不匹配触发的位图重建次数 | 单帧 > 10 | `RenderCache.h:34`, `RenderCache.cpp:1167` |
| `gDeviceGenRecreations` | RecreateRenderTarget 调用次数 | 60s 内 > 3 | `RenderCache.h:35`, `GpuBackend.cpp:154,356,400,435,462,489` |
| `gD2dErrorFallbacks` | D2D EndDraw 失败后 fallback 次数 | 60s 内 > 5 | `RenderCache.h:36`, `RenderCache.cpp:1195` |

### 8.3 测试扩展（待完成）

在 `tests/issue-device-generation.ts` 中扩展：
- 模拟 HDC 变更测试：DC_A → 上传 → DC_B → 触发自愈 → 绘制验证
- 设备重建测试：调用 RecreateRenderTarget → 验证代际递增 → 验证旧位图释放

---

## 9. 总结

**实现状态总览（2026-07 最终版）**：

| 组件 | 状态 | 文件位置 |
| :--- | :--- | :--- |
| `Pixmap::d2dDeviceGeneration` 字段 | ✅ | `Pixmap.h` 第 59 行 |
| `GpuBackend::deviceGeneration` 计数 | ✅ | `GpuBackend.h` 第 100 行 |
| `GetDeviceGeneration()` 接口 | ✅ | `GpuBackend.h` 第 43 行 |
| PaintTile 代际检测 + 重建 | ✅ | `RenderCache.cpp` 第 1152-1162 行 |
| 上传后代际更新 | ✅ | `RenderCache.cpp` 第 1173-1175 行 |
| 延迟释放队列 | ✅ | `RenderCache.cpp` 第 30-71 行 |
| **GetRenderTarget() 中 deviceGeneration++** | ✅ 已实现 | `GpuBackend.cpp:84` |
| **RecreateRenderTarget() 方法** | ✅ 已实现 | `GpuBackend.cpp:128-155` |
| **D2DERR_RECREATE_TARGET 恢复路径** | ✅ 完整 | `RenderCache.cpp:1197-1210` |
| **调试断言 ReportIf(deviceGeneration <= 0)** | ✅ 已实现 | `GpuBackend.cpp:85-87,147-149` |
| **D2D Overlay EndDraw 错误处理** | ✅ 已修复 | `GpuBackend.cpp:352-489` (5 个函数) |
| **gDeviceGenEvictions 性能计数器** | ✅ 已实现 | `RenderCache.h:34`, `RenderCache.cpp:1167` |
| **gDeviceGenRecreations 性能计数器** | ✅ 已实现 | `RenderCache.h:35`, 6 处注入点 |
| **gD2dErrorFallbacks 性能计数器** | ✅ 已实现 | `RenderCache.h:36`, `RenderCache.cpp:1195` |

**所有组件均已完成实现。** 本报告初始时识别的 3 项关键缺口（§4.2）和本轮新增的调试断言、性能计数器、D2D Overlay 修复（§5.2）均已实现并编译通过。

**后续维护建议**：
1. 在 `tests/issue-device-generation.ts` 中添加 HDC 变更和设备重建自动化测试（参见 §8.3）
2. 通过 `sumlog.txt` 中的计数器输出监控 GPU 路径健康度
3. 若发现新的 D2D EndDraw 失败场景，参照 §5.2.2 的模式新增处理

---

## 10. 参考

- `src/GpuBackend.h` — 设备代际声明
- `src/GpuBackend.cpp` — GetRenderTarget、CreateBitmapFromPixmap、Overlay 辅助函数实现
- `src/base/Pixmap.h` — Pixmap 结构体、d2dDeviceGeneration 字段
- `src/RenderCache.h` — 性能计数器声明
- `src/RenderCache.cpp` — PaintTile 检测、延迟释放队列、EndDraw 处理、计数器注入
- `src/base/tests/GpuBackend_ut.cpp` — 设备代际、像素转换、延迟释放队列单元测试
- `tests/issue-device-generation.ts` — 设备代际压力测试
- `tests/issue-render-stability.ts` — 渲染稳定性综合测试
- `docs/reports/annot-render-crash-analysis.md` — 标注渲染崩溃综合分析（v6.5 修订版）
