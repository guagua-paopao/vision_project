# P3 固定推理工作池阶段记录

> 状态：完成
>
> 完成日期：2026-07-23（Asia/Shanghai）
>
> 退出门禁：PostgreSQL 17 全量回归 14/14 通过，0 失败、0 跳过

## 1. 当前结果

CameraPipeline 的分析帧已接入 `CameraInferencePool`，形成以下独立服务内链路：

```text
HTTP Camera CRUD/启停
  -> Redis Run 命令
  -> 每 camera_id 一条 CameraPipeline 线程
  -> 每 camera 最新一帧的有界调度
  -> 启动时固定 W 个推理 Worker / ModelRunner
  -> 每 camera_id 有状态算法 Session
  -> PostgreSQL 告警事件 + callback_outbox
```

本阶段没有为每个摄像头创建模型实例。`analysis.inference_workers=W` 只在 Worker
进程启动时读取；启动时创建恰好 W 个线程和 W 个 `ModelRunner`，运行期间摄像头
增删改查不会改变模型实例数。

## 2. 配置契约

Server 与 Worker 使用一致的部署配置：

```yaml
analysis:
  enabled: true
  inference_workers: 2
  model_init_timeout_ms: 120000
  supported_algorithms:
    - people_flow
    - security
    - electronic_fence
    - pose_action
    - temporal_action
```

- `inference_workers` 限制为 1–16，代表固定 GPU 推理并行度，不等于摄像头线程数。
- `model_init_timeout_ms` 限制为 1–600 秒。
- `supported_algorithms` 是部署白名单；解析后排序、去重。
- 分析功能关闭、白名单外算法或空算法集合会在 HTTP 边界拒绝。
- 任一 Worker 的模型创建、初始化或超时失败都会使整个池启动失败，不能以部分容量
  对外提供服务。

## 3. 调度与生命周期不变量

1. `camera_id` 使用稳定 FNV-1a hash 映射到一个固定 shard；同一摄像头的有状态算法
   始终在同一推理 Worker 上串行执行。
2. 每个摄像头在一个正在推理的帧之外最多保留一个 pending 帧；新帧替换旧 pending
   帧并增加 `replaced_jobs`，不会形成无界延迟队列。
3. 每个 Run 的 `source_sequence` 必须严格单调；重复或回退序号被拒绝。
4. 新 `run_id` 会增加 generation。停止、删除或替换 Run 时，Pipeline 调用
   `detachCamera`；旧 generation 即使已在推理，也不能进入算法处理和告警落库。
5. WorkerHost 关闭顺序为 Camera Manager → 推理池 → 算法 Processor →
   People Flow → FrameHub，保证不再接收新帧后才释放模型和状态。
6. 推理池停止时 join 全部 W 个线程并释放恰好 W 个 runner；启动失败同样回收全部
   已创建 runner。

池快照已包含：

- `running`、`workers_configured`、`workers_ready`；
- `active_cameras`、`pending_cameras`；
- `submitted_jobs`、`replaced_jobs`、`processed_jobs`；
- `failed_jobs`、`stale_results`。

P5 将把这些内部快照接入统一 HTTP/Prometheus 观测面。

## 4. 算法处理与告警一致性

`CameraAlgorithmProcessor` 以 `camera_id + run_id` 维护独立的检测适配器、人物
Tracker、过线计数器和安全分析状态。当前部署白名单对应：

- `people_flow`：过线 `PEOPLE_FLOW_IN/OUT`；
- `security`：启用全部现有安全分类；
- `electronic_fence`：区域类事件；
- `pose_action`：姿态规则事件；
- `temporal_action`：现有时序 DEMO 事件。

告警 `event_id` 和 `fingerprint` 由稳定输入做 SHA-256 生成。相同业务事件重复处理
命中唯一约束，计入 `duplicate_alerts`，不会创建第二条事件。

若任务配置 `callback_profile`，`security_alert_events` 与 `callback_outbox` 的 pending
记录在同一个 PostgreSQL 事务中提交；任一写入失败都会回滚。本阶段只负责可靠地产生
outbox，P4 才负责 HTTP 投递、重试、退避和死信。

## 5. 变更文件

固定池和算法处理新增：

