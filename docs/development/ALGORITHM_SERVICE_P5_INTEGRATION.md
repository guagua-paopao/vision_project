# P5 集成、可观测性与 Postman 阶段记录

> 状态：完成
>
> 完成日期：2026-07-23（Asia/Shanghai）
>
> 软件退出门禁：PostgreSQL 17 + Redis 7 全量回归 18/18 通过，
> 0 失败、0 跳过
>
> 硬件门禁：未关闭，真实 RTSP、TensorRT/GPU 与长稳数据由 P6 验收

## 1. 阶段结果

P5 将此前只能在 Worker 进程内部读取的 Pipeline、固定推理池、算法 Processor
和回调 Worker 快照接入统一运维面，并补齐 dead-letter 管理、Postman、模拟后端
与跨模块软件集成测试。

当前独立服务链为：

```text
Postman / 外部后端
  -> four_stage_server Camera HTTP CRUD / lifecycle
  -> PostgreSQL durable definition + Run
  -> Redis command
  -> four_stage_worker / VisionWorkerHost
  -> 每 camera_id 一条 Pipeline 线程
  -> 启动时固定 W 个 inference workers
  -> per-camera algorithm session
  -> PostgreSQL alert + callback_outbox
  -> CallbackDeliveryWorker
  -> HMAC HTTP callback
```

Server 不加载 TensorRT，不读取 RTSP URI 和回调密钥。Worker 通过现有 Redis
heartbeat 发布运行快照；Server 合并 Redis 热状态与 PostgreSQL 持久化统计。

## 2. 跨进程可观测性

新增 `AlgorithmRuntimeSnapshot`，包含：

- `VisionWorkerHost` 运行状态和活动 Pipeline 数；
- 固定推理池配置/就绪 Worker 数、活动/等待摄像头和作业计数；
- Processor 活动 Session、处理帧、持久化/重复告警和失败计数；
- callback profile、claim、delivered、retry、dead-letter、传输失败、租约冲突
  和稳定错误码。

`PeopleFlowInferenceWorker` 在原 Worker heartbeat 中携带该快照，不新增独立
Redis 轮询线程。字段随 heartbeat TTL 自动过期；Server 不把缺失或过期快照解释
为“健康的零计数”。

`GET /api/v1/ready` 新增：

- `algorithm_runtime_available`
- `algorithm_runtime_fresh`
- `algorithm_runtime_generated_at_ms`
- `inference_pool_ready`
- `callback_delivery_ready`

Camera Tasks 启用时，模型池 Worker 数不匹配、Processor 未运行、回调 profile
未就绪或快照过期都会使 readiness 为 false。

## 3. 统一指标

受 Bearer 鉴权保护的端点保持为：

- `GET /api/v1/operations/metrics`
- `GET /api/v1/operations/metrics/prometheus`

JSON 新增 `callback_outbox` 持久化状态计数和 `algorithm_runtime` 热指标。
Prometheus 新增：

- `yolo11_callback_outbox{state=...}`
- `yolo11_algorithm_runtime_available`
- `yolo11_algorithm_runtime_stale`
- `yolo11_algorithm_active_pipelines`
- `yolo11_algorithm_inference_workers{state=...}`
- `yolo11_algorithm_inference_jobs_total{state=...}`
- `yolo11_algorithm_alerts_total{state=...}`
- `yolo11_callback_delivery_total{state=...}`

运维响应不包含 RTSP URI、回调 URL、HMAC 密钥、请求正文或响应正文。

## 4. dead-letter 查询与受控重放

新增：

```http
GET /api/v1/operations/callbacks?status=dead_letter&camera_id=...&limit=20&offset=0
POST /api/v1/operations/callbacks/{outbox_id}/replay
```

查询默认只返回 dead-letter，可使用 `status=all` 做审计。结果包含事件、摄像头、
profile、attempt、HTTP 状态、稳定错误码和响应 SHA-256。

重放必须携带从查询中观察到的 attempt：

```http
If-Match: "3"
Content-Type: application/json

{}
```

