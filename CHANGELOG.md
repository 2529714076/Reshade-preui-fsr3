# 更新日志

版本号唯一来源是 `CMakeLists.txt` 的 `project(reshade_preui_fsr3 VERSION x.y.z)`，
编译期以 `PREUI_VERSION` 宏注入。tag 写 `v0.4` 或 `v0.4.0` 都可以，CI 会归一化后比较。

---

## v0.4.0 — 2026-11-10

**首个可用版本。** 在《王者荣耀世界》（4K + FSR 3 / HDR）实测通过：
滤镜只作用于 3D 画面、UI 不受影响，连续 2400+ 帧稳定注入，无崩溃。

### 实机确认的结构布局

`amd_fidelityfx_dx12.dll`（FFX API 1.1）的 `FfxDispatchDescription`：

```
+0      uint64 type = 0x10001     // 0x0001xxxx = upscale 类
+8      void*  pNext
+16     void*  commandList
+24     FfxResource color                       3840x2160 R16G16B16A16_FLOAT
+72     FfxResource depth                       3840x2160 R32G32_FLOAT
+120    FfxResource motionVectors               3840x2160 R16G16B16A16_FLOAT
+168    FfxResource transparencyAndComposition  1x1 dummy（本作未使用）
+216    FfxResource reactive                    3840x2160 R8_UNORM
+264    FfxResource shadingChange               3840x2160 R8_UNORM
+312    FfxResource output                      3840x2160 R16G16B16A16_FLOAT  ← 目标
```

每个 `FfxResource` 占 48 字节：`ptr(8) + type(4) + format(4) + w(4) + h(4) + depth(4) + mip(4) + flags(8+)`，
所以 `width` 分别落在 **40 / 88 / 136 / 232 / 280 / 328** 字节。
FSR 3.1.4 / FSR 4 之前只有 5 个资源（没有 reactive / shadingChange），`output` 在 `+216`。

### 优化

- **每帧只做一次 `memcpy` + 指针比较**：标定成功后记住 `(offset, resource)`，
  只要指针没变就直接返回，不再每帧 `VirtualQuery`（那是一次内核调用）。
- **分辨率变化走快路径**：同一偏移换了纹理时，只做一次校验就接受，不重新扫描整个 desc。
- **去掉每帧重复的 `get_resource_desc`**：标定阶段已经查过，结果直接复用。
- **交换链尺寸只在标定阶段查询**，不再每帧调 `get_screenshot_width_and_height`。
- **帧生成 / 降噪的跳过规则改为按类别判断**：`type >> 16 == 0x0002 / 0x0003`，
  覆盖 `0x20000`、`0x20001` 等所有变体；老布局下首字段是堆指针（高 16 位远大于 3），判断依然安全。

### 变更

- 版本 0.4.0；README 补上实机验证日志与结构布局说明。

---

## v0.3.0 — 2026-11-10

v0.2 的实测日志：钩子挂上了、逐帧注入 10800 帧、不崩，但**画面没有任何效果**。
线索是 `injected ... fmt=61`（`R8_UNORM`）——注入的是一张 8bit 单通道纹理，不是场景色。

### 根因

FSR 3.1.4 / FSR 4 在 `output` 之前又插了 `reactive` 和 `shadingChange` 两张 mask，
`FfxDispatchDescription` 里的 `FfxResource` 从 5 个变成 7 个（≈ 360 字节）。
v0.2 只扫 desc 的前 **256** 字节，看到第 5 个资源就收工——那正好是 R8 的 mask，
于是整条注入链路都在给 mask 做拷贝，画面自然毫无变化。

### 修复

- **扫描范围 256 → 512 字节**（hex dump 同步扩到 512，能直接看到第 6/7 个资源）。
- **不再靠 desc 里的 uint32 猜 output，改用真实的 D3D12 资源描述来选**：
  收集 desc 里所有「长得像 FfxResource」的候选（最多 12 个），逐个
  `device->get_resource_desc()` 拿真实 type / 尺寸 / 格式 / 采样数 / usage，再打分：
  - 必须 `texture_2d`、非 MSAA、**每像素 ≥ 32 bit**（排除 R8/R16 的单通道 mask）；
  - 排除 `depth_stencil`（深度缓冲也是 32bpp，不排掉会被误选）；
  - `render_target` / `unordered_access` 各 +100（FSR 用 UAV 写输出）；
  - 尺寸等于交换链分辨率 +1000；desc 声明尺寸与真实资源不符 −500；
  - 位深越大越优先；同分取 desc 里最靠后的（`output` 是最后一个 `FfxResource`）。