- `include/server/camera_inference_pool.h`
- `src/server/camera_inference_pool.cpp`
- `include/server/camera_algorithm_processor.h`
- `src/server/camera_algorithm_processor.cpp`
- `tests/camera_inference_pool_test.cpp`
- `tests/camera_algorithm_processor_test.cpp`

运行时集成：

- `include/business/camera_pipeline.h`
- `src/business/camera_pipeline.cpp`
- `include/server/camera_task_runtime.h`
- `src/server/camera_task_runtime.cpp`
- `include/server/vision_worker_host.h`
- `src/server/vision_worker_host.cpp`
- `src/server/main_people_flow_worker.cpp`
- `include/server/model_runner.h`
- `src/server/model_runner.cpp`

配置、HTTP 与存储：

- `include/server/app_config.h`
- `src/server/app_config.cpp`
- `config/server.yaml`
- `config/worker.yaml`
- `src/server/camera_task_http_controller.cpp`
- `include/business/camera_task_repository.h`
- `src/business/camera_task_repository.cpp`

构建、测试与文档：

- `CMakeLists.txt`
- `scripts/build_backend.ps1`
- `tests/app_config_runtime_test.cpp`
- `tests/camera_task_http_contract_test.cpp`
- `tests/camera_task_repository_test.cpp`
- `docs/CAMERA_FRAME_TASK_API.md`
- `docs/ARCHITECTURE.md`
- `README.md`
- `docs/development/README.md`
- `docs/development/ALGORITHM_SERVICE_IMPLEMENTATION_INDEX.md`
- `docs/development/ALGORITHM_SERVICE_P3_INFERENCE_POOL.md`

## 6. 验证证据

构建与无数据库回归：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\build_backend.ps1
```

结果：后端编译成功；14 项中 8 passed、0 failed，6 项因未配置一次性测试 DSN 按设计
skip。

数据库验收使用随机容器名、随机宿主端口和随机密码启动
`postgres:17-alpine`，设置 `YOLO11_TEST_POSTGRES_DSN` 与破坏性测试保护开关后执行：

```powershell
D:\vs2019\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe `
  --test-dir .\out\build\backend-Release -C Release --output-on-failure
```

最终结果：

- 14/14 passed；
- 0 failed；
- 0 skipped；
- 总耗时 8.33 秒；
- 一次性 PostgreSQL 容器已在 `finally` 中删除；
- 测试 DSN 和随机密码未写入文件。

新增确定性证明包括：

- 固定创建 W=2 个 runner，运行期间不动态增长；
- 同 camera 稳定 shard affinity；
- in-flight + 最新 pending 的序列结果为 `[1, 3]`，被替换的 2 不执行；
- 非单调序号被拒绝；
- detach 后旧 generation 推理结果被抑制；
- 单个模型初始化失败时整个池失败关闭并释放 2/2 runner；
- 合成过线帧产生一条 PostgreSQL `PEOPLE_FLOW_IN` 告警和一条 pending outbox；
- 白名单外算法在 Processor 明确失败，并在 HTTP 边界返回 400。

## 7. 回滚与剩余风险

P3 没有新增数据库表；复用 P1 已冻结的告警与 outbox schema。安全回滚方式：

1. 将 Server/Worker 两侧 `analysis.enabled` 设为 `false`；
2. 停止新的分析 Run；
3. 回退 P3 Worker 二进制；保留 PostgreSQL 事件和 outbox 数据。

剩余风险：

- 本阶段的 runner 调度使用 fake runner 做并发确定性测试；真实 TensorRT 多 runner、
  GPU 显存余量和真实 RTSP 长稳数据属于 P6 硬件验收门禁。
- outbox 目前只会可靠落库，不会主动投递；必须完成 P4 才能声明告警 HTTP 回传闭环。
- 推理池和 Processor 快照尚未暴露到统一运维 API；由 P5 完成。
- `temporal_action` 仍是既有 `RAPID_MOTION_DEMO`，不是训练后的暴力/打架分类器。

## 8. P4 入口门禁

P4 必须从 `callback_outbox.status=pending` 开始，实现 callback profile 的 URL/认证
解析、HTTP POST、超时、指数退避、最大尝试次数、可观测错误和 dead-letter 状态。
只有后端收到稳定 `event_id` 且重复投递可安全去重，才可关闭 P4。
