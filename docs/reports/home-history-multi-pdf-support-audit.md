# Home 历史列表、多文档稳定性与 PDF 兼容性调查报告

> 调查日期：2026-09-25
> 调查基线：`3742f7aa9`
> 调查方式：只读代码、文档、测试和 MuPDF 集成审查；本报告不声称已经在当前机器上复现所有最终崩溃。

## 1. 执行摘要

本报告调查三个问题：

1. Home 页面打开历史过多时缺少可见滚动条；
2. 打开过多 PDF、窗口或后台任务时的崩溃、挂死和资源耗尽风险；
3. 当前实现对不同 PDF 版本、加密、签名、表单、可访问性和工具链的支持范围。

结论如下：

| 项目                       | 结论                                                                                     | 严重度 |
| -------------------------- | ---------------------------------------------------------------------------------------- | ------ |
| Home 历史列表缺少滚动条    | 根因已确认：默认 `windows` 模式只实现了 overlay 滚动条，Home 没有恢复原生滚动条          | 中     |
| 打开过多 PDF 导致崩溃/挂死 | 没有统一内存预算、后台线程无界、OOM 路径直接崩溃、关闭/重载存在等待和生命周期风险        | 高     |
| PDF 版本和格式支持         | PDF 1.x 核心能力较完整；PDF 2.0 部分支持；PDF 2.1、XFA、PDF/UA、权限和签名互操作仍不完整 | 中到高 |

## 2. Home 页面打开历史缺少滚动条

### 2.1 根因

Home 页面已经正确计算内容高度和溢出范围：

- `src/HomePage.cpp:1577-1612`
  - `thumbsContentDy` 是列表或缩略图内容高度；
  - `thumbsVisibleDy` 是可视区域高度；
  - `maxScrollY` 是最大滚动位置。

但绘制结束时只在 overlay 模式下创建滚动条：

```cpp
bool showScrollbarV = ScrollbarsUseOverlay() &&
    l.totalContentDy > l.thumbsVisibleDy;
```

位置：`src/HomePage.cpp:2616-2634`。

`ScrollbarsUseOverlay()` 只对 `smart` 和 `overlay` 返回真：

- `src/SumatraPDF.cpp:1498-1505`

默认设置却是：

- `src/Settings.h:1876`
- `Scrollbars = windows`

非文档页面还会隐藏原生滚动条：

- `src/SumatraPDF.cpp:2140-2148`
- `src/SumatraPDF.cpp:5188-5191`

因此默认配置下会出现：

- 列表内容确实溢出；
- 鼠标滚轮、键盘和合成 `WM_VSCROLL` 仍能滚动；
- 右侧既没有原生 `WS_VSCROLL`，也没有 overlay 滚动条。

### 2.2 复现条件

可参考 `tests/issue-5870-list-dirs.ts:54-71`：

1. 设置 `Scrollbars = windows`；
2. 设置 `HomePageViewMode = list`；
3. 开启 `ShowStartPage`；
4. 准备约 30 条有效打开历史；
5. 使用较小窗口，例如 `900x700`；
6. 启动后直接进入 Home 页面。

现象是内容超过可视区域，但没有可见滚动条；滚轮仍能移动列表。

### 2.3 修复建议

不要简单地把 Home 永久改成 overlay 模式，因为这会违背用户的 `windows` 设置。

建议集中实现 Home 滚动条状态同步：

- `windows`：使用 `WS_VSCROLL`、`SetScrollInfo()` 和 `SIF_DISABLENOSCROLL`；
- `smart/overlay`：使用现有 `OverlayScrollbar`；
- `hidden`：隐藏两种滚动条；
- Home 首次布局、窗口 resize、DPI/RTL 改变、历史数量改变和模式切换时重新同步。

需要同时处理：

- `HomePageOnVScroll()` 当前优先使用 `overlayScrollV->nTrackPos`，见 `src/HomePage.cpp:2977-3017`；切换到原生滚动条后不能继续依赖 overlay 状态；
- `HIWORD(wp)` 当前按有符号 `short` 解释，见 `src/HomePage.cpp:2996-3003`，大范围原生滚动条应使用无符号解释；
- 大量历史可能超过 16 位滚动范围，最好按行/页映射，而不是直接传像素高度；
- 避免在绘制线程中直接触发 `ShowScrollBar()`，因为该操作可能引发 `WM_SIZE` 重入，已有说明见 `src/SumatraPDF.cpp:1475-1486`。

