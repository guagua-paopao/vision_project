# P1 数据模型与 HTTP 控制面阶段记录

> 状态：完成  
> 完成日期：2026-07-23（Asia/Shanghai）  
> 上一阶段：[P0 契约冻结](ALGORITHM_SERVICE_P0_CONTRACT.md)  
> 下一阶段：P2 单摄像头 `CameraPipeline` 生命周期

## 1. 阶段结果

P1 已把 P0 冻结的任务与告警契约落到现有独立 HTTP 服务中。外部后端现在可以围绕
`/api/v1/cameras` 创建、查询、修改、删除、启动和停止任务；任务定义能够持久保存抽帧、
算法与回调配置，并通过 Redis 命令传到 Worker。P1 只传递算法配置，不在本阶段执行推理。

本阶段同时完成：

- `desired_state` 与兼容字段 `enabled` 的双向一致性；
- `analysis.enabled/target_infer_fps/algorithm_profile/algorithms` 持久化和参数校验；
- 仅允许部署侧解析的 `camera_profile`、`callback_profile`，递归拒绝请求中的 URI/凭据字段；
- create/start/stop 的 `Idempotency-Key`、24 小时持久化结果和 SHA-256 请求摘要；
- PostgreSQL v2 迁移、告警事实表、回调 outbox 表、幂等键表；
- `GET /api/v1/cameras/{camera_id}/alerts` 与告警总量指标；
- Camera API 与兼容 People Flow start/stop 共用
  `YOLO11_CAMERA_TASK_ADMIN_TOKEN`；
- Qt 客户端从环境变量读取同一令牌，仅写入 `Authorization` 请求头；
- 修复完整 PostgreSQL 回归发现的既有 People Flow SQL 歧义问题。

## 2. 已实现的外部行为

### 2.1 任务控制

主资源仍为 `camera_id`，内部每次执行生成新的 `run_id`。新增/扩展字段如下：

```json
{
  "desired_state": "running",
  "analysis": {
    "enabled": true,
    "target_infer_fps": 5.0,
    "algorithm_profile": "security_default",
    "algorithms": ["people_flow", "ppe_detection"]
  },
  "callback_profile": "backend_primary"
}
```

`desired_state=running` 对应 `enabled=true`；`stopped` 对应 `false`。同时提交冲突值会返回
`400 INVALID_TASK_CONFIG`。开启任务时，上述安全配置随 Redis START 命令传给 Worker，
不包含 RTSP URI、回调 URL 或凭据。

### 2.2 幂等和并发控制

- `POST /cameras`、`POST /cameras/{id}/start`、`POST /cameras/{id}/stop`
  接受 `Idempotency-Key`；
- 同一 scope、key、规范化请求摘要会重放已保存的状态码和 JSON，并返回
  `X-Idempotent-Replay: true`；
- 同一 key 对应不同摘要返回 `409 IDEMPOTENCY_CONFLICT`；
- 重放时重新读取当前 Camera 版本并返回 ETag；
- PATCH/DELETE 继续要求 ETag/If-Match；
- 单进程生命周期互斥和数据库活动 Run 唯一索引共同阻止重复线程代际。

### 2.3 告警查询

`GET /api/v1/cameras/{camera_id}/alerts` 支持：

- `event_type`；
- `minimum_severity=1..5`；
- `limit=1..200`；
- `offset`。

响应包含 v1 标准告警事件、算法来源、证据、业务 payload、创建时间和回调投递状态。
P1 仅提供表结构、仓储与查询；告警生产和 HTTP 回调执行分别在 P3、P4 实现。

## 3. PostgreSQL v2

显式迁移文件为 `db/postgresql/002_algorithm_service_contract.sql`。运行时初始化也使用相同
的幂等 DDL，支持已有 v1 数据库原位升级。

| 对象 | 用途 |
|---|---|
| `camera_tasks` 新列 | desired state、分析开关/FPS/profile/算法列表、callback profile |
| `security_alert_events` | 去重后的不可变告警事实，`UNIQUE(task_id,fingerprint)` |
| `callback_outbox` | P4 使用的至少一次投递状态 |
| `camera_idempotency_keys` | 控制面幂等响应，按 scope/key 唯一并带过期时间 |
| `camera_schema_version=2` | 当前 Camera 数据模型版本 |

迁移不存储原始 RTSP URL、回调 URL、Bearer token、数据库密码或 Redis 密码。

## 4. 统一鉴权边界

- Camera API 所有路由继续要求 Bearer token；
- 兼容 People Flow 的 start/stop 写操作开始要求同一 Bearer token；
- People Flow 健康检查与现有只读兼容路由在 P1 不改变；
- 服务端和 Qt 均只引用环境变量名，不把 token 写入 YAML、URL、响应或日志；
- token 比较使用常量时间比较。

## 5. PostgreSQL 回归中发现并修复的问题

第一次真实数据库运行发现两个独立问题：

1. 新增 HTTP metrics 测试读取了错误 JSON 层级；已改为 `alerts.total`。
2. 既有 People Flow PostgreSQL SQL 使用了未限定的 upsert 列名以及
   `? + ?` 未知参数类型。PostgreSQL 17 分别报告列歧义和操作符无法唯一确定。

修复为：

