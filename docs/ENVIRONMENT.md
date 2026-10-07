# 环境变量

两版的变量都在 `NrSettingsInitFromEnvironment()`（`NrSettings.h` 附近）里读一次，之后由面板的实时设置块接管可以改的那部分；下面的"仅启动时"是指必须重开会话才生效。

## AMD 版（`amd/`）

| 变量 | 默认 | 作用 |
| --- | --- | --- |
| `AMDNR_XR_CROP` | `1920x1080` | **网络工作分辨率上限**（`WxH`），也就是面板 `ceilw/ceilh` 的初值 |
| `AMDNR_XR_COVER` | `1.0` | 窗口覆盖上限的比例（面板 `winw/winh` 初值），1.0 = 与上限同尺寸（1:1，最清晰） |
| `AMDNR_XR_STRENGTH` | `1.0` | 强度（进网络的强度对） |
| `AMDNR_XR_TEMPORAL` | `0` | 打开运行时的时域历史（每眼一个会话）。**仅启动时** |
| `AMDNR_XR_ASYNC` | `0` | 异步布局（本帧提交、下帧收，省掉 CPU 等 GPU 的 ~112 ms）。**当前有挂死设备的缺陷，默认关闭**，见 TROUBLESHOOTING |
| `AMDNR_XR_NR_OFF` | — | 置位即让 NR 不工作（等同面板总开关关闭） |
| `AMDNR_XR_GAIN` / `AMDNR_XR_GAMMA` | `1.0` / `0.95` | 网络答案的亮度/伽马校正 |
| `AMDNR_XR_FEATHER` / `AMDNR_XR_ROUNDNESS` | `0.15` / `0.0` | 窗口边缘羽化宽度（窗口短边的比例）/ 形状（0 矩形 → 1 内切椭圆） |
| `AMDNR_XR_ANTIFLICKER` / `AMDNR_XR_ANTIFLICKER_GATE` | `0` / `0.03` | 抗闪烁：历史保留上限 / 纠偏门限 |
| `AMDNR_XR_MOTION_NEAR` | `0` | 运动场用的近平面（米）；0 = 只做旋转 |
| `AMDNR_XR_GAZE_DEAD_ZONE` / `AMDNR_XR_GAZE_MAX_STEP` | `128` / `0` | 窗口跟随眼动的死区 / 单帧最大步进 |
| `AMDNR_XR_RENDER_SCALE` | `1.0` | 向应用建议的眼缓冲区缩放（<1 减少应用渲染像素） |
| `AMDNR_XR_UPSCALE` | `0` | 全分辨率输出通路（层自己持有一条推荐尺寸的交换链并做 FSR4 上采样） |
| `AMDNR_XR_UPSCALE_SHARPEN` | `0.3` | 上述通路的锐化量 |
| `AMDNR_XR_FSR4` / `AMDNR_XR_JITTER` / `AMDNR_XR_JITTER_SIGN` | `0` / `0` | FSR4 后端与抖动注入（诊断用） |
| `AMDNR_XR_SKIP` / `AMDNR_XR_SKIP_STALE` | — | 跳过本层的 pass（分别用于纯基线测量与跳过陈旧帧） |
| `AMDNR_XR_DEBUG_VIEW` | `0` | 运行时调试视图（1 proxy / 2 神经单独输出 / 3 差分 20× / 4 染色） |
| `AMDNR_XR_DUMP` | — | 把前后画面写成 PPM 到 `dump/`（限速，读回会冲刷管线） |
| `AMDNR_XR_PANEL_PORT` | `8787` | 面板端口（仅 127.0.0.1） |
| `AMDNR_XR_ADDON_SLOT` | — | 指定 addon 配置来源（见 `NrControlServer.cpp` 的桥） |
| `AMDNR_XR_DEPTH_*` / `AMDNR_XR_NEAR_PROBE` / `AMDNR_XR_CBUF_PROBE` / `AMDNR_XR_DEPTH_LINEAR` / `AMDNR_XR_DEPTH_REGION` | — | 深度捕获与诊断探针（`AMDNR_XR_DEPTH_PROBE`、`_PROBE_SELFTEST`、`_DUMP`、`_ROTATE`、`_SAMPLE`、`_NEAR`） |
| `DLSS5_STRENGTH` / `DLSS5_MULTI_PASS` / `DLSS5_STYLE` | — | 由层转交给运行时/面板的键（`MULTI_PASS` 决定网络的遍数，写入 `custom-config.txt`） |

**由运行时（lmxxf / DLSS5-NR）自己读、层不碰的键**（同样在启动前设置）：`DLSS5_NETWORK_HEIGHT`、`DLSS5_NETWORK_FREE_RES`（不再把输入吸附到 720/900/1080 档，而是补到 64 的倍数）、`DLSS5_SKIP_BLOCKS`、`DLSS5_IMPORT_POOL`（导入池开关）、`DLSS5_FRAME_STATS`、`DLSS5_NET_TIMING`、`LMXXF_NR_TS`。

## NVIDIA 版（`nvidia/`）

与 AMD 版同名同义的有：`AMDNR_XR_CROP`、`AMDNR_XR_COVER`、`AMDNR_XR_STRENGTH`、`AMDNR_XR_TEMPORAL`、`AMDNR_XR_ASYNC`、`AMDNR_XR_NR_OFF`、`AMDNR_XR_ONE_EYE`（只处理一只眼，调试）、`AMDNR_XR_INTENSITY`、`AMDNR_XR_GAIN`、`AMDNR_XR_GAMMA`、`AMDNR_XR_FEATHER`、`AMDNR_XR_ROUNDNESS`、`AMDNR_XR_ANTIFLICKER`、`AMDNR_XR_ANTIFLICKER_GATE`、`AMDNR_XR_GAZE_DEAD_ZONE`、`AMDNR_XR_GAZE_MAX_STEP`、`AMDNR_XR_RENDER_SCALE`、`AMDNR_XR_UPSCALE`、`AMDNR_XR_UPSCALE_SHARPEN`、`AMDNR_XR_SKIP`、`AMDNR_XR_DUMP`、`AMDNR_XR_PANEL_PORT`、`AMDNR_XR_ADDON_SLOT`、`AMDNR_XR_D3D12_DEBUG`、`AMDNR_XR_DEPTH_PROBE` / `_SELFTEST`。

## 面板动作脚本里用的短名（ab-run.ps1）

作者的 A/B 测量脚本用短名映射到上面的长名：`TEMPORAL`、`UPSCALE`、`FSR4`、`JITTER`、`CROP`、`COVER`、`RENDER_SCALE`、`FREE_RES`、`DEPTH_*`、`NET_HEIGHT`、`MULTI_PASS`、`SKIP_BLOCKS`、`FAST_NUMERIC`、`STYLE`。该脚本不在本仓库内（它是作者机器的测量工具），思路是：清掉进程里的 `AMDNR_/DLSS5_/LMXXF_` 前缀变量 → 灌入 User 作用域里保存的基线 → 显式写入本次要测的那几个 → 启动游戏 → 等 N 秒 → 摘日志出摘要。