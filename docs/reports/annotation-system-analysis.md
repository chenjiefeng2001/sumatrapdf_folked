# SumatraPDF 标注 (Annotation) 系统实现分析报告

> 版本: folked 主分支现状 (对应 commit 9c9a8e548), **2026-08-03 (7) 重构后已同步** — P1/P2 改进均已落地 (见 §4/§9/§10)
> 日期: 2026-08-03
> 分析范围: 标注数据模型、创建/编辑/删除/移动/调整大小生命周期、渲染与缓存失效、并发与锁、编辑窗口 UI、命令面板集成 (最新 CommandAvailability 实现)、测试覆盖、已知风险
> 关联文档: `docs/reports/annot-render-crash-analysis.md` (渲染崩溃根因分析) / `docs/UI_REPORT.md` §4.4 (UI 集成)

---

## 1. 功能概览

当前标注系统是**基于 MuPDF `pdf_annot` 的直接包装**, 而非自维护的标注模型:

| 能力 | 实现 | 入口 |
|---|---|---|
| 标注创建 | 高亮/波浪线/删除线/下划线/文本框/图像戳 (Stamp) 等 | `CmdCreateAnnot*` → `EngineMupdfCreateAnnotation` |
| 标注编辑 | 移动 / 8 向 resize / 属性修改 (颜色/边框/透明度/图标/文本) | Canvas 拖动 + `EditAnnotationsWindow` |
| 标注删除 | `DeleteAnnotation` | `CmdDeleteAnnotation` / 编辑窗口 |
| 标注渲染 | 内容流与标注层**分离缓存**: 页面内容 displayList 仅建一次, 标注/表单控件单独叠加层, 修改标注不重跑页面内容 | `GetOrBuildContentDisplayList` + `GetOrBuildAnnotDisplayList` |
| 缓存失效 | 内容层常驻; 标注层 `annotDisplayListGeneration` 原子计数 + RenderCache 瓦片级 `Invalidate` | `MarkNotificationAsModified` → `OnAnnotationModified` |
| 命中测试 | 每页空间网格索引 (`AnnotHitIndex`), 拖动/悬停命中 O(候选数) 而非 O(n) | `EngineMupdfGetAnnotationAtPos` / `GetWidgetAtPos` |
| 保存 | 增量写回原 PDF / 另存新文件 | `CmdSaveAnnotations` / `CmdSaveAnnotationsNewFile` |
| 命令面板集成 | 按上下文动态显示/隐藏/禁用标注命令 | `CommandAvailability::GetCommandVisibility` |

---

## 2. 核心数据模型

### 2.1 `Annotation` 结构 (src/Annotation.h)

```cpp
struct Annotation {
    AnnotationType type = AnnotationType::Unknown;
    int pageNo = -1;
    RectF bounds = {};          // 页面坐标系
    EngineMupdf* engine = nullptr;   // 所属引擎 (非拥有)
    pdf_annot* pdfannot = nullptr;   // MuPDF 原生标注对象 (非拥有)
};
```

设计要点:

- `Annotation` 是 `pdf_annot` 的**轻量抽象**, 头文件不包含 MuPDF 头 (`extern "C" struct pdf_annot;`), 隔离引擎细节。
- `engine` + `pdfannot` 构成全局唯一身份: **EngineMupdf 是 Annotation\* 列表的唯一所有者** (见 §2.3), 页面上的交互只持有指针引用。
- 类型转换 `AnnotationType` ↔ `pdf_annot_type` 按枚举顺序映射, 有 static 断言保护 (注释注明 "must match the order of pdf_annot_type enum in annot.h")。
- 所有标注属性读写 (颜色/文本/边框/图标/透明度/行端样式) 都封装为 `Annotation.{h,cpp}` 上的自由函数 (`GetColor` / `SetColor` / `Contents` / `Opacity` / …), 内部持有 `docLock` (exclusive) 调用 MuPDF。

### 2.2 `AnnotCreateArgs` 创建参数

