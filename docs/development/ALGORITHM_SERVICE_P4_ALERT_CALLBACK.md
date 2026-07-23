# P4 持久化告警回调阶段记录

> 状态：完成
>
> 完成日期：2026-07-23（Asia/Shanghai）
>
> 退出门禁：PostgreSQL 17 全量回归 16/16 通过，0 失败、0 跳过

## 1. 阶段结果

P3 已经能够在同一 PostgreSQL 事务中写入告警事件和
`callback_outbox`。P4 在此基础上完成了独立 Worker 进程内的持久化 HTTP
投递闭环：

```text
算法告警事务
  -> callback_outbox(pending)
  -> 固定 CallbackDeliveryWorker
  -> 原子认领 + 带 fencing token 的租约
  -> 构造 alert_event.v1 JSON
  -> HMAC-SHA256 签名
  -> HTTP POST 外部后端
  -> delivered / retry / dead
```

回调投递和每摄像头抽帧线程、固定推理工作池相互独立。短时后端故障不会阻塞
抽帧或 GPU 推理；Worker 重启后仍会继续处理 PostgreSQL 中的待投递记录。

## 2. 配置和安全契约

Server 与 Worker 使用相同的 profile 白名单，但只有 Worker 从进程环境变量解析
真实 URL 和 HMAC 密钥。配置文件只保存环境变量名称：

```yaml
callbacks:
  enabled: false
  poll_interval_ms: 250
  request_timeout_ms: 5000
  lease_timeout_ms: 30000
  max_attempts: 8
  initial_backoff_ms: 1000
  max_backoff_ms: 300000
  request_body_limit_bytes: 1048576
  response_body_limit_bytes: 4096
  profiles:
    backend_primary:
      enabled: true
      url_env: YOLO11_CALLBACK_BACKEND_PRIMARY_URL
      hmac_secret_env: YOLO11_CALLBACK_BACKEND_PRIMARY_SECRET
      allow_insecure_http: false
```

关键约束：

- 任务中的 `callback_profile` 必须精确命中已启用的部署白名单，否则 HTTP
  控制面返回 400；
- 默认只允许 HTTPS，并使用系统证书验证；HTTP 只能通过 profile 显式允许；
- 禁止 URL 用户信息凭据，禁止自动跟随重定向；
- 密钥至少 16 字节，URL、密钥、请求正文均不写日志；
- 请求正文和响应留存均有大小上限；
- 启用回调但配置不完整时，readiness 为 false，Worker 在初始化模型前失败关闭。

## 3. HTTP 回调契约

投递正文为稳定的 `alert_event.v1` JSON，`event_id` 在重试期间不变。请求至少包含：

```http
Content-Type: application/json
Idempotency-Key: <event_id>
X-Event-Id: <event_id>
X-Timestamp: <epoch milliseconds>
X-Signature-Version: 1
X-Signature: <lowercase hex HMAC-SHA256>
```

签名输入严格为：

```text
X-Timestamp + "\n" + 完整原始 HTTP 请求正文
```

后端应以 `event_id` 或 `Idempotency-Key` 做幂等去重，并先对原始正文验签再解析
JSON。任意 2xx 视为成功；408、429、5xx 和传输错误进入重试；其他 4xx、
3xx 和异常状态进入 dead-letter。

## 4. 持久化、并发和故障恢复

1. 使用 `FOR UPDATE SKIP LOCKED` 原子认领到期的 `pending`/`retry` 记录；
2. 认领时状态变为 `delivering`，`attempt` 自增并作为 fencing token；
3. 只有持有当前 `attempt` 的投递者才能提交成功、重试或 dead 状态；
4. Worker 崩溃后，超过 `lease_timeout_ms` 的 `delivering` 记录可以被重新认领；
5. 指数退避为 `initial_backoff_ms * 2^(attempt-1)`，并受
   `max_backoff_ms` 限制；
6. 到达 `max_attempts` 后转入 `dead`，不再自动请求外部后端；
7. profile 被撤销、告警缺失、请求超限或安全配置错误均 fail closed；
8. 响应正文只留存配置上限内的前缀，同时计算完整响应流 SHA-256，便于审计又不
   无界占用内存。

该实现复用 P1 已冻结的 v2 表结构，没有新增数据库迁移。

## 5. 可观测快照

`CallbackDeliveryWorker` 提供进程内快照：

- `running`、`profiles_configured`；
- `claimed`、`delivered`、`retries`、`dead`；
- `transport_failures`、`lease_conflicts`；
- `last_success_at_ms`、`last_error_at_ms`、稳定错误码。

P5 将把这些数据与抽帧、推理池和算法 Processor 指标统一暴露到运维 HTTP/
Prometheus 接口。

