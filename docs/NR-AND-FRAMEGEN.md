# 与插帧层（OFXR Bridge）共存

本文记录 NR 层与 **OFXR Bridge**（光学流插帧的 OpenXR 层，`XR_APILAYER_XRFrameBridge_*`）的共存契约与验证方法，是"两个 OpenXR 隐式层同链工作"的正式依据。

## 两层各做什么

| | NR（本仓库） | OFXR Bridge |
| --- | --- | --- |
| 位置 | OpenXR 隐式层（**HKLM**，机器级） | OpenXR 隐式层（**per-user / HKCU**，托盘 arm） |
| 对帧做什么 | 每个应用帧：裁剪神经窗口 → DLSS-NR → 贴回眼缓冲（就地改写局部） | 在应用帧之间插入**合成帧**：2X（+1 张）/ 3X（+2 张），带自己的节拍与合成帧环 |
| 数据 | 运行时眼动（gaze）、应用深度（可选）、运动场 | **颜色-only 光流**（NVIDIA OF 硬件，AMD/Intel/老 N 卡自动走 FidelityFX），另有 DLSS/OptiScaler motion-vector 接口 |
| 目标 | 提高"已有帧"的画质与时序稳定 | 提高显示率（约半率游戏最多 +100%） |

结论：**互补而非重复**。NR 的时间域是"复用上一帧答案"（运行时 bridge 的历史输入 = 重投影后的上一帧答案），OFXR 是"造新帧"；合成帧基于 NR 的输出生成，天然拿到降噪后的画面。

## 共存契约

1. **顺序：NR 在上，OFXR 在下。** OFXR 自己的文档已给出同型先例（Cheeky 的 DLSS 层在 OFXR 之上——"读游戏原始帧，bridge 之后再提交"）。本机顺序天然正确：NR 注册在 HKLM、OFXR 在 HKCU，OpenXR loader 先枚举 HKLM。顺序正确时 **OFXR 的合成帧不会经过 NR**，不存在二次降噪或时间域污染。
2. **帧所有权**：NR 只处理应用调用的 `xrWaitFrame / xrBeginFrame / xrEndFrame`；合成帧由 OFXR 在链的下游自行提交。
3. **帧率预算**：OFXR 要游戏稳定 ≥ 半刷新率。合用时 NR 默认**降一档**（窗口 1152×648 或 `AMDNR_XR_RENDER_SCALE=0.7`），跑通后再逐档加回；异步管线（本帧交/下帧收）修复后是这一条的正解。
4. **深度**：`AMDNR_XR_DEPTH_PROBE` 必须保持 **Off**（默认即是）——NR 不向帧里注入合成深度层，避免 OFXR 把合成深度当真深度；只转发应用自带深度。
5. **运行时路线（Pimax）**：默认留在 **PiOpenXR**，保留眼动跟随（`XR_EXT_eye_gaze_interaction`）。代价是 OFXR 在其上的显存开销（OFXR 文档实录：PiOpenXR 会给每张收到的图留一份 VRAM 拷贝且不释放，OFXR 贵约 1.5×）。备用路径是改走 SteamVR（OFXR 更省），代价是 NR 的 `eye_gaze=absent`、眼动跟随失效——两者不可兼得时由使用者取舍。
6. **逃生开关**：NR 侧 `AMDNR_XR_LAYER_DISABLE=1`（manifest 自带 `disable_environment`）；OFXR 侧托盘 **Disarm**。两层可独立关闭，便于二分定位。

## 冲突排查矩阵

| 接触点 | 契约 | 检查方法 |
| --- | --- | --- |
| wait/begin/end 帧循环 | NR 只包应用帧；OFXR 负责造帧与节拍 | 两边日志并排读：NR 的 `negotiated with loader` 次序；OFXR flight log 的层列表与 synthetic 计数 |
| 眼缓冲内容 | NR 就地改写局部；OFXR 只读 NR 的输出 | NR 的 `window ...` / `held` / `waiting` 行；OFXR 无内容告警 |
| swapchain | 双方都不改创建/acquire 语义 | 应用无 swapchain 相关错误 |
| 深度层 | NR 不注入合成深度 | `AMDNR_XR_DEPTH_PROBE` 保持关闭 |
| 帧率前提 | NR 档位不得吃掉半率预算 | NR 面板 fps + OFXR 的 synthetic 判定 |
| Pimax 特有 | 层不建实例（单实例限制与层无关）；每图 VRAM 双份 | 显存实测（任务管理器 / OFXR 日志） |

## 实机验证步骤

1. 选一个**原生 OpenXR** 游戏（Kayak VR / MSFS 2024 / DCS——OpenVR 游戏 OFXR 不加载）。
2. arm OFXR 托盘；确认 NR 的隐式层注册在位（HKLM `...\ApiLayers\Implicit` 值 = 0）。
3. 启动游戏，读两份日志：
   - `AmdnrXrLayer.log`：层加载次序、窗口尺寸/档位、`recreates`、gaze 行、fps；
   - OFXR 的 flight log（托盘 → Open bridge logs）：层列表、合成提交数、半率判定。
4. 判定标准：NR 每个应用帧恰好处理一次、OFXR 按预期提交合成帧、游戏不低于半率、无 device removed / create 失败 → 共存成立。
5. 有冲突时用逃生开关二分（先禁 NR 跑一次、再禁 OFXR 跑一次），对照两份日志定位。

## 互相参考的落点

- NR 手里有**应用深度 + 运动场**，OFXR 是颜色-only 光流且已留出 DLSS/OptiScaler motion-vector 接口——把深度/运动按该接口喂过去可直接减少合成帧在遮挡/转头处的 artifact。
- OFXR 的 D3D11/Vulkan "session bridge"（把 runtime 的 session 建在自己的 D3D12 设备上、游戏侧共享纹理）与 NR 的 EyeInterop 同构；**Vulkan 那条 NR 还没有**，实现经验可互相借鉴。

## 许可

OFXR Bridge 为 **LGPL-3.0-or-later**（其上游声明），与本仓库 MIT 无关；分发其构建物需遵守其条款。