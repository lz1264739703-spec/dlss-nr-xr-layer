# 变更记录

## 2026-10-07 — 冻结版（当前）

这一版的标题是"把改分辨率时的反复冻结，变成一次真实变化一次重建"。

**发布包（发布前重新打包）**
- 此前的两个 zip 是从旧暂存目录直接压的（AMD 层 DLL 是 10-04、NVIDIA 是 10-06 的旧构建），已作废；新包里的层 DLL 与实机冻结构建逐一哈希一致（AMD 14:04、NVIDIA 14:18）。
- AMD 包随**实机验收用的运行时资产树**（本地构建的 `LmxxfNrRuntime.dll` + `HIP\`、权重 `block*-*.f16/.f32`、`noise.f32`、`normalized-output.f32`、`native_*.cso`、`shaders\` 等），并剔除构建与日志产物；AMD 包仍随 FSR4 上采样运行库。
- 两个包的 `CONFIG.txt` 去掉 `AMDNR_XR_ASYNC=1`（会挂设备）；NVIDIA 包另去掉诊断开关（`AMDNR_XR_D3D12_DEBUG` 等）。

**窗口尺寸策略（两版同步）**
- 窗口**尺寸**量化到 64 px 栅格（`WindowBand`）：滑块在同一带内怎么动，尺寸不变。
- **带内保持**（`HoldInBand`，AMDNR 的 3/4 规则）：比当前尺寸更小的请求，只有掉到 75% 以下才被接受；更大的请求直接采纳。
- **停稳门**（400 ms）：设置稳定 400 ms 后才应用新尺寸——拖动过程中窗口不跟手，停手后切一次。
- 三行新日志：`waiting (ask band …)` / `held (ask …)` / `a new size rebuilds the network`。
- 实测（AMD 版，Alyx）：同带内拖 10 次 **0** 次重建；快速连拖 6 步（150 ms/步）**0** 次重建，停手后**恰好 1** 次；`recreates` 计数只在真实跨带时 +1。

**纹理重建的生命周期（两版同步）**
- 共享 crop/result 对换新前先 `DrainQueue()`（Signal + 等队列空闲），旧对进**退役槽**、按 fence 收集后释放（`CollectRetired`/`RetireCurrent`）。此前是"无等待立即 Reset"。
- NVIDIA 版的三族纹理（同步对、逐眼异步对、左眼对）在释放前都加了 drain。

**默认布局固定为同步**
- 新增 `AMDNR_XR_ASYNC`（本帧提交、下帧收，去掉 CPU 等 GPU 的 ~112 ms：实测 `producer wait 112 ms → 0.00 ms`、`d11 pending 300/300 → 0`、同窗口 14.3 → 40.7 fps），但因为**挂死过设备**（`0x887A0006`、`PrepareFrame: GPU drain failed`），默认关闭，代码保留在开关后等待修复。

**面板 / addon 桥（此前完成，本版一并固定）**
- 面板新增 addon 配置读取与粘贴框；配置来源探测顺序 env → 游戏 exe 旁 → DLL 旁；别名表补齐 4 个 NR 键（NVIDIA 版同步时抓到）。
- `check-panel-ranges.ps1` 契约保持 30/30。

**NVIDIA 版**
- 同步上述窗口尺寸策略与 drain/退役释放；编译零警告（`nvidia/build.ps1`，输出 `vdxr\xr-layer-ngx`）。

**诊断结论（写进文档）**
- "切完分辨率帧率回不来"的根因是与层无关的 GPU 抢占：`PimaxHome-Win64-Shipping` 67.6% + `pi_server` 51% + `pi_overlay` 27%；层全关时游戏也只有 9 fps，清理后回到 137–171 fps（层开、跑网络时 1280×704 约 31–41 fps）。见 `docs/TROUBLESHOOTING.md`。

## 更早

- 全分辨率输出通路（FSR4 上采样）、抗闪烁历史（逐眼）、运动场（旋转 + 可选深度的平移项）、眼动跟随窗口（死区 + 单帧步进上限）、深度捕获与三类诊断探针：见 `amd/` 各文件头部注释，它们按时间顺序记录了每一处的动机与实测。