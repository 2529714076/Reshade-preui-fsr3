---
name: Bug report
about: 出问题了？请按下面的模板把信息填全，否则很难定位
title: '[Bug] '
labels: bug
---

## 现象

<!-- 一句话说明：UI 也被滤镜影响了 / 没有 injected / 没找到 FSR 模块 / 游戏启动不了 / 崩溃 ... -->

## 环境

| 项 | 内容 |
|---|---|
| PreUI FSR 版本 | <!-- 例如 0.4.0，见 PreUI-FSR.log 第一行 --> |
| ReShade 版本 | <!-- 例如 6.8.0 --> |
| 游戏 / 引擎 | <!-- 例如 XXX / UE4.27 --> |
| 显卡 / 驱动版本 | <!-- A 卡还是 N 卡？开 FSR 是否正常 --> |
| FSR 版本 / 设置 | <!-- FSR 3.1？质量档位？帧生成是否开启 --> |
| 分辨率 / 是否 HDR | |
| 系统 | <!-- Windows 版本 --> |

## 日志

`PreUI-FSR.log` 全文粘贴（`PreUI_FSR.ini` 里 `Trace=1`、`DumpDesc=1`）：

```
把 PreUI-FSR.log 内容贴在这里
```

重点是这几行，没有的话说明更早就断了：

```
[PreUI-FSR] module: ...                       <- 游戏加载了哪些 fsr/ffx 相关 DLL
[PreUI-FSR] target ...ffxDispatch -> 0x...
[PreUI-FSR] commandList offset resolved: 0/16
[PreUI-FSR] cand[0] off=40 ... fmt=10 bpp=64  <- 每个候选资源的真实描述
[PreUI-FSR] output resolved: off=... fmt=...
[PreUI-FSR] injected N frames (... fmt=10)    <- fmt 必须是 10 或 28
```

## 已经试过

- [ ] `PreUI_FSR.ini` 里 `Enabled=0`（能否启动）
- [ ] 游戏内确认升采样确实是 FSR 3（不是 DLSS / XeSS / TSR）
- [ ] 关闭 FSR 帧生成
- [ ] 移除其它 ReShade add-on

## 补充

<!-- 截图 / 录屏 / 游戏目录里的 amd_fidelityfx_*.dll 文件名 -->
