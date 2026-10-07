# 排查

所有结论都尽量附上"在日志里长什么样"，因为这套东西的大部分问题只有日志能回答。日志：层的输出目录（如 `C:\Users\Administrator\vdxr\xr-layer\AmdnrXrLayer.log`）。

## 1. 帧率突然掉到个位数 / 拖完滑块帧率回不来

**先怀疑 GPU 被别人抢**，而不是层。本机实测过一次完整案例：

```
PimaxHome-Win64-Shipping   67.6 % GPU   ← Pimax 头显里的 Home 场景
pi_server                  51.1 % GPU   ← Pimax 合成器
pi_overlay                 27.1 % GPU
hlvr (游戏)                72   % GPU
```

此时把层的总开关关掉（面板 `enabled=0`，层完全不裁剪、不过网络），游戏**仍然只有 9 fps** —— 也就是说"卡"与层无关。结束 `PimaxHome-Win64-Shipping` 与 `pi_overlay` 后，同一状态回到 137–171 fps；层开着跑网络时 1280×704 窗口约 31–41 fps。

查法：

```powershell
Get-Counter '\GPU Engine(*)\Utilization Percentage' -SampleInterval 1 -MaxSamples 2 |
  Select-Object -ExpandProperty CounterSamples |
  Where-Object CookedValue -gt 1 |
  Group-Object { ($_.InstanceName -split '_')[0..1] -join '_' } |
  ForEach-Object { "{0,-12} {1,6:N1} %" -f $_.Name, (($_.Group | Measure-Object CookedValue -Sum).Sum) }
```

同一台机器上"头显没戴/睡着"时，Pimax 合成器会把应用限速到约 14.3 fps（层全关也一样）。判断是否处于该状态的现成指标：日志里 `layer: frame split wait X + render Y ... predicted period Z` —— 当 `wait` 远大于 `Z` 时，帧率是运行时的节拍而不是这个配置的；戴上头显（面板亮起）再测。

## 2. 改分辨率/拖窗口滑块时有 1.6–2.5 s 冻结

这是**后端重建网络**的固有成本，不是层在泄漏或死等。日志形状：

```
layer: window 1408x704 (ask 1408x720, band 1408x704) - a new size rebuilds the network
EyeInterop: crop textures 1408x704 fmt=27
NrCore: fp16 working set 1280x640
EyeInterop: slow pass 1675 ms (layout sync, crop 0, producer 1659 [record 1659 ...], ...)
```

`producer record` 里那一秒多就是运行时在 `PrepareFrame` 里重建网络。冻结版的三条尺寸规则（64 px 量化、<75% 才接受缩小、400 ms 停稳）把**次数**压到"一次真实尺寸变化一次"：同带内拖动 0 次重建，快速连拖 6 步也只有 1 次（等停手后）。

顺带一提：`recreates=NN` 出现在 `NrCore: status "..."` 行里，那是**运行时自己**的网络重建计数——它一直是"窗口尺寸改变了多少次"的最直接证据。

## 3. 窗口滑块"不动"

冻结版设计如此（见 README）：尺寸只在 64 栅格上取值、缩小要掉到 75% 以下、变化要等 400 ms 稳定。实际生效值看 `/state` 与这三行日志：`waiting (ask band ...)`（正在等停稳）、`held (ask ...)`（带内保持）、`a new size rebuilds the network`（真的换了）。想立刻强制回到目标尺寸：把对应滑块拖到目标值的 75% 以下再拖回来。

## 4. `AMDNR_XR_ASYNC=1` 会挂死设备（默认关闭）

异步布局（本帧提交、下一帧收，能去掉 CPU 等 GPU 的 ~112 ms：实测 `producer wait 112 ms → 0.00 ms`、`d11 pending 300/300 → 0`）在本机两次把设备挂掉：

```
NrCore: fp16 working texture creation failed
layer: interop failed: fp16 working texture creation failed     (随后 0x887A0005)
device: removed (0x887A0006) first seen at RecordProducer textures ready
NrCore: PrepareFrame failed (PrepareFrame: color geometry change; GPU drain failed ...)
```

对照：同步布局的会话（同一天、同一份代码）一次都没出现。怀疑点是我为跨眼/跨帧加的 "crop copy 前 Wait / paste 后 Signal" 与运行时内部 drain 形成了死等。**修好之前请保持默认（同步）**。

## 5. 层"放弃"了：`layer: interop failed`

层对 NR 通路里的任何失败采取的策略是**整局放弃**（`the neural renderer stays off for the rest of the session; frames go on through without it`），因为最常见的失败是设备被移除，重试只会刷屏（曾记录 81 秒 6494 行同样的失败）。看到这行就是本局不会再有 NR；重启游戏即可。

## 6. 没有画面变化 / 窗口看不出边界

- 检查 `/state` 里 `enabled` 是否为 1；
- 窗口是一块**局部**补丁：`AMDNR_XR_CROP` 越小、`winw/winh` 越小，补丁越小，越容易被忽略；
- 想看"网络到底改了什么"：`AMDNR_XR_DEBUG_VIEW=2`（只看神经输出）或 `3`（差分 20×），配合 `AMDNR_XR_DUMP` 落 PPM 对比；
- 抗闪烁开着（`AMDNR_XR_ANTIFLICKER>0`）时，差异会被历史平均得很淡，关掉它对比一次。

## 7. 层没被加载

- 检查 OpenXR 隐式层注册（当前用户）：`HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit` 下应有指向 `AmdnrXrLayer.json` 的项；`install.ps1` 写的就是它。
- 游戏侧：部分标题（含 Alyx + OpenComposite 组合）需要 `opencomposite.ini` 放在游戏 exe 同目录或层目录，否则会走 SteamVR 而完全绕过本层。
- 日志里应出现 `layer: negotiated with loader (interface v1)` 与 `layer: creating instance: app=...`；没有就是没加载。

## 8. 日志速查

| 行 | 含义 |
| --- | --- |
| `layer: eye 7906x3171 -> window 1280x704 at (776,728) as 1280x704 srgb=1 gaze=1` | 本会话第一帧的窗口几何 |
| `EyeInterop: pass layout synchronous / asynchronous` | 本会话的 pass 布局 |
| `EyeInterop: 2 sessions, ...` | 打开了第二会话（temporal 或异步布局） |
| `NrCore: status "lmxxf modules_ok=76 hip=1 net=1280x704 ... recreates=0 ..."` | 运行时自述：网络尺寸、模块数、重建次数（每 300 帧一条） |
| `layer: frame N passes=600 avg X ms/pass (Y fps overall)` | 300 帧窗口的真实应用帧率与每 pass 成本 |
| `layer: frame breakdown gap + layer + present` | 帧间隔三分解：应用自己的时间 / 层内时间 / 运行时呈现 |
| `layer: launch avg enqueue X wait Y ms` | 网络入队与等答案的平均耗时 |
| `EyeInterop: slow pass ... (layout sync/async, ...)` | 单次 pass 拆解（超过 60 ms 才打） |