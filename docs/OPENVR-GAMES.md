# 把层带到 OpenVR 游戏上（OpenComposite 桥）

层是 **OpenXR 隐式 API 层**，只在应用走 OpenXR 时生效：原生 OpenXR 游戏（如 Beat Saber 1.42）直接覆盖；**OpenVR 游戏必须先经 OpenComposite 翻译到 OpenXR**，层才够得着。这份文档记录这条桥的现状、安装方式与实机边界。

## 桥的构成（本机在用的构建）

在上游 [OpenComposite](https://gitlab.com/znixian/OpenOVR/-/tree/openxr)（openxr 分支，GPLv3）之上加了：

- **新版 OpenVR 接口注册**：`IVRCompositor_029`、`IVRSystem_026`、`IVROverlay_028`、`IVRInput_011`（由 OpenVR 2.15.6 头文件生成）。用 OpenVR 2.x SDK 构建的新游戏会点名要这些接口，老构建没注册就会起不来/退回 SteamVR。
- **眼动查询的真实现**：`IVRInput_011::GetEyeTrackingDataRelativeToNow / ForNextFrame` 由 `XR_EXT_eye_gaze_interaction` 供数（绑定 `/user/eyes_ext/input/gaze_ext/pose`、profile `/interaction_profiles/ext/eye_gaze_interaction`）。标准 gaze 只给 origin+direction，没有注视深度，`vGazeTarget` 取视线方向 1 米处；运行时没有眼动时返回 inactive，不编造数据。
- 注意区分：**层自己的眼动不依赖这条桥**——层自建 `amdnr_eye_gaze` action set 从运行时直接取数（见 `docs/ENVIRONMENT.md` 的 gaze 项）；桥的这份眼动是给**游戏**用的。

构建物是 `vrclient_x64.dll`（VS 2022 + CMake，见 `opencomposite-src` 目录的 CMake 工程）。

## 两种安装方式

1. **单游戏投放（推荐，改动最小）**：把 `vrclient_x64.dll` 改名成**恰好 `openvr_api.dll`**（名字就是功能的一部分：游戏按这个名字找 DLL），备份并替换游戏目录里原来那份——UE 游戏通常在 `Engine\Binaries\ThirdParty\OpenVR\<版本>\Win64\`。还原 = 把备份换回去。
2. **全局路由**：把 OpenComposite 目录加进 `openvrpaths.vrpath` 的 `runtime` 列表，并用 `apps-config.json` 按游戏指定 `runtime: 1`（其余默认走 SteamVR，`default_runtime: 2`）。注意 **SteamVR 每次启动都会把自己排回列表首位**，全局路由随时可能失效，这是单游戏投放更省事的原因。

## 实测边界（重要）

- **Pimax PiOpenXR 每进程只允许一个 XR 实例**。探针实测（`xr-ext-probe`）：同一进程里并发创建第二个实例一律返回 `-10`（`XR_ERROR_LIMIT_REACHED`，与扩展清单无关——先销毁已建实例后，同一份全量清单逐项验证均 `XR_SUCCESS`；含 `XR_EXT_eye_gaze_interaction` 与 `XR_KHR_win32_convert_performance_counter_time` 的 7 项全量列表也成功）。
- 因此**已经自己用 OpenXR 的游戏不要再注入 OpenComposite**。实测 Kayak VR：它的 UE OpenXR 插件与 OpenVR 并存，注入后 OCOVR 会在同进程建第二个实例 → `-10` → OpenComposite 把 create 失败当致命 → 游戏 VR 初始化直接失败。这类游戏让它自己走 OpenXR（层会自然生效），或留在 SteamVR。
- **纯 OpenVR 游戏**（Alyx、Arizona Sunshine 等）单实例，桥可用；Alyx 已实机跑通（本机经 `apps-config.json` 只路由 Alyx）。
- 层侧对这条桥的配合：`xrCreateInstance` 注入眼动扩展前**先查重**（应用已启用就不重复追加），且任何创建失败都会去掉眼动重试一次——它必须容忍桥，因为 OpenComposite 把 create 失败当致命。

## 许可

OpenComposite 系（含这条桥的构建）是 **GPLv3**，与上游一致。若随整合包一起分发，[NOTICE.md](../NOTICE.md) 必须单独标注它的许可，不能只挂本仓库的 MIT。