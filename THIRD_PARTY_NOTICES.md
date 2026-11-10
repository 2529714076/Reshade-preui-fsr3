# 第三方组件与许可

本仓库的原创代码使用 [MIT License](LICENSE)。

与 DLSS 版不同，**本仓库不包含任何第三方源码**：`third_party/` 由
`scripts/fetch-deps.ps1` 在构建时下载，因此不存在再分发问题。

---

## 1. ReShade add-on API 头文件

| 项 | 内容 |
|---|---|
| 位置 | `third_party/reshade/include/`（构建时下载，8 个文件） |
| 来源 | https://github.com/crosire/reshade （tag **v6.8.0**） |
| 版权 | Copyright (C) 2021 Patrick Mours；`reshade_overlay.hpp` 另含 Copyright (C) 2014-2026 Omar Cornut（Dear ImGui） |
| 许可 | **BSD-3-Clause OR MIT** |
| 用途 | 仅作为 add-on 编译期头文件；不链接、不修改、不再分发 ReShade 本体 |

ReShade 本体（`dxgi.dll` / `d3d12.dll`）由使用者自行安装。

## 2. MinHook

| 项 | 内容 |
|---|---|
| 位置 | `third_party/minhook/`（构建时下载，master 分支） |
| 来源 | https://github.com/TsudaKageyu/minhook |
| 版权 | Copyright (C) 2009-2017 Tsuda Kageyu；内嵌 Hacker Disassembler Engine（Vyacheslav Patkov） |
| 许可 | **BSD-2-Clause** |
| 用途 | 对 `amd_fidelityfx_upscaler_dx12.dll!ffxDispatch` 做 inline hook |

---

## 3. AMD FidelityFX / FSR

本 add-on **不包含、不链接、不重分发**任何 AMD 代码或头文件：

- 不引入 `ffx_*.h`（避免随 SDK 版本变化的 ABI 依赖）；
- 不打包 `amd_fidelityfx_upscaler_dx12.dll`（由游戏自带）；
- 只在运行期对已加载 DLL 的导出函数 `ffxDispatch` 做 inline hook，并从调用方传入的
  `FfxDispatchDescription` 里读取参数。

FidelityFX SDK 本身是 MIT 许可（https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK）。
如果你的发行政策要求显式声明，可以在 README 里附上这句说明。

## 4. NVIDIA

本仓库与 NVIDIA 无关，不含任何 NGX / DLSS 头文件或二进制。
DLSS 的版本在另一个仓库（`reshade-preui-addon`）里。
