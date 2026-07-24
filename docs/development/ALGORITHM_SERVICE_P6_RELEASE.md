# P6 真实硬件验收与发布阶段记录

> 状态：完成
>
> 开始日期：2026-07-24（Asia/Shanghai）
>
> 原则：只有真实 RTSP、TensorRT/GPU、Postman、故障恢复与长稳证据全部
> 关闭后，才将本记录改为“完成”

## 1. 验收目标

P6 不再以合成帧或 CPU 确定性输入替代部署证据。验收对象为一台用户授权的
本地测试摄像头、当前仓库内唯一 Pose TensorRT engine、RTX GPU、真实
PostgreSQL/Redis、真实 Server/Worker 进程以及 HTTP callback。

RTSP 凭据只注入当前进程的 `YOLO11_CAMERA_ENTRY_URL`，不写入 YAML、
PowerShell 参数文件、运行证据、Git 或 Postman 环境文件。阶段记录只保留
视频规格和 Profile，不记录内网源地址或账号密码。

## 2. 已确认硬件与视频输入

- GPU：NVIDIA GeForce RTX 4080 Laptop GPU；
- 驱动：581.80；
- 显存：12282 MiB；
- TensorRT engine：
  `engines/yolo11n-pose.engine`；
- engine SHA-256：
  `e911f38f1c0797453b09bb0d1b162aaa2b764dbb1db8a82c3a96d9fe4cd29e8f`；
- Camera Profile：`entry_camera_01`；
- 视频：H.264 High、1920×1080、25 FPS；
- Postman Desktop：12.20.4；
- 自动 Collection Runner：Newman 6.2.2、Node.js 22.15.0；
- 基础设施镜像：PostgreSQL 17 Alpine、Redis 7 Alpine。

## 3. 已完成的真实门禁

### 3.1 RTSP 与 GPU 烟测

首次独立验收结果：

- `ffprobe` 成功读取 H.264 High、1920×1080、25 FPS；
- 15 秒 `rtsp_capture_smoke`：
  `open_count=1`、`reconnect_count=0`、序列推进 464；
- TensorRT engine 哈希一致并在目标 GPU 初始化成功；
- 15 秒 `pose_rtsp_interop_smoke`：
  `open_count=1`、`reconnect_count=0`、117 次推理。

### 3.2 真实算法告警链路

一次性 PostgreSQL/Redis、真实 Server/Worker、真实 RTSP、2 个固定推理
Worker 和 loopback HMAC 后端运行时，
`verify_algorithm_service_p5.ps1` 已完成：

```text
HTTP Camera JSON
  -> PostgreSQL Camera/Run
  -> Redis command
  -> per-camera Pipeline
  -> fixed inference pool
  -> algorithm alert + outbox
  -> retry after injected HTTP 503
  -> signed callback delivered
```

验收 Camera 在 `finally` 中停止并软删除；数据库、Redis、Bearer Token 和
HMAC secret 均为该轮随机生成的一次性值。

### 3.3 Postman

真实部署链路上的 Newman 运行结果：

- 18 个请求；
- 16 个测试脚本；
- 16 个断言；
- 0 请求失败；
- 0 断言失败；
- 人工 dead-letter replay 按默认安全开关跳过。

P6 修正了两个真实 Runner 问题：

1. Stop 后保存新的 ETag，Delete 不再以旧版本触发 409；
2. `disabled` UI 属性不能作为 Newman 安全边界，因此增加
   `enable_dead_letter_replay=false` 和 `pm.execution.skipRequest()`。

### 3.4 Worker 恢复、RTSP 重连与 dead-letter

2026-07-24 新二进制真实摄像头回归结果：

- 强制终止 Worker 前确认恰好 1 个 FFmpeg reader；旧 reader 随 Worker
  退出，孤儿进程数为 0；
- 新 Worker 等待旧 Redis lease 过期后，将旧 Run 持久化为
  `failed / WORKER_RESTARTED`，创建 `cr_recovery_*` 新 Run 并恢复运行；
- 强制终止共享 FFmpeg reader 后，保持同一 Hub instance，
  `open_count: 1 → 2`、`reconnect_count: 0 → 1`，源序列继续推进；
- 重连后订阅仍为 `camera_task=1 / people_flow=1`，未发生每个业务各开
  一路 RTSP；