- 日志会打印**每个候选的真实描述**（`cand[k] off=… -> WxH fmt=… bpp=…`），
  哪张才是场景色一目了然。
- **优先 hook DLL，不 hook 游戏 exe**：实测 `NGR-Win64-Shipping.exe` 也导出了
  `ffxDispatch`（静态链接 FFX 时常见），但游戏实际调用的是
  `amd_fidelityfx_dx12.dll` 那份。现在扫完所有 DLL 都没挂上时才退回 exe。
- 首次标定额外打印 `desc type=0x…`，便于确认是不是升频那次 dispatch。

---

## v0.2.0 — 2026-11-10

针对 v0.1 在《王者荣耀世界》上的实测日志修复。**第一次测试发现的关键事实**：
游戏用的是 **`amd_fidelityfx_dx12.dll`**，即 FSR 3.1.3+/FSR 4 的统一 FFX API 库
（升频 / 帧生成 / 降噪合并在一个 DLL 里），而不是老版的 `amd_fidelityfx_upscaler_dx12.dll`。
v0.1 按文件名里的 `upscaler` 匹配，所以一个模块都没命中，钩子根本没装。

### 修复

- **改为按导出找模块**：遍历所有模块，`GetProcAddress(mod, "ffxDispatch")` 成功就挂钩，
  不再依赖文件名。名字里带 `framegeneration` / `denoiser` 的专用 DLL 仍然跳过。
- **自动探测 desc 里 `commandList` 的位置**：新 FFX API 的 dispatch desc 以
  `{ uint64 type; void* pNext; }` 开头，`commandList` 在第 16 字节；老 API 在第 0 字节。
  现在依次尝试 0/8/16/24，用「能否在 ReShade 的命令列表表里查到」判定，结果缓存。
- **跳过非升频 dispatch**：同一个 DLL 也承载帧生成，`ffxDispatch` 每帧会被调用多次。
  现在读 desc 的 `type`（`0x00020000` 帧生成 / `0x00030000` 降噪 / `0x00020001` 帧生成交换链）
  直接跳过。老布局下第 0 字节是 commandList 指针，不可能等于这些小常量，所以两种布局都安全。
- 模块普查日志新增 `exports ffxDispatch` / `no ffxDispatch export`，
  一眼看出哪个模块才是注入点。

---

## v0.1.0 — 2026-11-10

**首个测试版（未生效）。** 目标是在 N 卡上确认 FSR3 路径能跑通，再找 A 卡验证。

### 已实现

- 扫描已加载模块，找名字里带 `upscaler` 的 FidelityFX 模块，
  hook 其导出 `ffxDispatch`。
- **不引入 AMD 头文件、不硬编码结构体偏移**：在调用方传入的 `FfxDispatchDescription`
  里扫描 `[width, height, 1, 1]` 四元组（即 `FfxResourceDescription` 的
  `width/height/depth/mipCount`），命中后往前推 16 字节即为 `resource` 指针。
  标定结果缓存，失效时自动重扫。
- 命令列表代理偏移沿用 DLSS 版的自标定（`native + {0,8,…,256}`）。
- 注入流程与 DLSS 版一致：拷贝到临时纹理 → `render_effects` → 拷回输出资源。
- 诊断日志：模块普查、标定打分、`FfxDispatchDescription` 前 256 字节 hex dump。

### 已知限制

- 只支持 **FSR 3.x 的 DLL 版**。若游戏把 FSR 静态编译进 exe，没有可 hook 的导出。
- 不支持 FSR 帧生成 / 降噪。
- 结构体布局靠运行时自标定，需要在真实游戏里验证——v0.2 就是这一步的结果。