创建标注时通过参数包传递, 而非逐属性调用:

```cpp
struct AnnotCreateArgs {
    AnnotationType annotType;   // 标注类型
    ParsedColor col;            // 主色 (描边)
    ParsedColor bgCol;          // 文本框背景色
    ParsedColor interiorCol;    // 形状填充色 (Square/Circle/Line)
    int opacity = 100;          // 透明度 0-100
    bool copyToClipboard = false;
    int textSize = -1;          // 文本框字号, <0 表示未指定
    int borderWidth = -1;       // 边框宽度, <0 表示未指定
    bool setContentToSelection = false;  // 用当前选中文本作为内容
    Str content;                // 标注内容 (备注文本)
    Str stampImage;             // Stamp: 编码图像字节 (如剪贴板 BMP), 按图像尺寸建标注
};
```

`stampImage` 字段支持"剪贴板图像 → Image Stamp"功能 (`CmdCreateAnnotImageFromClipboard`), 是近年新增的扩展点。

### 2.3 EngineMupdf 所有权模型

- `EngineMupdf` 维护每页 `FzPageInfo.pages[pageNo-1]`, 其中含 `annotations` 列表 (Annotation\* 容器)。
- `MarkNotificationAsModified` (EngineMupdf.cpp:5456) 注释明确了所有权契约:

  > EngineMupdf is the ultimate source of truth for Annotation\* list; all other places only get references to Annotation\* created inside EngineMupdf. On add/remove we update the list manually; on change we assume Annotation\* lives in the list.

- 所有 Annotation\* 生命周期由引擎统一管理: 添加/删除时更新列表, 修改时 Annotation\* 身份不变, 避免让 UI 层持有悬垂指针。
- `MakeAnnotationWrapper` (Annotation.cpp:1451 附近) 从 `pdf_annot` 构建包装并触发 `AnnotationChange::Add` 通知。

---

## 3. 标注生命周期实现

### 3.1 创建 (EngineMupdfCreateAnnotation)

流程: 命令 → `EngineMupdfCreateAnnotation(engine, pageNo, args)`:

1. 按 `args.annotType` 调用 MuPDF 创建原语 (`pdf_create_annot` / `pdf_create_annot_raw` 等), 在 `fz_try/fz_catch` 内执行。
2. 根据参数设置颜色 / 边框 / 透明度 / 图标 / 内容 (含 `setContentToSelection` 时从选区文本填充)。
3. `MakeAnnotationWrapper` 包装后加入 `FzPageInfo.annotations`, 触发 `MarkNotificationAsModified(Add)`。
4. UI 侧 `SetSelectedAnnotation(tab, annot, /*isNew=*/true)` 选中新标注并弹出编辑窗口 (focus=Edit)。

### 3.2 属性修改

编辑窗口属性更改直接走 `Annotation.cpp` 的 `Set*` API, 每个 Setter 内部:

```
ScopedSRWLockExclusive(&e->docLock)
  → pdf_annot_*_set(ctx, pdfannot, ...)   (fz_try/fz_catch 包裹, 失败时 fz_report_error)
  → MarkNotificationAsModified(e, annot, AnnotationChange::Modify)
```

`MarkNotificationAsModified` 的职责 (EngineMupdf.cpp:5456):

- 置 `e->modifiedAnnotations = true` (驱动"未保存标注"状态与 `CmdSaveAnnotations` 可用性);
- 维护引擎侧 Annotation 列表 (Add 追加 / Remove 移除 / Modify 校验存在);
- **递增 `pageInfo->annotDisplayListGeneration`** (原子计数) — 只使标注叠加层失效 (§4.2), 页面内容 displayList 不被触碰;
- **置 `pageInfo->hitIndexDirty = true`** — 空间命中索引在下次 `EngineMupdfGetAnnotationAtPos` 时惰性重建 (§4.5);
- 通过 `annot->engine->requestRerender = true` / 通知机制触发 UI 刷新 (提交 b361391bf 集成)。