## 3. 打开过多 PDF 时的崩溃和资源风险

本轮没有实际复现最终崩溃，但以下风险可以由当前代码的控制流和资源生命周期直接证明。

### 3.1 RenderCache 只限制条目数，不限制内存

当前限制是：

```cpp
#define MAX_BITMAPS_CACHED 128
```

位置：`src/RenderCache.h:11-16`。源码注释已经指出应按内存而非条目数限制。

风险包括：

- 128 个大 tile 仍可能占用大量内存；
- 同一页可能同时保留 CPU DIB 和 D2D bitmap；
- D2D 上传还会额外分配完整 staging buffer；
- 渲染过程中的临时 pixmap 不受 128 条限制。

相关位置：

- `src/RenderCache.cpp:107-125`
- `src/RenderCache.cpp:296-369`
- `src/GpuBackend.cpp:217-305`
- `src/base/Pixmap.h:51-69`

### 3.2 每个 PDF 都有独立 MuPDF store

`EngineMupdf` 为每个文档创建独立 context：

- `src/EngineMupdf.cpp:3350-3357`

MuPDF `FZ_STORE_DEFAULT` 为 256 MiB 阈值：

- `ext/mupdf/include/mupdf/fitz/context.h:310-322`

store 是每个 engine 一份，而不是每个线程一份。页面对象、stext、display list 和图片引用通常要到 engine 析构才释放：

- `src/EngineMupdf.h:22-80`
- `src/EngineMupdf.cpp:3375-3446`
- `src/EngineMupdf.cpp:5749-5795`
- `src/EngineBase.h:642-645`

打开很多不同 PDF 并访问大量页面后，资源可能线性叠加；切换到后台标签不会主动释放 engine 资源。

### 3.3 加载完成后的后台线程没有统一上限

加载阶段最多约 4 个线程：

- `src/SumatraPDF.cpp:3988-4105`

但加载完成后还存在没有全局上限的线程：

- 缩略图生成：`src/SumatraPDF.cpp:1283-1319`
- 每个 MuPDF 文档的 heading TOC：`src/DisplayModel.cpp:612-624`、`src/EngineMupdf.cpp:5085-5110`
- hover 文本提取：`src/Canvas.cpp:1761-1765`、`src/EngineBase.cpp:468-509`

渲染线程默认值写成 8，但实际计算为 `max(8, CPU 数)`，最多 32：

- `src/RenderCache.cpp:107-125`
- `src/RenderCache.h:122`

### 3.4 OOM 路径会主动终止进程

全局 `new` handler 在分配失败时调用 `CrashMe()`：

- `src/CrashHandler.cpp:913-920`
- `src/CrashHandler.cpp:1035-1043`

部分分配路径没有统一的失败处理：

- Arena placement-new：`src/base/Arena.h:165-170`
- MuPDF 临时 bitmap 分配：`src/EngineMupdf.cpp:1822-1874`

因此大量 PDF 导致 heap、commit 或 GDI 资源耗尽时，应用可能直接崩溃，而不是拒绝新文档或回退到低内存模式。

### 3.5 瞬态渲染失败会永久标记页面失败

`RenderPage()` 返回空结果时设置 `errorCode=1`：

- `src/RenderCache.cpp:1294-1313`

`DisplayModel` 随后设置 `failedToRender=true`：

- `src/DisplayModel.cpp:305-316`

Canvas 后续直接显示“Could not render page”，不再进入正常渲染路径：

- `src/Canvas.cpp:3311-3325`

如果失败原因只是临时内存或 GDI 压力，而不是 PDF 损坏，关闭其他窗口后页面仍可能永久显示失败。

### 3.6 关闭活动渲染的标签可能无限等待

`CancelRenderingBlocking()` 等待活动渲染完成，没有超时：

- `src/RenderCache.cpp:966-991`

