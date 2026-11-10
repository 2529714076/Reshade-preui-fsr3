# ReShade Pre-UI add-on (FSR 3)

[![Build](https://github.com/2529714076/Reshade-preui-fsr3/actions/workflows/build.yml/badge.svg)](https://github.com/2529714076/Reshade-preui-fsr3/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/2529714076/Reshade-preui-fsr3)](https://github.com/2529714076/Reshade-preui-fsr3/releases/latest)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

把 ReShade 的 `.fx` 效果绘制到 **UI 合成之前的 3D 画面** 上——UI 完全不受滤镜影响，
画面仍然由游戏自己 Present。这是 **FSR 3** 版本，注入点在 AMD FidelityFX 的升频器上，
**A 卡 / N 卡 / 核显都能用**（FSR 跨厂商，不挑显卡）。

在《王者荣耀世界》（4K + FSR 3 + HDR）实测通过：滤镜只作用于 3D 画面、UI 不受影响，
连续 2400+ 帧稳定注入，无崩溃。

> **Pre-UI effects for games that use AMD FSR 3.** Hooks `ffxDispatch` in the loaded
> FidelityFX library (`amd_fidelityfx_dx12.dll` on FSR 3.1.3+/FSR 4,
> `amd_fidelityfx_upscaler_dx12.dll` on older 3.1.x), picks the upscaler's output resource out of
> the `FfxDispatchDescription` using its real D3D12 descriptor, runs ReShade's effect chain on it
> and copies the result back — before the UI is composed.
> No AMD headers, no hard-coded struct offsets.

DLSS 版本在另一个仓库：`reshade-preui-addon`。

---

## ⚠️ 先读这一段

- 本程序会**注入 DLL 并 inline hook 游戏加载的 FSR 升频器**。在任何带反作弊的在线游戏里使用
  都存在**封号风险**，请自行评估。建议先在单机 / 离线场景验证。
- 只支持 **DX12 + FSR 3 的 DLL 版**。FSR 2.x、以及把 FSR 静态编进 exe 的游戏没有可 hook 的导出，
  本方案不适用（日志的 `module:` 列表能一眼看出属于哪种）。
- 这是**实验性**实现，无担保。不同游戏 / 不同 FSR 版本的结构体布局可能不同，
  出问题时把 `PreUI-FSR.log` 带上。

---

## 为什么 A 卡能用 FSR，而 DLSS 版不行

| | DLSS 版 | 本仓库（FSR 3 版） |
|---|---|---|
| 升频器 | `_nvngx.dll` / `nvngx_dlss.dll` | `amd_fidelityfx_dx12.dll`（3.1.3+/FSR 4）<br>`amd_fidelityfx_upscaler_dx12.dll`（旧 3.1.x） |
| 导出 | `NVSDK_NGX_D3D12_EvaluateFeature` | `ffxDispatch` |
| 输出资源 | NGX 参数 `"Output"` | `FfxDispatchDescription::output` |
| 显卡要求 | 仅 NVIDIA RTX | **任意支持 DX12 的显卡**（FSR 跨厂商） |

所以：**N 卡也能开 FSR 3**，可以在 N 卡上开发和调试，最后在 A 卡上确认一次即可。

---

## 工作原理

```
3D 场景 ──► 后处理 ──► [FSR 3 升频] ──► UI 合成 ──► Present
                            │
                            └──► 我们的注入点（UI 之前）
```

1. 第 120 帧之后扫描已加载模块，找导出 `ffxDispatch` 的 FidelityFX 库并 inline hook
   （优先 hook DLL；游戏 exe 里也有一份时只在 DLL 都挂不上时才用）。
2. 拿到 `FfxDispatchDescription`：
   - `commandList` 在 `+16`（新 FFX API 有 16 字节头），用「能否在 ReShade 命令列表表里查到」
     来自动探测，兼容老布局的 `+0`；
   - `output` 用运行时自标定找出（见下）。
3. 拷贝到同尺寸临时纹理 → `render_effects()` → 拷回输出资源。
4. ReShade 因为本帧已经手动渲染过，会跳过 Present 阶段对整帧（含 UI）的再次渲染。

### 为什么不能写死结构体偏移

`FfxDispatchDescription` 是 AMD 的私有结构体，字段顺序 / 数量随 SDK 版本变化，而这个 add-on
又不能保证游戏用的是哪个版本。硬编码 = 一次 SDK 更新就崩。

这里的做法分两步：

1. **结构扫描**：在 desc 内存里找 `[width, height, 1, 1]` 连续四个 `uint32`
   （即 `FfxResourceDescription` 的 `width / height / depth / mipCount`，后两个恒为 1），
   命中后往前推 16 字节就是 `resource` 指针；
2. **用真实资源描述打分**：对每个候选调用 `device->get_resource_desc()`，按
   `texture2d / 非 MSAA / ≥32bpp / 非 depth_stencil / 尺寸匹配 / 位深 / 是否 RT·UAV /
   在 desc 里的位置` 选出一个，规则写在源码注释里。

### 实测布局（FFX API 1.1 · `amd_fidelityfx_dx12.dll`）

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

每个 `FfxResource` 48 字节，`width` 落在 **40 / 88 / 136 / 232 / 280 / 328**。
FSR 3.1.4 / FSR 4 之前只有 5 个资源（没有 reactive / shadingChange），`output` 在 `+216`，
所以扫描范围必须覆盖 512 字节——只扫 256 会把 R8 的 mask 当成 output。

---

## 安装

1. 先装好 **ReShade 6.8.0**（游戏目录里应有 `dxgi.dll` 或 `d3d12.dll`）。
2. 到 [Releases](../../releases/latest) 下载 `ReShade_PreUI_FSR-vX.Y.Z.zip`。
3. 把 **`ReShade_PreUI_FSR.addon64`** 和 **`PreUI_FSR.ini`** 解压到**游戏 exe 同目录**。
4. 进游戏 → 视频设置里把升采样设成 **FSR 3**（不要 DLSS / XeSS / TSR）→ 进实际场景。
5. 打开 ReShade 面板勾一个滤镜。

> 文件名带 `_FSR` 后缀，可以和 DLSS 版共存于同一目录而不互相覆盖
> （但同一个游戏里只会有一个生效）。

## 配置（`PreUI_FSR.ini`）

| 键 | 默认 | 说明 |
|---|---|---|
| `Enabled` | `1` | `0` = 只加载 DLL、不装 hook。排查启动问题时先用这个 |
| `DelayFrames` | `120` | 第 N 帧之后再开始扫描并挂钩 |
| `Trace` | `1` | 失败原因最多记 60 条 |
| `DumpDesc` | `1` | 首次标定时 dump desc 前 512 字节（只前 3 次） |
| `OutputOffset` | `0` | 人工指定 `width` 字段的字节偏移，填了就不自动标定 |

## 看日志

日志在游戏目录 `PreUI-FSR.log`（写不进去时退回 `%TEMP%`）。正常流程：

```
[PreUI-FSR] target amd_fidelityfx_dx12.dll!ffxDispatch -> 0x...
[PreUI-FSR] MH_EnableHook -> 0
[PreUI-FSR] slot[0] entered (call=1 depth=1)
[PreUI-FSR] desc type=0x10001 swapchain=3840x2160 candidates=6
[PreUI-FSR] cand[0] off=40  desc=3840x2160 -> 3840x2160 fmt=10 bpp=64 samples=1   ← color
[PreUI-FSR] cand[1] off=88  desc=3840x2160 -> 3840x2160 fmt=19 bpp=64 samples=1   ← depth
[PreUI-FSR] cand[2] off=136 desc=3840x2160 -> 3840x2160 fmt=34 bpp=32 samples=1   ← motion vectors
[PreUI-FSR] cand[3] off=232 desc=3840x2160 -> 3840x2160 fmt=61 bpp=8  samples=1   ← reactive mask
[PreUI-FSR] cand[4] off=280 desc=3840x2160 -> 3840x2160 fmt=61 bpp=8  samples=1   ← shadingChange
[PreUI-FSR] cand[5] off=328 desc=3840x2160 -> 3840x2160 fmt=10 bpp=64 samples=1   ← output ✔
[PreUI-FSR] output resolved: off=328 ptr=... 3840x2160 fmt=10
[PreUI-FSR] commandList offset resolved: 16
[PreUI-FSR] temp target created 3840x2160 fmt=10
[PreUI-FSR] injected 1 frames (3840x2160 fmt=10)
```

| 日志 | 含义 |
|---|---|
| `exports ffxDispatch` | 这个模块就是注入点 |
| `no ffxDispatch export` | 有 FidelityFX 库但导出不对（老 ABI） |
| `commandList offset resolved: 0/16` | desc 布局已识别（16 = 新 FFX API，0 = 老 API） |
| `skip non-upscale dispatch (desc type=0x…)` | 正确跳过了帧生成 / 降噪那次调用 |
| `cand[k] … fmt=… bpp=…` | 每个候选的真实 D3D12 描述 |
| `injected N frames (… fmt=10)` | ✅ 成功；`fmt` 必须是 10（HDR）或 28（8bit 颜色） |
| 有 `target` 但**没有** `slot[0] entered` | 钩子装好了，但游戏没用 FSR（升采样还开着 DLSS / TSR） |

**判定画面**：勾上滤镜后只有 3D 场景变色、UI 不变色。

**如果一直显示 `fmt=61`（R8）之类的单通道格式**，说明挑错了资源——
把 `cand[...]` 那几行发出来，或者自己数：`b+NNN:` 行里 `width`/`height` 是相邻两个
`uint32`（小端），比如 3840×2160 表现为 `00 0F 00 00 70 08 00 00`；
第一个 `00` 在该行内的位置加上 `NNN` 就是 `OutputOffset`（`resource` 在它前面 16 字节）。

---

## 已知限制

- **只支持 FSR 3.x 的 DLL 版。** FSR 2.x / 部分 FSR 3.0 集成是把代码静态编进游戏模块的
  （UE 的 `FSR2TemporalUpscaling` / `FSR3` 插件早期版本就是这样），没有可 hook 的导出函数。
- **帧生成与降噪不处理**：专用 DLL 直接跳过；像 `amd_fidelityfx_dx12.dll` 这种「全包」形态，
  则靠 dispatch desc 的 `type` 高 16 位跳过（`0x0002` 帧生成 / `0x0003` 降噪）。
- **某帧没调 FSR 时**：那一帧 ReShade 会按默认路径在 Present 前再叠一次，UI 会被影响一下。
  公开 API 的边界，无法屏蔽。
- **性能**：每帧两次全屏拷贝，4K HDR 约 66 MB/次。结构上必需
  （`render_effects` 的 `rtv` 参数就是「效果链眼中的 backbuffer」，输入必须先落到自己的纹理上）。
- **实验性**：无担保，风险自担。

---

## 从源码构建

依赖（ReShade 头文件 v6.8.0 + MinHook）不在仓库里，构建时下载。

### 方式 A：GitHub Actions

推到 `main` / 建 PR → `Build` workflow 跑一次，产物在 **Artifacts → `ReShade_PreUI_FSR`**。
打 tag → `Release` workflow 自动打包发布。

### 方式 B：本地

需要 **Visual Studio 2022**（含「使用 C++ 的桌面开发」）+ **CMake ≥ 3.20**。

```bat
build.bat
```

脚本会自动执行 `scripts/fetch-deps.ps1` 把依赖下到 `third_party/`（只下一次，之后跳过）。
不想联网的话，直接从 DLSS 版仓库把整个 `third_party/` 目录拷过来即可。

产物：`build\Release\ReShade_PreUI_FSR.addon64`

### 构建失败排查

| 报错 | 原因 | 处理 |
|---|---|---|
| `third_party/minhook is missing` | 依赖没下载 | `pwsh -File scripts/fetch-deps.ps1`，或拷贝 `third_party/` |
| `C1083: reshade_overlay.hpp` | ReShade 头文件不全 | 同上，确认 8 个头文件都在 |
| `LNK2019` / `LNK1120` | MinHook 的 `.c` 没编进去 | 检查 `CMakeLists.txt` 的 4 个源文件 |
| 编译通过但游戏里没日志 | ReShade 没加载 | 确认放在 exe 同目录且扩展名是 `.addon64` |

---

## 发布新版本

版本号唯一来源是 `CMakeLists.txt` 的 `project(reshade_preui_fsr3 VERSION x.y.z)`，
编译期以 `PREUI_VERSION` 宏注入。tag 写 `v0.4` 或 `v0.4.0` 都可以（CI 会归一化后比较）。

```bat
:: 1. 改 CMakeLists.txt 的 VERSION，并在 CHANGELOG.md 记一笔
git add -A
git commit -m "release: v0.4.0"
git push

:: 2. 打 tag 并推送
git tag v0.4
git push origin v0.4
```

`Release` workflow 会：校验 tag ↔ `CMakeLists.txt` 的 VERSION → 编译 →
打包 `ReShade_PreUI_FSR-v0.4.0.zip`（含 `.addon64`、`PreUI_FSR.ini`、README、CHANGELOG、
LICENSE、THIRD_PARTY_NOTICES、VERSION.txt）→ 创建 GitHub Release。

tag 已推送但只想重跑时：

```bat
git tag -d v0.4
git push origin :refs/tags/v0.4
git tag v0.4
git push origin v0.4
```

---

## 目录结构

```
.
├─ CMakeLists.txt                 # 版本号唯一来源
├─ build.bat                      # 本地一键编译（含依赖下载）
├─ PreUI_FSR.ini                  # 随包发布的默认配置
├─ scripts/fetch-deps.ps1         # 下载 ReShade 头文件 + MinHook
├─ src/main.cpp                   # 全部逻辑
├─ .github/workflows/
│  ├─ build.yml
│  └─ release.yml
├─ CHANGELOG.md
├─ THIRD_PARTY_NOTICES.md
└─ LICENSE
```

## 致谢
- [Deepseek Harness](https://www.deepseek.com/harness)(深度求索)——本代码完全由Deepseek Harness 设计 （作者只提供设计方向）
- [ReShade](https://reshade.me)（crosire）—— add-on API 与 `render_effects`
- [MinHook](https://github.com/TsudaKageyu/minhook)（Tsuda Kageyu）
- [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)（MIT）—— `ffxDispatch` 接口

第三方组件许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)，原创代码为 [MIT](LICENSE)。

## 免责声明

本项目仅供学习与技术研究。使用者需自行承担使用本软件的全部风险，包括但不限于账号封禁、 数据损坏、系统不稳定。作者不对任何直接或间接损失负责。请勿用于规避游戏反作弊机制或 任何违反服务条款的用途。
