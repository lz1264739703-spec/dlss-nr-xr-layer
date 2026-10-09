# DLSS-NR OpenXR Layer（AMD / NVIDIA 两版）

一个 **OpenXR 隐式 API 层**：拦截应用提交给合成器的投影层，把画面中央一块**神经窗口**（大小、位置可调）交给 **DLSS 神经降噪（DLSS-NR）** 处理，再把结果贴回眼缓冲区；剩下的画面保持原样。两版做同一件事，走的是两条完全不同的后端：

## 亮点（关键词）

- **让 DLSS 5 / DLSS-NR 在 VR 里真正显示出来**——DLSS5 神经降噪原本没有 VR 通路、无法在头显里显示；这一层把它的输出接进 OpenXR 的提交路径，双眼都能看到降噪后的画面。
- **任意 VR 游戏通用，免逐游戏适配**——以 **OpenXR 隐式 API 层**（implicit API layer）工作，在应用提交投影层时处理：不需要游戏原生支持 DLSS、不改游戏文件、不注入引擎，任何走 OpenXR 的 VR 游戏都能用（Half-Life: Alyx 实测）。
- **两套后端，一个层**——AMD（lmxxf / DLSS5-NR 运行时，HIP）与 NVIDIA（驱动 NGX + DLSS-NR）同一份层源码、同一套控制面板。
- **只算"神经窗口"**——只裁画面中央一块交给网络，其余画面原样保留：开销可控，窗口大小/位置可调并跟随眼动。
- **实时调参 + 抗冻结的尺寸策略**——浏览器面板随改随生效；64 px 量化 + 75% 保持 + 400 ms 停稳，把"拖滑块每步冻一次"变成"停手后只重建一次"。

> 关键词：`DLSS5` `DLSS 5` `DLSS-NR` `神经降噪` `Neural Rendering` `denoiser` `VR` `VR 游戏` `任意 VR 游戏` `OpenXR` `OpenXR 隐式层` `implicit API layer` `免游戏适配` `AMD` `RX 9070` `NVIDIA` `NGX` `HIP` `Half-Life: Alyx`

| | `amd/`（AMDNR） | `nvidia/`（NGX） |
| --- | --- | --- |
| 后端 | lmxxf / DLSS5-NR 运行时（HIP；本地构建的 `LmxxfNrRuntime.dll` + 目录树资产：HIP 模块、权重、着色器） | 驱动里的 NGX 核心 + 预发布 DLSS-NR 运行时（`nvngx_dlssnr.dll`） |
| 适用 GPU | AMD（gfx12 目标，含 RX 9070 系列实测） | NVIDIA（需要可用的 NGX/DLSS-NR） |
| 额外依赖 | 无（运行时随整合包提供） | `caller/nvngx.dll`（本仓库源码构建，见 `nvidia/build.ps1` 注释） |
| 输出目录 | `C:\Users\Administrator\vdxr\xr-layer`（可在 `build.ps1` 改） | `C:\Users\Administrator\vdxr\xr-layer-ngx` |
| 实测 | Half-Life: Alyx + Pimax，90 Hz 会话中 1280×704 窗口约 31–41 fps（见下） | 编译通过与冒烟测试；实机由用户在有 N 卡的机器上验证 |

> 当前为 **2026-10-07 冻结版**：窗口尺寸策略（64 px 量化 + 75% 保持 + 400 ms 停稳）、纹理重建的 drain/退役释放、面板与 addon 桥、以及"同步布局"默认。变更见 [CHANGELOG.md](CHANGELOG.md)。

## 它做什么

**一句话：把 DLSS 5 / DLSS-NR 神经降噪接进 VR 的 OpenXR 提交路径——DLSS5 原本没有这条通路、因此无法在头显里显示；层对任意 VR 游戏生效，无需逐游戏适配。**

```
眼缓冲区 ──裁剪(窗口)──> D3D11 共享纹理 ──> D3D12 转换(fp16) ──> DLSS-NR ──> 转换(8bit)+羽化混合 ──> 贴回眼缓冲区
                                     ↑ 运动场(旋转/平移，可选深度)        ↑ 抗闪烁历史(逐眼)
```