关闭路径会另建线程等待 render：

- `src/SumatraPDF.cpp:1393-1453`

如果图片解码或第三方 codec 长时间不返回，engine、页面缓存和 bitmap 会一直存活；连续关闭多个标签还会增加等待线程。

### 3.7 Reload 存在新旧资源重叠

`ReloadDocument()` 在旧 controller 仍存在时创建新 controller：

- `src/SumatraPDF.cpp:2705-2804`

切换到新 controller 后才删除旧 controller：

- `src/SumatraPDF.cpp:2364-2387`
- `src/SumatraPDF.cpp:2498-2520`

reload 期间可能同时存在两套 engine、MuPDF store、页面缓存和渲染任务。加密 PDF 的密码对话框还会泵消息，窗口或标签可能在对话框期间被关闭，存在旧指针继续被使用的风险：

- `src/GetPasswordDialog.cpp:226-268`
- `src/SumatraPDF.cpp:2065-2100`
- `src/SumatraPDF.cpp:2801-2829`

### 3.8 context clone 失败时可能错误共享主 context

`fz_clone_context()` 失败后回退到 engine 主 `_ctx`：

- `src/EngineMupdf.cpp:3278-3296`

随后 `RenderPage()` 在取得 `renderLock` 前修改 AA 和 min-line-width 状态：

- `src/EngineMupdf.cpp:6670-6724`
- `src/PdfCadEnhanceDevice.cpp:433-449`

如果多个线程同时走到该回退路径，可能产生共享 context 数据竞争。clone context 加入全局列表时的追加失败也缺少完整清理：

- `src/EngineMupdf.cpp:3291-3302`

### 3.9 多窗口画布 buffer 没有总预算

每个窗口都有自己的完整画布 DIB：

- `src/MainWindow.cpp:368-415`
- `src/base/Win.cpp:2648-2664`

窗口最小化时只是跳过 resize，不会释放旧 buffer：

- `src/Canvas.cpp:5734-5750`

打开大量独立窗口时，这些 buffer 不受 RenderCache 的 128 条限制。

## 4. PDF 版本和格式支持情况

当前 MuPDF 版本为 `1.28.2`：

- `ext/versions.txt:3-5`

### 4.1 支持矩阵

| 能力                         | 当前状态     | 说明                                                                            |
| ---------------------------- | ------------ | ------------------------------------------------------------------------------- |
| PDF 1.x 阅读、渲染、文本提取 | 条件支持     | 核心路径完整，但缺少全面标准合规矩阵                                            |
| PDF 2.0                      | 部分支持     | MuPDF 能识别 2.0，并有缩写键、结构类型、AES-256 R6 等原语；应用层没有一致性验证 |
| PDF 2.1                      | 仅未验证     | 通常可容错打开，但没有专门实现、文档声明或测试矩阵                              |
| XFA                          | 不支持       | 仅支持部分 AcroForm 层；纯 XFA/hybrid XFA 动态字段不支持                        |
| Standard password encryption | 条件支持     | 支持 RC4/AES 常见组合                                                           |
| Public-key encryption        | 不支持       | 非 `/Standard` handler 会报 unknown encryption handler                          |
| PDF 权限执行                 | 不完整       | 主要执行打印和复制权限，编辑、批注、表单、组装等缺少统一门控                    |
| 数字签名查看                 | 条件支持     | 可读取摘要、证书和部分 PAdES 信息，但不做完整吊销验证                           |
| 新建数字签名                 | 条件支持     | 支持 PFX/P12、Windows 证书库和 detached PKCS#7；无完整 TSA/证书链保证           |
| PDF/A、PDF/X                 | 仅识别元数据 | 属性页可显示相关标记，但没有合规验证                                            |
| PDF/UA、Tagged PDF           | 不完整       | 有平面文本 UIA，没有完整 tag tree/PDF-UA 语义树                                 |
| 线性化读取                   | 支持         | 可检测 Fast Web View                                                            |
| 线性化写出                   | 不支持       | MuPDF 明确标记 linearize 不再支持                                               |
| malformed PDF 修复           | 条件支持     | 依赖 MuPDF 自动修复，应用没有明确显示“已修复”状态                               |