### 3.3 删除 (DeleteAnnotation)

- `Annotation.cpp:611 DeleteAnnotation` 在 `docLock` 下调用 `pdf_delete_annot`, 引擎列表移除该指针, 触发 `AnnotationChange::Remove` 通知;
- UI 入口 `DeleteAnnotationAndUpdateUI(tab, annot)` (EditAnnotations.cpp) 负责关闭选中态、刷新编辑窗口列表、重渲染;
- `pdfannot` 被引擎置空, 其它位置的 stale 引用通过 `tab->selectedAnnotation` / `annotationUnderCursor` 的清理避免悬垂。

### 3.4 移动 (拖动)

Canvas.cpp 实现, 全 GDI 交互:

```
OnMouseLeftButtonDown 命中 GetAnnotationAtPos(pt)
  → StartAnnotationDrag: 记录 annotationBeingDragged / 原始矩形 / 偏移, DrawMovePattern 画棋盘拖影
  → OnMouseMove: 每次移动先 XOR 清掉旧拖影再在新位置画
  → OnMouseLeftButtonUp → StopDraggingAnnotation:
      仅允许同页内移动 → SetRect(annot, 新矩形) → OnAnnotationModified(win, annot, &origRect)
```

拖影用 `SetBrushOrgEx + PatBlt(PATINVERT)` 的闪烁动画, 避免直接改动标注本身。

### 3.5 调整大小 (8 向手柄)

- `ResizeHandle` 枚举 (TopLeft/Top/…/Left) + `GetResizeHandleAt` 命中检测 (kResizeHandleSize = 8px 命中区, 角优先于边);
- 光标按方向切换 (IDC_SIZENWSE / SIZENS / SIZEWE …);
- `StartAnnotationResize` → 拖动中实时 `SetRect` (在 `annotationOriginalRect` 基础上 clamp) → `StopAnnotationResize` 落定并走统一失效;
- 可 resize 的类型有白名单判断 (`AnnotationCanBeResized(type)`)。

### 3.6 统一失效钩子 `OnAnnotationModified` (Canvas.cpp:862)

所有 create / modify / delete / move / resize 操作**必须**经由该钩子收口:

```
static void OnAnnotationModified(MainWindow* win, Annotation* annot, RectF* knownBounds) {
    RectF invalidation = knownBounds ? *knownBounds : GetRect(annot); // 旧矩形
    RectF newBounds = GetRect(annot);
    invalidation = invalidation.Union(newBounds);                     // 旧 ∪ 新 (双矩形)
    if (invalidation.IsEmpty()) invalidation = engine->PageMediabox(pageNo); // 零面积回退整页
    gRenderCache->Invalidate(dm, pageNo, invalidation);               // 瓦片级失效
    NotifyAnnotationsChanged(win->CurrentTab()->editAnnotsWindow);
    MainWindowRerender(win);
    ToolbarUpdateStateForWindow(win, true);              // 刷新保存/编辑按钮可用态
}
```