- callback 在 2 次失败预算后进入真实 `dead_letter`，人工重放返回
  `retry`，接收端观察到事件且数据库最终状态为 `delivered`。

### 3.5 60 分钟长稳与全量回归

正式长稳时间为 2026-07-24 10:22:17 至 11:22:13
（Asia/Shanghai）：

- 682 个 5 秒级采样，0 违规；
- 全程 1 个 Hub instance，`open_count=1`、`reconnect_count=0`、
  `subscriber_count=2`；
- 源序列由 462 推进至 90363，保存帧由 0 推进至 3596；
- 固定推理池全程 `workers_ready=2`；
- CPU 峰值 7.28%，Server+Worker Working Set 峰值 498.66 MiB；
- GPU 显存峰值 1885 MiB、利用率峰值 75%、温度峰值 55°C；
- 项目盘使用率峰值 72%，存储压力保持 `normal`；
- 前 5 分钟/后 5 分钟 Working Set 均值为 473.30/477.01 MiB。

新代码随后在一次性 PostgreSQL 17 + Redis 7 上完成两轮完整 CTest：
19/19 通过；新增的 `camera_task_lease_fence_test` 真实验证相同逻辑 consumer
的两代 Worker 不能同时 acquire/refresh/release 同一 Run lease。Qt 合同、
mock callback 合同、Postman JSON、PowerShell/Node.js 语法与发布守卫全部通过。

## 4. 一键验收与证据

新增 `scripts/verify_algorithm_service_p6.ps1`。脚本：

1. 校验 engine SHA-256 并执行 engine/RTSP/TensorRT 互操作烟测；
2. 在随机 loopback 端口创建一次性 PostgreSQL 17 与 Redis 7；
3. 生成随机数据库密码、管理员 Token、callback HMAC 与 mock control token；
4. 仅在 `out/tmp` 生成启用 loopback callback 的临时 Server/Worker 配置；
5. 启动真实 Server/Worker；
6. 强制终止 Worker，验证租约围栏、旧 Run 审计和新一代 Pipeline 恢复；
7. 启动 People Flow 会话，强制终止共享 FFmpeg reader，验证原 Hub 自动重连且
   Camera Task 与 People Flow 两个订阅者保持；
8. 执行真实告警链路与 Postman Collection；
9. 通过持续受控 503 故障注入产生真实 dead-letter，再执行人工重放并确认
   持久化状态最终为 `delivered`；
10. 执行 60 分钟共享 Hub 长稳并采集主机/GPU 遥测；
11. 在 `finally` 中停止进程、删除容器、恢复环境变量并清理临时配置；
12. 对保留日志再次执行 RTSP URI 与随机 secret 脱敏。

证据目录：

- `reports/p6/<UTC stamp>`：总门禁、烟测、Postman、真实告警和日志；
- `reports/p6/soak/<UTC stamp>`：5 秒粒度 JSONL 和最终摘要。

`reports/`、`runtime/` 与 `out/` 均由 `.gitignore` 排除。Git 只记录脱敏后的
汇总结论，不上传视频帧、数据库、完整运行日志或凭据。

## 5. 长稳遥测定义

`soak_camera_frame_feature.ps1 -CaptureHostTelemetry` 每个样本记录：

- health/readiness；
- Camera/Pipeline 状态；
- Hub instance、open/reconnect、订阅总数与订阅类型；
- 固定推理池 ready/active/pending/job 计数；
- callback Worker 与 outbox 状态；
- Server/Worker 归一化 CPU、累计 CPU 与 Working Set；
- GPU 显存、利用率和温度；
- 项目盘剩余空间与使用率。

任何 readiness 失败、Camera failed、多 Hub、无解释的 reopen、订阅数不一致、
People Flow 订阅丢失或主机遥测失败都会使长稳失败。

## 6. P6 开发中发现的问题

1. `verify_algorithm_service_p5.ps1` 绑定旧 PowerShell 响应具体类型，已改为
   兼容对象合同；
2. `-ControlPlaneOnly` 错误要求 callback Worker 运行，已修正；
3. Camera 清理改为从正文读取最新版本并等待异步停止；
4. 原长稳只采集服务指标，P6 增加 CPU、内存、GPU 和磁盘证据；
5. 启动脚本原先写死生产 YAML，现支持显式的 Server/Worker 配置路径，
   从而无需修改安全默认值即可使用临时验收配置；