只有 `outbox_id + dead_letter + attempt` 同时匹配时，一次原子 PostgreSQL
更新才会把记录移到 `retry` 并重置自动重试预算。迟到或重复操作返回
`409 CALLBACK_REPLAY_CONFLICT`。重放不改变 `event_id`，外部后端仍必须幂等。

## 5. Postman 与模拟后端

交付物：

- `postman/vision_project_p5.postman_collection.json`
- `postman/vision_project_p5.local.postman_environment.json`
- `scripts/mock_callback_backend.js`
- `scripts/test_mock_callback_backend.ps1`
- `scripts/verify_algorithm_service_p5.ps1`

Postman 覆盖 health/readiness、Camera CRUD、ETag 更新、启停、Run/告警审计、
JSON/Prometheus 指标和 callback 运维查询。环境模板中的 token 均为空且类型为
`secret`；禁止提交填充后的环境。

Node.js 模拟后端无第三方包，只监听 `127.0.0.1`。它验证时间戳、event ID、
幂等键和 HMAC，支持首 N 次 503 故障注入，并以 control token 保护事件查询和
清空入口。

`verify_algorithm_service_p5.ps1` 是真机一键链路：创建临时 Camera、PATCH、
START、等待 Pipeline、检查指标、等待 mock 收到签名告警、检查 Run，并在
`finally` 中停止和软删除 Camera。`-ControlPlaneOnly` 只验证控制面，不能替代
P6。

## 6. 跨模块软件集成测试

`algorithm_service_integration_test` 在一个确定性流程中执行：

1. 通过 `CameraTaskHttpController` 的真实 handler 接收 Camera JSON；
2. PostgreSQL 创建 Camera 和内部 Run，fake Redis 控制边界只捕获安全命令；
3. 两张合成帧进入真实 `CameraAlgorithmProcessor` 并产生
   `PEOPLE_FLOW_IN`；
4. 告警和 outbox 在一个事务中成为 `pending`；
5. 真实 `CallbackDeliveryWorker` 和 WinHTTP 向 loopback 后端 POST；
6. 测试端独立验证原始 JSON、event ID、幂等头和 HMAC；
7. loopback 返回 204，最终 HTTP 告警查询观察到 `delivered`。

该测试关闭了 P5 软件集成门禁，但它使用合成帧和 CPU 侧确定性输入，不构成
真实 RTSP 或 GPU 证据。

## 7. 变更文件

运行时和 Redis heartbeat：

- `include/server/algorithm_runtime_snapshot.h`
- `include/server/people_flow_inference_worker.h`
- `src/server/people_flow_inference_worker.cpp`
- `include/server/vision_worker_host.h`
- `src/server/vision_worker_host.cpp`
- `include/server/redis_task_queue.h`
- `src/server/redis_task_queue.cpp`

持久化与 HTTP：

- `include/business/camera_task_types.h`
- `include/business/camera_task_repository.h`
- `src/business/camera_task_repository.cpp`
- `include/server/camera_task_http_controller.h`
- `src/server/camera_task_http_controller.cpp`
- `include/server/people_flow_http_server.h`
- `src/server/people_flow_http_server.cpp`

测试、工具和发布守卫：

- `tests/algorithm_runtime_heartbeat_test.cpp`
- `tests/algorithm_service_integration_test.cpp`
- `tests/camera_task_http_contract_test.cpp`
- `tests/camera_task_repository_test.cpp`
- `scripts/mock_callback_backend.js`
- `scripts/test_mock_callback_backend.ps1`
- `scripts/verify_algorithm_service_p5.ps1`
- `scripts/build_backend.ps1`
- `scripts/test_postgresql_connection.ps1`
- `scripts/verify_camera_frame_feature.ps1`
- `postman/vision_project_p5.postman_collection.json`
- `postman/vision_project_p5.local.postman_environment.json`
- `CMakeLists.txt`

文档：