- **任意 VR 游戏通用（免游戏适配）**：层以 **OpenXR 隐式 API 层**（implicit API layer）挂在提交路径上——游戏不需要原生支持 DLSS、不改游戏文件、不注入引擎，任何走 OpenXR 的 VR 游戏都能用（Half-Life: Alyx 实测）。
- **神经窗口（neural window）**：默认等于网络上限（`AMDNR_XR_CROP` 或面板 ceilw/ceilh），跟随眼动（gaze）在眼缓冲区内移动，位置按 8 px 栅格吸附。
- **尺寸策略（本冻结版的核心）**：窗口**尺寸**按 64 px 栅格量化；比当前尺寸小的请求要在**低于 75%** 时才被接受（AMDNR 的"带内保持"）；且任何尺寸变化都要等设置**稳定 400 ms** 后才生效。原因：改变尺寸会让后端**重建网络**（一次约 1.6–2.5 s 的冻结），量化 + 保持 + 停稳把"拖动滑块时每步冻一次"变成"停手后只重建一次"。
- **重建的生命周期**：共享纹理换新前先 **drain**（等队列空闲），旧对先进**退役槽**、按 fence 收集后释放。
- **输出通路**：另有全分辨率输出（FSR4 上采样，`AMDNR_XR_UPSCALE`）与几路诊断探针（深度、投影、立体、抗闪烁回读），默认关闭或按需启用。

## 安装（整合包）

整合包（Release 附件里的两个 zip）是**开箱即用**的：层 DLL + 第三方运行时 + OpenComposite runtime + 安装脚本。用法：

1. 解压到任意目录。
2. 以**管理员**运行 `install.ps1`（隐式层注册在 HKLM；脚本也会把 OpenVR 路由指到包内的 OpenComposite）。约半数配置需要把 `opencomposite.ini` 一起放到 **游戏 exe 同目录**（或层目录），让游戏走 OpenComposite 而不是 SteamVR。
3. 启动游戏。层日志在层目录的 `AmdnrXrLayer.log`，控制面板默认 `http://127.0.0.1:8787`。

要从 **OpenVR 游戏**（Alyx、Arizona Sunshine 等）进入，需要 OpenComposite 桥——构建构成、两种安装方式与实测边界见 [docs/OPENVR-GAMES.md](docs/OPENVR-GAMES.md)。

卸载：`uninstall.ps1`。

## 控制面板

层内起一个只监听 127.0.0.1 的小 HTTP 服务（端口 `AMDNR_XR_PANEL_PORT`，默认 8787）：

- `GET /` 面板页面；`GET /state` 当前生效值（含 clamp 后的实际值）；`GET /set?<key>=<value>` 写入并立即生效（下一次会话生效的项会在页面标注"restart"）。

常用键：`enabled`（总开关，关掉后层完全不裁剪/不过网）、`transfer`/`color`（强度）、`scale`/`passes`（模型强度/遍数）、`antiflicker`/`afgate`、`ceilw`/`ceilh`（网络工作分辨率上限）、`winw`/`winh`（窗口覆盖比例）、`feather`/`roundness`（窗口边缘羽化/形状）、`motionnear`、`tone`/`structure`/`skin`/`mask`/`style`（模型控制项）、`autoq`（自动画质档）。

**关于窗口滑块的行为**（冻结版设计如此，不是失灵）：
- 窗口尺寸只在 64 px 栅格上取值 → 滑块小幅移动不改变实际尺寸；
- 比当前尺寸小的请求要掉到 **75% 以下**才被跟随 → 缩小方向最多滞后约 1/4；
- 任何尺寸变化等 **400 ms** 稳定 → 拖动过程中画面不跟手，停手后切一次；
- 实际生效值看 `/state` 与日志里的 `layer: window ... (ask ..., band ...)` 三行（`waiting` / `held` / `a new size rebuilds the network`）。

## 环境变量

见 [docs/ENVIRONMENT.md](docs/ENVIRONMENT.md)（两版全表，含 `AMDNR_XR_CROP`、`AMDNR_XR_COVER`、`AMDNR_XR_ASYNC`、诊断类开关等）。

## 构建

见 [docs/BUILD.md](docs/BUILD.md)。两版都是 `cl.exe` 直接编译（无需解决方案），`build.ps1` 会把 DLL 与清单/FSR 头文件一起布置到输出目录。

## 已知问题与排查

见 [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)。几条最常遇到的：

- **帧率突然掉到个位数**：先看是不是头显里的 **Pimax Home 场景** / overlay 在抢 GPU（实测占到 67% GPU，游戏被挤到 9 fps；结束 `PimaxHome-Win64-Shipping` 与 `pi_overlay` 即恢复）。
- **改分辨率时有约 1.6–2.5 s 冻结**：这是**后端重建网络**的固有成本，一次真实尺寸变化一次；本冻结版的量化/保持/停稳把次数压到最少。
- **异步布局**（`AMDNR_XR_ASYNC=1`，本帧提交/下帧收，能省掉 CPU 等 GPU 的 ~112 ms）**默认关闭**：它在本机把设备挂死过（`0x887A0006` / `PrepareFrame: GPU drain failed`），修好之前不要开。

## 许可

- 本仓库源码与脚本：MIT（见 [LICENSE](LICENSE)）。
- 第三方运行时/权重/驱动组件：见 [NOTICE.md](NOTICE.md)，它们不在本仓库内，仅作为 Release 附件分发，且不受 MIT 覆盖。