### 4.2 PDF 2.0/2.1

MuPDF 版本扫描逻辑：

- `ext/mupdf/source/pdf/pdf-xref.c:948-1007`

PDF 2.0 被接受；PDF 2.1 通常可以打开，但可能产生未知版本警告。当前没有 PDF 2.1 专用 fixture 或互操作测试。

已存在的 PDF 2.0 原语包括：

- `ext/mupdf/source/pdf/pdf-object.c:2489-2497`
- `ext/mupdf/source/pdf/pdf-run.c:474-584`
- `ext/mupdf/source/pdf/pdf-crypt.c:1355-1437`

这些只能说明解析器具备部分能力，不能等同于完整支持 PDF 2.0。

属性页显示的 PDF/A、PDF/X 标记来自文件元数据：

- `src/SumatraProperties.cpp:436-464`

这属于识别/展示，不是合规性验证。

### 4.3 XFA 和 AcroForm

应用会检测 `Root/AcroForm/XFA`：

- `src/EngineMupdf.cpp:4606-4610`
- `src/EngineMupdf.cpp:7531-7535`

并在 `src/SumatraPDF.cpp:2627-2643` 显示不支持提示。

当前 AcroForm 对文本框、复选框、单选框、组合框、列表框和部分 JavaScript 有支持，但以下能力不完整：

- XFA-only 动态字段；
- 任意 push-button JavaScript；
- Barcode 字段；
- rich text；
- 完整 editable combobox；
- 多选列表；
- 完整字段类型填写、保存、重开矩阵。

计划文档也明确将 XFA-only、数字签名等列为限制：

- `docs/pdf-form-filling-plan.md:8-53`

### 4.4 加密和权限

MuPDF 只接受 Standard security handler：

- `ext/mupdf/source/pdf/pdf-crypt.c:69-96`

应用层目前只保存并执行少数权限：

- `src/EngineMupdf.cpp:4511-4512`
- `src/EngineBase.h:512-513`
- `src/Print.cpp:1385-1389`
- `src/Selection.cpp:545-564`

未发现对 `FZ_PERMISSION_EDIT`、`FZ_PERMISSION_ANNOTATE`、`FZ_PERMISSION_FORM`、`FZ_PERMISSION_ASSEMBLE` 的统一应用层门控。因此带有权限限制的 PDF 在批注、表单修改、签名和保存路径上可能表现不一致。

### 4.5 数字签名

现有签名检查代码可以读取 signer、摘要、证书链状态、哈希算法、issuer、有效期、部分 CAdES/PAdES 属性、时间戳和 DSS 信息。

但：

- `CertGetCertificateChain()` 不做吊销检查：
  - `ext/mupdf/source/helpers/pkcs7/pkcs7-windows.c:267-272`
- 只完整处理 signer index 0；
- DSS 存在不等于完成长期验证；
- 新签名没有 TSA 和完整证书链嵌入保证；
- 自动测试主要使用临时自签名证书。

因此当前更适合描述为“可查看和创建部分签名”，而不是完整支持 PAdES/PDF 签名互操作。

### 4.6 可访问性

当前 UIA 主要提供平面文本树：

- `src/uia/DocumentProvider.cpp:223-280`
- `src/uia/PageProvider.cpp:168-251`
- `src/uia/TextRange.cpp:556-642`

缺少：

- PDF tag tree 到 UIA 的语义映射；
- Heading/Paragraph/Table/Figure；
- Artifact；
- Alt text；
- Link role；
- PDF/UA 阅读顺序；
- 扫描 PDF OCR。

### 4.7 线性化、修复和工具链

- 可读取/检测线性化文件：`src/EngineMupdf.cpp:4367-4384`
- 属性页显示 Fast Web View：`src/SumatraProperties.cpp:436-453`
- 线性化写出不支持：
  - `ext/mupdf/source/pdf/pdf-write.c:2907-2914`
  - `ext/mupdf/source/tools/pdfclean.c:23-32`

GUI 压缩/解压：

- `src/PdfTools.cpp:476-565`

存在两个明显缺口：

