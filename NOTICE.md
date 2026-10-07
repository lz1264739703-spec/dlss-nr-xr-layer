# 第三方组件与许可

本仓库的 MIT 许可（见 LICENSE）只覆盖本仓库中的源码与脚本。层在运行时驱动的神经网络运行时、模型权重与驱动组件都是第三方作品，各自适用其自身条款：

| 组件 | 出现在 | 许可 / 备注 |
| --- | --- | --- |
| lmxxf runtime（`LmxxfNrRuntime.dll` + 模型权重/资产树） | AMD 版运行时依赖 | 运行时由 lmxxf 源码本地以 MinGW 构建；上游头文件标注 `Origin: lmxxf's runtime, Copyright (c) 2026 Kien (MIT)`。模型权重与编译好的着色器以**目录树**形式随整合包提供（`HIP\`、`block*-*.f16/.f32`、`noise.f32`、`normalized-output.f32`、`native_*.cso`、`shaders\` 等，约 600 MB），**仅作为 Release 附件提供，不在本仓库内**，也不受本仓库 MIT 覆盖 |
| AMD FidelityFX 上采样运行库（`amd_fidelityfx_upscaler_dx12.dll`、`amd_fidelityfx_loader_dx12.dll`） | AMD 整合包（`AMDNR_XR_UPSCALE` 功能用） | AMD 的 FidelityFX 二进制，随整合包提供，不在本仓库内 |
| AMD FidelityFX Super Resolution 头文件（`ffx_a.h`、`ffx_fsr1.h`） | AMD 版与 NVIDIA 版编译期 | MIT，Copyright (c) AMD；随构建复制到输出目录 |
| OpenComposite runtime（`vrclient_x64.dll` 等） | 整合包中 | 见 OpenComposite 上游仓库的许可；整合包内提供，不在本仓库内 |
| NVIDIA DLSS-NR 预发布运行时（`nvngx_dlssnr.dll`，约 158 MB） | NVIDIA 版运行时依赖 | NVIDIA 预发布组件，**仅作为 Release 附件提供**，不在本仓库内，不受本仓库 MIT 覆盖 |
| NVIDIA NGX SDK 头文件 | NVIDIA 版编译期 | 来自本地 DLSS SDK（`external/DLSS/include`），不在本仓库内；`caller/nvngx.dll` 是**本仓库源码 `nvidia/caller_shim.cpp` 构建出来的**，不是 NVIDIA 的二进制，命名只为满足运行时的模块名检查（见 `nvidia/build.ps1` 注释） |
| OpenXR-SDK 头文件 | 两版编译期 | Apache-2.0（Khronos），不在本仓库内 |

## 在整合包 / Release 附件中再分发的注意

Release 附件（两个整合包 zip）里含有上表中的第三方二进制（lmxxf runtime 及其权重资产树、AMD FidelityFX 上采样运行库、OpenComposite runtime、NVIDIA 预发布 DLSS-NR）。它们的再分发条款由各自的上游决定，本仓库作者不对其授权状态作任何声明；如需再分发，请自行向对应上游确认。

## 本仓库自己那部分的归属

- 层的源码（`amd/`、`nvidia/` 下的 `.cpp/.h`）、构建与安装脚本、文档：MIT，Copyright (c) 2026 3zwr1 (AMDNR)。
- `amd/LmxxfNrApi.h` 保留其上游署名（lmxxf / Kien，MIT），本层对其的修改在同一文件头注明。