## 6. 变更文件

核心运行时：

- `include/server/callback_delivery_worker.h`
- `src/server/callback_delivery_worker.cpp`
- `include/server/vision_worker_host.h`
- `src/server/vision_worker_host.cpp`
- `include/business/camera_task_repository.h`
- `include/business/camera_task_types.h`
- `src/business/camera_task_repository.cpp`

配置、HTTP 和构建：

- `include/server/app_config.h`
- `src/server/app_config.cpp`
- `config/server.yaml`
- `config/worker.yaml`
- `include/server/camera_task_http_controller.h`
- `src/server/camera_task_http_controller.cpp`
- `src/server/people_flow_http_server.cpp`
- `CMakeLists.txt`
- `scripts/build_backend.ps1`
- `scripts/test_postgresql_connection.ps1`

测试和文档：

- `tests/app_config_runtime_test.cpp`
- `tests/camera_task_http_contract_test.cpp`
- `tests/callback_delivery_worker_test.cpp`
- `tests/callback_http_transport_test.cpp`
- `docs/ARCHITECTURE.md`
- `docs/CAMERA_FRAME_TASK_API.md`
- `docs/CAMERA_FRAME_TASK_OPERATIONS.md`
- `README.md`
- `docs/development/README.md`
- `docs/development/ALGORITHM_SERVICE_IMPLEMENTATION_INDEX.md`
- `docs/development/ALGORITHM_SERVICE_P4_ALERT_CALLBACK.md`

## 7. 验证证据

编译和无数据库回归：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\build_backend.ps1
```

结果：后端增量编译成功；16 项中 9 passed、0 failed，7 项因未提供一次性测试
DSN 按设计 skipped，总耗时 1.31 秒。

最终数据库验收使用随机容器名、随机宿主端口和随机密码启动
`postgres:17-alpine`，设置 `YOLO11_TEST_POSTGRES_DSN` 与破坏性测试保护开关后
执行：

```powershell
D:\vs2019\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe `
  --test-dir .\out\build\backend-Release --output-on-failure
```

最终结果：

- 16/16 passed；
- 0 failed；
- 0 skipped；
- CTest 总耗时 9.38 秒；
- 包含 PostgreSQL 仓储、算法落库、回调 outbox、HTTP 控制面和真实 WinHTTP
  loopback 测试；
- 一次性 PostgreSQL 容器在 `finally` 中删除；
- 测试 DSN 和随机密码未写入文件。

新增确定性测试覆盖：

- 500 后退避重试，随后 204 成功；
- 400 立即 dead；
- 传输错误按 100/200 毫秒指数退避并在最大尝试次数后 dead；
- 租约过期重领、旧 attempt 完成被 fencing 拒绝；
- profile 撤销、超限请求和密钥缺失均不发起网络请求；
- HMAC 签名、幂等头和 `alert_event.v1` 正文独立校验；
- 真实 WinHTTP 请求禁止重定向、验证 HTTP 安全开关，并验证截断响应的完整流
  SHA-256。

## 8. 回滚与剩余风险

安全回滚步骤：

1. 将 Server/Worker 的 `callbacks.enabled` 设为 `false`；
2. 停止新的回调认领，等待当前请求超时或完成后停止 Worker；
3. 回退到 P3 Worker 二进制；
4. 保留 `security_alert_events` 和 `callback_outbox` 数据，不删除或重置
   pending/retry/dead 记录。

剩余风险：

- 本阶段用真实 WinHTTP loopback 验证 HTTP，但未连接用户实际后端的 TLS
  证书、网关和幂等存储；
- 多 Worker 的并发认领逻辑已有数据库级 fencing，但当前部署约束仍为一个
  `VisionWorkerHost`；
- dead-letter 的人工重放入口尚未加入管理 API；
- 回调指标尚未通过统一运维端点暴露；
- 真机 RTSP、TensorRT、GPU 饱和度和网络抖动仍属于 P6 硬件验收门禁。

## 9. P5 入口门禁

P5 应完成统一集成和可观测性：

- 抽帧 Pipeline、固定推理池、算法 Processor、callback worker 指标统一暴露；
- readiness 明确区分 PostgreSQL、Redis、模型池和回调配置；
- 提供 Postman 环境、CRUD/启停/查询/告警回调完整集合；
- 使用本地模拟后端验证签名、幂等和重试闭环；
- 补充 dead-letter 查询与受控重放运维方案；
- 形成从 HTTP 下发任务到后端收到告警的单命令集成验收。

## 10. GitHub 检查点

本节在 P4 分支和草稿 PR 发布后追加，作为不可歧义的版本回溯入口。