- `pf_aggregates_minute.in_count/out_count` 显式限定目标表；
- 相加的预编译参数显式 `CAST(... AS BIGINT)`；
- 测试失败信息附带 repository 的实际 `last_error`，便于后续定位。

## 6. 变更文件

### 构建、配置和迁移

- `CMakeLists.txt`
- `config/server.yaml`
- `config/worker.yaml`
- `db/postgresql/002_algorithm_service_contract.sql`

### 服务与领域代码

- `include/business/camera_task_types.h`
- `include/business/camera_task_repository.h`
- `include/server/app_config.h`
- `include/server/camera_task_manager.h`
- `include/server/camera_task_http_controller.h`
- `include/server/people_flow_http_server.h`
- `src/business/camera_task_repository.cpp`
- `src/business/people_flow_repository.cpp`
- `src/server/app_config.cpp`
- `src/server/camera_task_queue.cpp`
- `src/server/camera_task_http_controller.cpp`
- `src/server/people_flow_http_server.cpp`

### 客户端、测试和契约

- `qt_client/src/people_flow_api_client.h`
- `qt_client/src/people_flow_api_client.cpp`
- `tests/app_config_runtime_test.cpp`
- `tests/camera_task_repository_test.cpp`
- `tests/camera_task_http_contract_test.cpp`
- `tests/phase22_repository_test.cpp`
- `tools/qt_demo_contract_test.py`
- `api/schemas/algorithm_task.v1.schema.json`
- `api/schemas/alert_event.v1.schema.json`
- `docs/CAMERA_FRAME_TASK_API.md`
- `docs/development/ALGORITHM_SERVICE_IMPLEMENTATION_INDEX.md`
- `docs/development/README.md`
- `docs/development/ALGORITHM_SERVICE_P1_CONTROL_PLANE.md`

## 7. 验证证据

### 7.1 后端全量编译

```powershell
cmd.exe /d /s /c `
  '""D:\vs2019\VC\Auxiliary\Build\vcvars64.bat" >nul && "D:\vs2019\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "out\build\backend-Release" --config Release"'
```

结果：成功。`four_stage_server`、`four_stage_worker` 和全部测试目标链接完成。

### 7.2 JSON Schema

使用 Python `jsonschema` 的 Draft 2020-12 validator 检查两个 schema 自身，并验证一份
启用分析的任务样例和一份带投递状态的告警样例。

结果：`PASS: JSON Schema 2020-12 contracts and representative payloads are valid`。

### 7.3 Qt 契约与真实编译

```powershell
python tools\qt_demo_contract_test.py
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\build_qt.ps1 -QtRoot D:\qt\6.11.1\mingw_64
```

结果：

- Qt API workflow/endpoint/token-header 静态契约通过；
- `people_flow_qt_client.exe` Release 编译与部署通过；
- Qt 工具输出一次 license client lock 警告，但构建返回成功且产物已生成。

### 7.4 完整 PostgreSQL 17 回归

测试使用随机容器名、随机密码、随机本机端口的 `postgres:17-alpine` 临时容器。
仅在测试进程内设置：

```powershell
$env:YOLO11_TEST_POSTGRES_DSN = "<disposable PostgreSQL DSN>"
$env:YOLO11_ALLOW_DESTRUCTIVE_POSTGRES_TESTS = "1"
& 'D:\vs2019\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe' `
  --test-dir 'out\build\backend-Release' --output-on-failure
```

最终结果：12/12 passed，0 failed，0 skipped，总耗时 8.44 秒。临时容器在
`finally` 中停止并因 `--rm` 自动删除，测试 DSN 和密码随进程清除。

覆盖的数据库相关目标：

- `repository_test`；
- `camera_task_repository_test`；
- `camera_frame_extraction_test`；
- `camera_storage_policy_test`；
- `camera_task_http_contract_test`。

## 8. 已知限制和后续风险

- P1 只把分析配置传到 Worker；尚未创建每摄像头算法执行路径。
- `callback_outbox` 尚无投递器，P4 前不会向外部后端发送告警。
- 幂等结果在业务操作成功后写入；进程在两者之间崩溃时存在一个很小的恢复窗口。
  当前部署约束为单 HTTP 服务实例，P5 需加入故障注入并决定是否引入“处理中”占位记录。
- 幂等记录过期清理由后续运维任务完成；过期记录当前只是不参与查询。
- 本工作区仍无可用 Git 元数据，无法记录 commit hash；本阶段文件表、测试证据和日期是
  当前回溯链。

## 9. 数据安全回滚

安全回滚方式是回退服务二进制和配置，但保留 PostgreSQL v2 表/列。旧代码会忽略新增
对象，保留它们可以避免告警、outbox 和幂等审计数据丢失。

不建议自动执行 down migration。只有在已验证备份且确认不再需要 v2 数据时，才人工
删除新增对象。由于当前工作区没有 Git 历史，代码回滚必须依赖工作区/发布包备份，不能
使用破坏性的 `git reset`。

## 10. P2 准入

P2 可以开始，原因如下：

- v1 任务/告警 schema 与 HTTP 行为一致；
- 新字段已贯通 HTTP、PostgreSQL、Redis command；
- 完整 PostgreSQL 17 回归 12/12 通过；
- 统一控制鉴权、幂等和 ETag 边界已建立；
- P2 可以只关注一个 Camera 对应一个 `CameraPipeline` 线程及其启停/重连生命周期。
