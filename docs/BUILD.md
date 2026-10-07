# 构建

两版都是"一个 `cl.exe` 直接编译"，没有解决方案/工程文件需要同步。`build.ps1` 负责：找 MSVC 与 Windows SDK → 组 `INCLUDE`/`LIB` → 编译 → 把清单与 FSR 头文件复制到输出目录。

## 工具链

- Visual Studio 2022 Build Tools（MSVC x64）。脚本按 `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\<最新版本>` 与 `C:\Program Files (x86)\Windows Kits\10` 自动选择最新版。
- 编译参数：`/W4 /O2 /EHsc /std:c++20 /utf-8 /LD /MD`（NVIDIA 版的 caller shim 额外用 `/std:c++17 /Od /MT`，`/Od` 是功能性的，见下）。

## 头文件依赖（两版 `build.ps1` 顶部的三个路径）

| 变量 | 内容 | AMD 版 | NVIDIA 版 |
| --- | --- | --- | --- |
| `$openXrInc` | OpenXR-SDK 的 `include` | 必需 | 必需 |
| `$fsrInc` | FidelityFX-FSR 的 `ffx-fsr`（`ffx_a.h`/`ffx_fsr1.h`，同时会在运行时被着色器 `#include`） | 必需 | 必需 |
| `$ngxInc` | DLSS SDK 的 `include`（NGX 头文件，仅编译期需要；入口点运行时按名字解析） | — | 必需 |

脚本里的绝对路径是作者机器上的布置；换机器时改这三行（以及 `$output`）即可。

## 输出

- AMD 版：`C:\Users\Administrator\vdxr\xr-layer\AmdnrXrLayer.dll` + `AmdnrXrLayer.json` + `ffx_a.h` + `ffx_fsr1.h`。**运行时依赖**：同目录还要有 `LmxxfNrRuntime.dll` 与 `LmxxfNrRuntime.pak`（来自 Release 附件），否则层会报 `LmxxfNrRuntime.dll / .pak not found` 并拒绝启动 NR。
- NVIDIA 版：`C:\Users\Administrator\vdxr\xr-layer-ngx\AmdnrXrLayer.dll` + 清单 + FSR 头 + `caller\nvngx.dll`。**运行时依赖**：`nvngx_dlssnr.dll`（Release 附件）与驱动里的 NGX 核心。

## 两条容易踩的坑

1. **AMD 版重编译前先关游戏**。游戏进程加载着 `AmdnrXrLayer.dll`，DLL 被占用时链接会失败（`LNK1104`）。NVIDIA 版输出到独立目录，不受影响。
2. **NVIDIA 版的 caller shim 名字不能改，`/Od` 不能省**。DLSS-NR 运行时会检查"调用是否来自它所认可的模块"（返回地址所属模块 + 模块名），否则一律回 `0xBAD00002`；优化器把转发调用变成跳转会让返回地址又落回本层，从而重新被拒。详见 `nvidia/build.ps1` 顶部的注释。

## 契约检查（AMD 版）

`amd/check-panel-ranges.ps1` 在编译前运行：它比对面板 HTML 里每个滑块的 `min/max` 与 `NrSettingsSet` 的 clamp 范围，确保面板不会给用户一个层会悄悄改掉的值（当前 `30 checked, 0 failed`）。改了任何一边都要让这个脚本继续通过。

## 独立测试程序

- `amd/nr-quality-controller-test.cpp`：自动画质档控制器的规则测试（档位迟滞、重建帧不计入测量等）。构建脚本：`amd/build-quality-controller-test.ps1`。
- `amd/nr-addon-bridge-test.cpp` / `nvidia/nr-addon-bridge-test.cpp`：addon 配置桥的键名归一化冒烟测试（喂 ini 与 slot 串，检查解析结果）。

## 运行时的位置与选择

`NrCore::LoadRuntime()` 按顺序找 `C:\Users\Administrator\vdxr\xr-layer`、`C:\Users\Administrator\vdxr\bin\x64\Release`，取第一个同时有 `LmxxfNrRuntime.dll` 与 `.pak` 的目录并 `SetDllDirectory` 到那里。层只导出 `LmxxfNrGetApi` 就可以工作；`LmxxfNrGetImportPoolStats`、`LmxxfNrSetTierPolicy` 等可选导出存在就用、不存在就安静跳过（日志里不会出现对应的行）。