1. GUI 调用 `pdfclean` 时没有传入已保存的 PDF 密码；
2. 压缩/解压在 UI 线程同步执行，大文件会阻塞界面。

MuPDF 能自动修复部分损坏 PDF，但应用没有调用 `pdf_was_repaired()` 来明确显示修复状态，修复后的保存限制也缺少专门测试。

## 5. 建议的修复顺序

### 第一阶段：稳定性

1. 修复 Home `windows` 模式原生滚动条。
2. 为 RenderCache 增加字节预算、in-flight 预算和动态内存压力处理。
3. 为 thumbnail、heading TOC、hover extraction、render 建立全局并发限制。
4. 修复 OOM 失败处理和 context clone fallback。
5. 区分瞬态渲染失败与永久文档错误。

### 第二阶段：PDF 兼容性

1. 建立 PDF 2.0/2.1 测试矩阵。
2. 统一执行 PDF 权限位。
3. 增加 XFA、public-key encryption、签名吊销/TSA、PDF/A/PDF/X fixture。
4. 增加 AcroForm 完整填写、保存、重开测试。
5. 明确区分“识别 PDF/A/PDF/X”与“通过合规验证”。
6. 为 malformed PDF 和自动修复增加回归语料。

### 第三阶段：多文档压力测试

建议增加一个定向测试，逐步打开 20/50/100 个 PDF，记录：

- private bytes / working set；
- GDI handle；
- D2D bitmap 数量；
- 线程数量；
- MuPDF store/cache 大小；
- 关闭标签后资源是否回落；
- reload 和关闭窗口时是否出现等待或 UAF。

## 6. 本轮已实施的修复（2026-09-25）

本轮先落地了风险较低、可独立验证的修复：

1. **Home 滚动条**
   - `src/SumatraPDF.cpp` 新增 `UpdateHomePageScrollbars()`。
   - `src/HomePage.cpp` 统一同步 `windows`、`smart/overlay` 和 `hidden` 模式。
   - 修复默认 `windows` 模式下内容溢出却没有可见滚动条的问题，并抑制滚动条触发的画布尺寸重入。

2. **RenderCache 字节预算**
   - `src/RenderCache.h` 增加缓存字节计数和物理内存上限。
   - `src/RenderCache.cpp` 按物理内存设置 128–512 MiB 上限，淘汰时同时遵守条目数和字节数限制，超大单张 bitmap 不进入缓存。
   - 当前预算统计 CPU `Pixmap` 的近似大小，D2D 上传和渲染中的临时 pixmap 尚未纳入。

3. **悬停文本提取并发上限**
   - `src/EngineBase.cpp` 将机会性的 `RequestTextExtraction()` 后台任务限制为最多 2 个。
   - 超出上限时只放弃预取，不影响搜索、选择、复制等按需同步路径。
   - 这是针对已确认调用点的局部限流，不等同于覆盖所有后台任务的进程级线程池。

4. **Heading TOC 按需启动**
   - `src/DisplayModel.cpp` 不再在每个文档控制器构造时启动 heading 扫描，而是在当前文档查询 TOC 时启动。
   - `src/DisplayModel.h` 记录每个控制器是否已请求扫描，避免重复启动。
   - `src/SumatraPDF.cpp` 在判断侧栏状态前先查询 TOC，保留异步扫描期间的 `showToc` 偏好。
   - 批量打开时后台标签不会各自立即创建 heading 线程；切换到标签并查询 TOC 后仍会正常生成。

已验证：

- `bun cmd/build.ts -debug`：通过，0 警告、0 错误。
- `bun tests/issue-5870-list-dirs.ts --no-build`：通过。
- `bun tests/issue-5724.ts`：通过。
- `bun tests/issue-annot-locking.ts --no-build`：通过。
- `bun tests/issue-render-stability.ts --no-build`：通过。

尚未实施的项目仍包括：MuPDF store 跨文档回收、context clone 失败时的安全策略、OOM 统一处理、渲染失败重试、in-flight bitmap 预算、覆盖所有后台任务的进程级线程池，以及 PDF 2.0/2.1、权限、XFA、签名和 PDF/UA 测试矩阵。