移动/resize 时**新旧矩形取并集**失效: 旧位置残留像素与新位置新像素都清掉, 大页面上移动不再依赖整页回退 (报告 §9 已知问题 #1 → 已修复)。

## 4. 渲染与缓存失效

### 4.1 内容层与标注层分离渲染 (MuPDF displayList)

**标注不再随页面内容一起重渲染** — 页面内容与标注层缓存为两条独立 displayList, 绘制时按 contents → annots → widgets 顺序回放 (与 `pdf_run_page_with_usage` 的绘制次序一致):

```
DrawDocument (Canvas.cpp:2019)
  → gRenderCache->Paint(hdc, bounds, dm, pageNo, pi, ...)   // 每页瓦片渲染
    → EngineMupdf::RenderPage:
        keptList      = GetOrBuildContentDisplayList(...)   // 页面内容流, 常驻缓存
        keptAnnotList = GetOrBuildAnnotDisplayList(...)     // 标注 + 表单控件叠加层
        fz_run_display_list(dev, keptList)                  // 内容
        fz_run_display_list(dev, keptAnnotList)             // 标注/控件在上
```

- `GetOrBuildContentDisplayList` (EngineMupdf.cpp) 用 `pdf_run_page_contents_with_usage(..., "View", ...)` 构建, **一旦建成永不因标注编辑失效** — 这是 P1 的核心: 修改标注不再重跑页面内容流 (报告 §9 已知问题 #3 → 已修复);
- `GetOrBuildAnnotDisplayList` 用 `pdf_run_page_annots_with_usage` + `pdf_run_page_widgets_with_usage` 构建叠加层, 依据 `annotDisplayListGeneration` 与 `annotGeneration` 是否一致决定是否重建 (见 §4.2);
- 两条列表都缓存在 `FzPageInfo`, 由 `renderLock` (CriticalSection) 串行化构建, 防止多线程同时调 MuPDF; 回放时在 `docLock` Shared 保护下进行, 杜绝 lazy-decode 期间的悬垂访问;
- 非 PDF 文档 (DjVu/图片等) 无标注层, `keptAnnotList == nullptr`, 直接以内容层为完整图像;
- 若任一 displayList 构建失败, 退回到 `fz_run_page_with_usage` 的整页直绘路径 (标注随页面内容一起渲染)。

### 4.2 annotDisplayListGeneration 失效机制

```
修改标注 (Set* / move / resize / delete)
  → MarkNotificationAsModified → pageInfo->annotGeneration++   (std::atomic)
  → 下一帧 GetOrBuildAnnotDisplayList:
      if (annotDisplayList && annotDisplayListGeneration != annotGeneration)
          fz_drop_display_list → annotDisplayList = nullptr     // 只重建标注叠加层
      annotDisplayListGeneration = annotGeneration.load()
```

要点:

- `annotGeneration` / `annotDisplayListGeneration` 为 `std::atomic`, 消除标注渲染多线程数据竞争 UB (commit 9c9a8e548);
- **页面内容 displayList 不参与失效**: 标注编辑只重建叠加层 (§4.1), 其它页缓存保持有效;
- `GetOrBuildAnnotDisplayList` 的构建/重建在 `fz_try/fz_catch` 中包裹, 失败时报告错误并返回空列表 (渲染回退到整页直绘路径, 而非崩溃)。

### 4.3 RenderCache 瓦片级失效

`RenderCache::Invalidate(dm, pageNo, rect)` (RenderCache.cpp:486):

1. `ScopedCritSec(&requestAccess)` 下清空该页未完成渲染请求 + Abort 当前线程正在渲染的同页请求;
2. `ScopedCritSec(&cacheAccess)` 下遍历缓存条目, 与目标矩形相交的瓦片置 `zoom = kInvalidZoom; outOfDate = true`;
3. 命中 `GetTileRect(mediabox, tile)` 判定瓦片覆盖范围, 实现**部分失效** (移动标注只清受影响瓦片, 不整页重渲染);
4. `OnAnnotationModified` 将修改前的旧矩形 `knownBounds` 与 `GetRect(annot)` 新矩形**取并集**失效, 移动/resize 时新旧区域都被清掉 (见 §3.6), 大页面不再残留"残影"。

渲染线程在 `Paint` 时拒绝过时条目 (`zoom == kInvalidZoom`, RenderCache.cpp:219-221), 并触发重渲染。

### 4.4 选中态覆盖层 (D2D/GDI+ 双路径)

选中标注的可视化标记**不走 MuPDF**, 由 Canvas 在页面上方绘制:

```
DrawDocument 末尾:
  → PaintAnnotationOverlaysGPU(win, hdc, dm)     // D2D 加速: 虚线边框 / resize 手柄 / 右键高亮
  → 失败回退: PaintCurrentEditAnnotationMark(tab, hdc, dm)  // GDI+ 路径
```

- resize 手柄: 8px 命中区 (`kResizeHandleSize`), 角优先, 按方向换光标 (`GetCursorForResizeHandle`);
- 右键上下文菜单高亮矩形: `contextMenuHighlightPageNo/rect`;
- 拖影 `DrawMovePattern`: `PatBlt(PATINVERT)` 棋盘格, 移动时闪烁反馈。

### 4.5 空间命中测试索引 (AnnotHitIndex)

`src/base/AnnotHitTest.{h,cpp}` 提供每页的网格 (uniform grid) 空间索引, 替代原先 `EngineGetAnnotationAtPos` 的全量线性扫描 (报告 §9 已知问题 #2 → 已修复):

- **构建**: `BuildHitIndexes` (EngineMupdf.cpp) 将 `FzPageInfo.annotations` / `widgets` 的 `RectF bounds` 登记到 `ceil(sqrt(n))` 等宽网格; 超出页面的条目 clamp 进边缘格 (保持可命中); 零面积条目跳过 (与旧逻辑的 `RectF::Contains` 拒绝一致);
- **查询**: `AnnotHitIndex::HitTest(pos, preferred)` 只查命中点所在格 — 候选为该格内条目 (同时遍历覆盖该格的跨格条目), **语义与旧线性扫描完全一致**: 命中 = `bounds.Contains(pos)`; `preferred` 含点则立即返回 (拖动优先); 否则选**面积最小**者, 面积相同取列表序靠前者;
- **失效**: 命中索引与 `annotGeneration` 无关, 而是经 `hitIndexDirty` 惰性重建 — `MarkNotificationAsModified` 在 Add/Remove/Modify (可能改矩形) 时统一置脏, 下次 `EngineMupdfGetAnnotationAtPos` / `GetWidgetAtPos` 时重建; 无 UI 线程额外开销;
- **访问线程**: 索引构建与查询都在 UI 线程且持有 `docLock` Shared, wrapper 列表本身有 pagesLock 保护, 与渲染线程的 displayList 无共享状态;
- **测试**: `src/base/tests/AnnotHitTest_ut.cpp` (无头, 随 `test_util.exe` 运行) 覆盖命中正确性、preferred 优先、最小面积/平局、边缘 clamp、零面积/退化页、1000 条稠密页的候选数上界。

---

## 5. 并发与线程安全

### 5.1 锁层级 (pagesLock → docLock → renderLock)

```
pagesLock (CriticalSection)     — FzPageInfo 数组 / 页面信息缓存
  → docLock (SRWLock)           — MuPDF document 操作 (标注读写), Shared/Exclusive
      → renderLock (CS)         — displayList 构建/渲染, 页面级串行
```

- `ScopedSRWLockExclusive(&e->docLock)` 包裹所有 `pdf_annot_*` 读写 (Annotation.cpp 全部 Get/Set);
- `GetFzPageInfo` 内锁定顺序为 pagesLock → docLock [Shared] → renderLock, **禁止**在持有 renderLock 时再获取 docLock (会反转层级造成死锁);
- `g_tlsCritSecDepth` TLS 深度断言: DEBUG 构建检测嵌套 CriticalSection 违规 (如 `GetAnnotations` 曾嵌套 pagesLock 触发 `g_tlsCritSecDepth assertion`);
- 标注属性 Getter/Setter 的 `fz_try/fz_catch` 失败路径均释放锁再报告错误, 无锁泄漏。

### 5.2 历史并发缺陷修复清单 (git log 追溯)

| commit | 修复内容 |
|---|---|
| 8bd1b027d | EngineMupdf 锁重构: 解决标注相关死锁 / use-after-free / UI 挂起 |
| b361391bf | 标注失效 + 锁安全 UI 回调 (OnAnnotationModified 雏形) |
| 3170e1af1 | `GetFzPageInfo` 补 dimensionsLoaded 回退, 修复页面压缩 (缩放前渲染未就绪) |
| 2fc241a76 | `EngineMupdfGetAnnotations` 去掉嵌套 pagesLock (g_tlsCritSecDepth 断言) |
| 792653c25 | `RenderPage` 移除引发饥饿的版本检查; `GetAnnotations` 改 loadQuick=true (避免 UI 线程长锁) |
| 301f08258 | `HandleLinkMupdf` / `MarkNotificationAsModified` 消除 CS→SRW 锁序断言 |
| 9c9a8e548 | `annotGeneration`/`displayListGeneration` → `std::atomic` (消除数据竞争 UB) |
| 464002069 | 修复引擎销毁时 Annotation wrapper 泄漏 |

## 6. 编辑窗口 (EditAnnotationsWindow)

UI 层 (src/EditAnnotations.{h,cpp}) 提供标注编辑面板:

```
EditAnnotations.h 公开 API:
  ShowEditAnnotationsWindow(WindowTab*, Annotation*, EditAnnotFocus = Default)
  CloseAndDeleteEditAnnotationsWindow(WindowTab*)
  DeleteAnnotationAndUpdateUI(WindowTab*, Annotation*)
  SetSelectedAnnotation(WindowTab*, Annotation*, bool isNew = false, EditAnnotFocus = Default)
  UpdateAnnotationsList(EditAnnotationsWindow*)
  NotifyAnnotationsChanged(EditAnnotationsWindow*)
```

- `EditAnnotFocus` 三种焦点预设: `Default` / `Edit` (新建标注后直接聚焦文本编辑框) / `List`;
- 窗口按标注类型展示属性控件 (颜色 / 边框宽度 / 透明度 / 图标 / 行端样式 / 文本内容), 更改即触发 §3.2 的 Setter → `MarkNotificationAsModified`;
- `SetSelectedAnnotation(..., isNew=true)` 在创建后自动打开编辑窗口并把焦点放到内容编辑框, 形成"画完即编辑"的流畅体验;
- 选中态与右键菜单联动 (`ContextMenu.cpp` 中 "编辑标注"/"删除标注" 等项), 删除走 `DeleteAnnotationAndUpdateUI` 统一刷新。

---

## 7. 命令面板集成 (基于最新 CommandAvailability 实现)

命令面板 (CommandPalette) 通过 `CommandAvailability::GetCommandVisibility` 与 `CommandShouldShow` 决定每类数据源中命令是否出现, **标注命令的可见性完全由上下文驱动**。

### 7.1 可见性规则 (CommandAvailability.cpp)

`AppCommandCtx` 中标注相关的上下文字段:

```
ctx.supportsAnnots        = EngineSupportsAnnotations(engine) && !win->isFullScreen;
                           // CanAccessDisk() == false 时强制 false
ctx.hasUnsavedAnnotations = EngineHasUnsavedAnnotations(engine);
ctx.annotationUnderCursor = dm->GetAnnotationAtPos(cursorPos, nullptr);
```

标注命令的判定 (GetCommandVisibility, 按顺序):

| 命令组 | 规则 | 面板结果 |
|---|---|---|
| `CmdCreateAnnotFirst..Last` (所有创建命令) | `!supportsAnnots` → Hide | 无标注能力时整组隐藏 |
| `CmdCreateAnnotHighlight/Squiggly/StrikeOut/Underline` | 另在 `disableIfNoSelection[]` 中, 无选区时 Disable | 面板中 Disable→Hide (仅 `>CmdCreateAnnotText` 等点击放置类无需选区) |
| `CmdDeleteAnnotation` | `!annotationUnderCursor` → Disable | 光标不在标注上时隐藏 |
| `CmdSaveAnnotations` / `CmdSaveAnnotationsNewFile` | `hasUnsavedAnnotations` → Show, 否则 Disable | 无未保存标注时隐藏 |
| `CmdShow/Hide/ToggleShowAnnotations`、`CmdEditAnnotations`、`CmdSaveAnnotations*` | `removeIfAnnotsNotSupported[]`, `!supportsAnnots` → Hide | 无标注能力时隐藏 |
| 全部 Disable 状态 | `MapForSurface`: Palette 下 `Disable→Hide` (菜单里灰显, 面板里直接不出现) | — |

`ctx.supportsAnnots` 额外叠加两个硬约束: **全屏模式**下禁用 (与演示模式语义一致), 以及**无磁盘访问权限** (`!CanAccessDisk()`) 时禁用 (标注需要落盘保存)。

### 7.2 toggle 动态名称与数据流

命令面板收集阶段 (`CommandPaletteCollect.cpp` `UpdateCommandNameTemp`) 对 toggle 命令动态改写显示名:

```cpp
case CmdToggleShowAnnotations: {
    WindowTab* tab = win->CurrentTab();
    if (tab) {
        isToggle = true;
        newIsOn = tab->hideAnnotations;   // 显示 "Toggle Show Annotations: set to true/false"
    }
} break;
```

配合 `CollectStrings` 的过滤链:

```
CollectStrings(win)
  → AllowCommand(ctx, cmdId) = CommandShouldShow(GetCommandVisibility(cmdId, ctx, Palette))
  → 内置命令逐条过滤 (CmdCreateAnnot* 等随上下文出现/消失)
  → UpdateCommandNameTemp 改写 toggle 名 (CmdToggleShowAnnotations 动态显示当前隐藏态)
  → 排序后进入列表
```

执行时走 `ExecuteCurrentSelection` → 命令分派; 标注创建命令执行后由 `SetSelectedAnnotation(tab, annot, /*isNew=*/true)` 自动选中并聚焦编辑窗口的文本编辑框 (实现"画完即编辑"), 与 `gCommandsNoActivate` (设置类命令执行后不抢焦点) 语义不同。

---

## 8. 测试覆盖

| 测试 | 文件 | 覆盖点 |
|---|---|---|
| 标注渲染缓存稳定性 | `tests/issue-annot-cache.ts` (fixture: 2 页 11 标注) | 混合类型标注多页渲染不崩溃; 6 轮 open/render/close 无陈旧缓存; 渲染 + TOC + 搜索并行路径 |
| 标注锁序 / 死锁 | `tests/issue-annot-locking.ts` (fixture: 3 页) | pagesLock→docLock→renderLock 无死锁; 并行渲染+TOC+目标解析 (path C); 延迟文本提取路径; 5 轮快速开合验证 EngineBase drain & join |
| 显示模式切换 + 标注 | `tests/issue-display-mode-annot.ts` (10 轮) | SinglePage/Continuous/Facing/BookView 切换带标注不崩溃; SetDisplayMode 后 Invalidate 无陈旧缓存; 标注 + 模式切换重入安全 |
| 页面几何/滚动 | `tests/issue-page-geometry-scroll.ts` | D2D 源矩形拉伸 + 标注 generation atomic 联动 (commit 9c9a8e548) |
| 剪贴板图像戳 (ad-hoc) | `tests/ad-hoc-paste-image-annot.ts` | CmdCreateAnnotImageFromClipboard 放红色位图 → 校验页面被盖章 (涉及系统剪贴板, 不入 all.ts) |
| 命中测试空间索引 (无头) | `src/base/tests/AnnotHitTest_ut.cpp` | 命中正确性 / preferred 优先 / 最小面积 + 平局 / 边缘 clamp / 零面积与退化页 / 1000 条目稠密页候选数上界 (`bun cmd/run-unit-tests.ts -dbg`) |
| 锁层无头单测 | `src/base/tests/InputScrolling_ut.cpp` 等 | 纯逻辑部分 (标注相关锁/渲染无 GUI 依赖) |

运行方式: `bun tests/issue-annot-cache.ts` / `bun tests/issue-annot-locking.ts` / `bun tests/issue-display-mode-annot.ts` (均带 `--no-build` 选项); 完整回归 `bun tests/all.ts`。

---

## 9. 已知问题与风险

1. ~~**移动/resize 的失效范围保守**~~ → **已修复**: `OnAnnotationModified` 现以旧矩形 ∪ 新矩形取并集失效 (见 §3.6 / §4.3), 大页面移动标注不再残留"残影"。
2. ~~**标注命中测试 O(n)**~~ → **已修复**: 每页空间网格索引 (`AnnotHitIndex`, §4.5) 将命中复杂度降为 O(该格候选数), `EngineMupdfGetAnnotationAtPos` / `GetWidgetAtPos` 已接入, 有独立无头单测。
3. ~~**MuPDF 标注渲染重**~~ → **已修复**: 内容流与标注层分离缓存 (见 §4.1), 修改标注只重建标注叠加层 (`GetOrBuildAnnotDisplayList`), 页面内容 displayList 常驻, 大页面 + 复杂内容不再整页重跑。
4. **编辑窗口为传统 Win32 布局**: `EditAnnotationsWindow` 用固定控件布局, 未迁移到约束布局引擎; 高 DPI / 窄窗口下布局可能局促 (属 UI 现代化范围, 见 `docs/UI_REPORT.md`)。
5. **全屏模式下标注禁用**: `supportsAnnots` 在 `isFullScreen` 时强制 false, 与演示模式语义一致但会让全屏观众模式下无法快速标注 — 需确认产品意图。
6. ~~**`ValidateAnnotationsInSync` 空实现**~~ → **已实现**: EngineMupdf.cpp 中的一致性校验现会对比 MuPDF 标注计数与 wrapper 列表计数, 不一致时日志 + ReportIf; 默认由 `gSkipAnnotatoinValidation` 关闭以零开销, 排查回归时可临时开启。

---

## 10. 改进建议

| 优先级 | 建议 | 状态 |
|---|---|---|
| P1 | 移动/resize 失效改双矩形 (旧 rect ∪ 新 rect) | **已实施** (§3.6 / §4.3): `OnAnnotationModified` 并集失效, 消除大页面残影 |
| P1 | 标注层独立 displayList (`pdf_run_page_annots_with_usage` / `pdf_run_page_widgets_with_usage` 合成叠加层) | **已实施** (§4.1 / §4.2): 内容流常驻缓存, 修改标注只重建标注叠加层, 不重跑页面内容 |
| P2 | 标注命中测试空间索引 (按页网格) | **已实施** (§4.5): `AnnotHitIndex` 均匀网格 + 惰性重建 + 无头单测, 语义与旧线性扫描一致 |
| P2 | 编辑窗口迁移约束布局引擎 | 未实施: 与 TabsCtrl/Notifications 等控件的 D2D/布局现代化保持一致 (属 UI 现代化路线, 见 `docs/UI_REPORT.md`) |
| P3 | 实现 `ValidateAnnotationsInSync` | **已实施** (§9 #6): 计数交叉校验 + ReportIf, 默认关闭零开销 |

---

## 附录 A: 相关文件索引

| 文件 | 职责 |
|---|---|
| `src/Annotation.h` / `src/Annotation.cpp` | 标注抽象 + 属性读写 API (含 `EngineMupdfCreateAnnotation` / `DeleteAnnotation`) |
| `src/EditAnnotations.h` / `src/EditAnnotations.cpp` | 编辑窗口 + 标注选中/删除 UI 集成 |
| `src/EngineMupdf.{h,cpp}` | MuPDF 封装: FzPageInfo / displayList / annotGeneration / 锁 |
| `src/Canvas.cpp` | 移动/resize/命中/选中态覆盖层/`OnAnnotationModified`/标注悬停通知 |
| `src/RenderCache.cpp` | 瓦片缓存 + `Invalidate` + 过时条目拒绝 |
| `src/CommandAvailability.cpp` | 标注命令可见性规则 (命令面板 + 菜单共用) |
| `src/CommandPaletteCollect.cpp` | 命令收集 + toggle 动态名称 (`CmdToggleShowAnnotations`) |
| `tests/issue-annot-*.ts` | 标注渲染/锁/显示模式测试 |
| `docs/reports/annot-render-crash-analysis.md` | 渲染崩溃根因历史分析 |