- `README.md`
- `docs/ARCHITECTURE.md`
- `docs/CAMERA_FRAME_TASK_API.md`
- `docs/CAMERA_FRAME_TASK_OPERATIONS.md`
- `docs/development/README.md`
- `docs/development/ALGORITHM_SERVICE_IMPLEMENTATION_INDEX.md`
- `docs/development/ALGORITHM_SERVICE_P5_INTEGRATION.md`

## 8. 验证证据

编译与无基础设施回归：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\build_backend.ps1
```

结果：编译成功；18 项中 9 passed、0 failed，9 项因未配置一次性 PostgreSQL/
Redis 按设计 skipped，CTest 1.10 秒。

发布守卫：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\verify_camera_frame_feature.ps1 -SkipBuild
```

结果：

- C++ 无基础设施模式通过；
- Qt/API 兼容合同通过；
- M0–M11 与 P5 架构/安全静态检查通过；
- Postman JSON、P5 PowerShell 和 Node.js 语法通过；
- mock 后端真实 HTTP 的首次 503、随后 204、重复 200 和错误 HMAC 401 通过。

最终退出验收使用随机容器名、随机宿主端口和随机 PostgreSQL 密码启动
`postgres:17-alpine` 与 `redis:7-alpine`，设置一次性测试环境后运行完整 CTest：

```powershell
D:\vs2019\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe `
  --test-dir .\out\build\backend-Release --output-on-failure
```

最终结果：

- 18/18 passed；
- 0 failed；
- 0 skipped；
- CTest 总耗时 10.55 秒；
- 新增 Redis heartbeat wire test 和 HTTP→算法→PostgreSQL→真实 WinHTTP
  callback 集成测试均通过；
- PostgreSQL 与 Redis 一次性容器均在 `finally` 中删除；
- 测试 DSN、随机密码和 token 未写入文件。

开发期间发现并关闭两项测试基础设施问题：

1. heartbeat 测试首次链接缺少已有 `app_support` 依赖，CMake 已补齐；
2. 既有发布守卫仍检查 P2 前的 `sessions_` 字段名，已更新为当前
   `pipelines_`，重新执行后通过。

## 9. 回滚与剩余风险

P5 没有新增数据库表或破坏 schema。Redis heartbeat 只增加字段，对旧 Server
为向后兼容的额外 Hash 字段。

安全回滚：

1. 禁止人工 dead-letter replay；
2. 回退 P5 Server/Worker 二进制到 P4；
3. 保留 PostgreSQL Camera、Run、alert 和 outbox 数据；
4. 等待新增 heartbeat 字段随 TTL 自然过期；
5. Postman 和 mock 工具可独立移除，不影响运行数据。

剩余风险：

- 尚未在用户真实 RTSP、TensorRT engine 和 GPU 上执行一键验收；
- Postman 集合已做结构、脚本和 mock 合同验证，但未对真实部署完整跑 collection
  runner；
- heartbeat 热指标依赖 Redis；Redis 故障时持久化数据仍在，但 readiness 和热
  指标会按设计失败/过期；
- 人工重放会重置自动 attempt 预算，旧 HTTP 状态、错误码和响应 hash 在下一次
  投递完成前保留，但当前 schema 没有独立 replay_count；
- 当前仍只支持一个 `VisionWorkerHost`；多 Worker 部署不在 P5 范围。

## 10. P6 入口门禁

P6 必须在目标硬件关闭以下门禁：

- 真实 RTSP 服务端确认每 Profile 只有一个连接；
- 真实 TensorRT/CUDA engine 初始化与固定 W Worker 数；
- 多摄像头 Pipeline、GPU 显存、CPU、内存和磁盘压力数据；
- 断流、重连、分辨率变化和 Worker 重启恢复；
- 使用真实后端 TLS/网关或受控 mock 执行
  `verify_algorithm_service_p5.ps1` 完整模式；
- Postman collection 在部署环境执行并保存脱敏结果；
- dead-letter 人工重放演练；
- 60 分钟长稳与最终发布/回滚清单。

只有这些真实证据全部记录后，才能声明整个算法服务完成。

## 11. GitHub 检查点

本节在 P5 分支和草稿 PR 发布后追加，作为版本回溯入口。