6. 仅创建 Camera Task 时不会自动出现 People Flow 订阅，P6 现在显式启动
   People Flow 会话后再验证共享 Hub。
7. Worker 被强制终止时，外部 FFmpeg reader 可能成为孤儿进程；Windows
   运行时现使用 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 约束子进程生命周期。
8. Worker 重启不能只按时间把旧 Run 判为陈旧，且只把 `run_id` 存入 lease
   不能区分同一 Run 的新旧 Worker；现在 lease 同时包含唯一 Worker 实例令牌，
   新进程必须等待旧租约释放或过期，再将旧 Run 记为 `WORKER_RESTARTED`，
   为每个仍处于 `desired_state=running` 的 Camera 创建新 Run，并在消费队列
   前恢复 Pipeline。
9. 固定 `fail_first=2` 在多告警并发时可能由两个不同事件分别消耗；P6 改为
   持续失败直至至少一条事件耗尽两次预算，重放后同时检查接收端和数据库
   `delivered` 状态。
10. 用户提供的 `F:\postman` 目录未包含可执行文件；验收脚本按
    `%LOCALAPPDATA%\Postman\Postman.exe` 自动发现实际 Desktop 安装，并记录
    版本而不记录用户目录。
11. 普通 Windows 会话可能拒绝 `Win32_Process` CIM 枚举；P6 使用
    Tool Help 原生进程快照读取 Worker 的 FFmpeg 子进程，不要求管理员权限，
    同时避免把“无法查询”误报成“没有孤儿进程”。

## 7. 门禁关闭状态

截至实现提交前：

- [x] 正式 60 分钟长稳结束且 0 违规；
- [x] 对真实 RTSP 做受控 reader 终止与自动重连；
- [x] 验证 Worker 崩溃/重启、租约围栏、旧 Run 审计与新 Run 恢复；
- [x] 完成真实 dead-letter、人工重放和最终 `delivered` 演练；
- [x] PostgreSQL 17 + Redis 7 全量 CTest 19/19 和发布守卫；
- [x] Postman 18 请求、16 断言、0 失败；
- [x] 发布 P6 分支与草稿 PR，并在本记录追加提交和 PR 链接。

本次发布目标是用户提供的单个物理 Camera Profile，因此第二个物理摄像头
不属于本次发布门禁。未来扩展为多物理 Profile 时，必须单独补充并发和资源
证据，不能用同源的两个逻辑订阅者代替。

## 8. 回滚

P6 当前不修改数据库 schema 或生产安全默认值。回滚步骤：

1. 停止 `four_stage_server`、`four_stage_worker` 和 mock 进程；
2. 删除名称前缀为 `vision-p6-postgres-`、`vision-p6-redis-` 的精确一次性容器；
3. 清除当前进程的 P6 环境变量；
4. 删除校验过位于 `out/tmp/p6_<stamp>` 的临时配置；
5. 回退 P6 脚本、Postman 和文档提交到 P5 检查点；
6. 保留 `reports/p6` 脱敏证据供问题分析，或按本地数据策略删除。

真实 RTSP 源、GPU 驱动、TensorRT engine 和 Postman 用户数据不由 P6 脚本
修改，因此不需要设备侧回滚。

## 9. GitHub 检查点

- 分支：`agent/p6-hardware-acceptance`
- 实现提交：
  [`863d88a7e6a2d0c16d62914116f8125a817a5e50`](https://github.com/guagua-paopao/vision_project/commit/863d88a7e6a2d0c16d62914116f8125a817a5e50)
- 草稿 PR：
  [guagua-paopao/vision_project#6](https://github.com/guagua-paopao/vision_project/pull/6)
- PR 基线：`agent/p5-integration-observability` /
  `b8f7ebf6bb105e5b5e8ac6bcc804c3fcc7dd896c`

实现提交冻结 P6 代码、测试和首次目标硬件证据；本次纯文档检查点只补充
GitHub 链接与完成状态，不改变实现提交对应的验收结论。堆叠合并顺序为
PR #5 → PR #6；P5 进入目标分支后，再将 PR #6 的 base 调整到该目标分支。
