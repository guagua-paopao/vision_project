# P2 CameraPipeline 阶段记录

> 状态：完成
>
> 完成日期：2026-07-23（Asia/Shanghai）
>
> 退出门禁：PostgreSQL 17 全量回归 12/12 通过，0 失败、0 跳过

## 1. 当前结果

现有 `CameraFrameExtractionSession` 已提升为正式的 `CameraPipeline`。Manager 以稳定
`camera_id` 为 key 持有 Pipeline 控制块，每个活动 camera_id 恰有一条 Pipeline 线程。
旧类名保留为类型别名，避免破坏 M0-M11 调用方。

已实现：

- `CameraPipeline` 统一拥有 Hub subscription、单调采样、JPEG 作业、运行状态和停止回收；
- Pipeline 不持有 `ModelRunner`，也不在取帧线程执行推理；
- 抽帧 `frame_interval_ms` 与算法 `target_infer_fps` 使用两套独立单调时钟；
- 新增非阻塞、有界语义的 `ICameraFrameJobSink::submitLatest` P3 接口；
- FrameJob 携带不可变共享帧、source sequence、camera/run/profile 和算法配置；
- 停止 Pipeline 时调用 `detachCamera`，为 P3 释放摄像头算法 Session；
- Manager 新增 Pipeline 注册表快照和活动线程计数；
- 替换 generation 时先 requestStop、join、移除旧控制块，再创建新线程；
- 迟到的旧 `run_id` STOP 只被确认，不会停止同 camera_id 的新 generation；
- Redis hot status 和 HTTP status 新增 `pipeline` 指标。

## 2. Pipeline 状态字段

`GET /api/v1/cameras/{camera_id}/status` 新增：

```json
{
  "pipeline": {
    "thread_running": true,
    "thread_started_at_ms": 1784770000000,
    "thread_age_ms": 12500,
    "sample_fps": 10.0,
    "sampled_frames": 125,
    "last_source_sequence": 53291,
    "skipped_frames": 187,
    "inference_submit_drops": 0
  }
}
```

这些字段由 Pipeline 写入 Redis 热状态；Redis 不可用时，HTTP 使用 PostgreSQL Run
字段降级，并继续标记 `runtime_stale=true`。

## 3. 线程与采样不变量

1. `pipelines_[camera_id]` 同时最多一个未完成控制块。
2. 相同 camera_id/run_id 的重复 START 不创建线程。
3. 相同 camera_id 的新 run_id 必须等待旧线程 join 后才能进入 factory。
4. STOP 必须匹配当前 run_id；旧 generation STOP 不影响新 generation。
5. Pipeline 从 Hub 读取最新 sequence，不补历史帧。
6. JPEG 与分析各有 cadence；同一时刻同时到期时只读取一次共享最新帧。
7. FrameJob 的 source sequence 单调；sink 必须非阻塞。
8. Pipeline 退出后先释放/排空 writer、发布终态、释放 lease，并 detach 推理 Session。
9. `sample_fps`/`sampled_frames` 仅统计 JPEG 抽帧；分析采样由 FrameJob 与 P3 worker 指标统计。

## 4. 变更文件

- `include/business/camera_pipeline.h`
- `include/business/camera_frame_extraction_session.h`
- `src/business/camera_pipeline.cpp`
- 删除构建引用：`src/business/camera_frame_extraction_session.cpp`
- `include/server/camera_task_api_control.h`
- `include/server/camera_task_manager.h`
- `src/server/camera_task_manager.cpp`
- `src/server/camera_task_queue.cpp`
- `src/server/camera_task_runtime.cpp`
- `src/server/camera_task_http_controller.cpp`
- `tests/camera_task_manager_test.cpp`
- `tests/camera_frame_extraction_test.cpp`
- `tests/camera_task_repository_test.cpp`
- `tests/camera_task_http_contract_test.cpp`
- `CMakeLists.txt`
- `docs/CAMERA_FRAME_TASK_API.md`
- `docs/development/ALGORITHM_SERVICE_IMPLEMENTATION_INDEX.md`
- `docs/development/ALGORITHM_SERVICE_P2_CAMERA_PIPELINE.md`

## 5. 已通过验证

后端增量/全量编译成功。

不依赖数据库的直接回归：

```powershell
out\build\backend-Release\camera_task_manager_test.exe
out\build\backend-Release\shared_camera_frame_hub_test.exe
out\build\backend-Release\people_flow_hub_regression_test.exe
```

结果：

- Camera Task Manager scheduling tests passed；
- Shared Camera FrameHub tests passed；
- People Flow Hub regression tests passed。

无测试 DSN 的 CTest：12 项、0 failed、7 passed、5 skipped。Manager 测试新增证明：

- N=2 活动 camera_id 对应 2 条 Pipeline；
- duplicate START 不新增 Pipeline；
- replacement 完成 stop/join 后仍保持每 camera 一条；
- stale STOP 不会误停 replacement；
- Manager shutdown 后活动 Pipeline 数为 0。

## 6. 退出门禁

`camera_frame_extraction_test` 已更新为直接构造 `CameraPipeline`，并新增以下数据库集成断言：

- 同 Profile 下 1 个 People Flow + 2 个 Pipeline 仍只有 1 个 Hub/open；
- Pipeline 热状态报告线程、启动时间、sample FPS 和 sample count；
- 10 FPS FrameJob 分析采样独立于 4 FPS JPEG 抽帧；
- FrameJob source sequence 严格递增；
- Pipeline 停止后 inference sink 已 detach；
- stale Run 标记失败后只允许一个 replacement generation。

用户明确授权强制重启 Docker Desktop 后，引擎恢复。第一次 PostgreSQL 17 全量回归
得到 11/12 通过，失败项为 `camera_frame_extraction_test`。根因是
`sampled_frames` 同时统计 JPEG 与分析 cadence，使开启 10 FPS 分析的 4 FPS 抽帧任务
计数高于纯 10 FPS 抽帧任务，与 HTTP 抽帧指标语义冲突。

修复后 `sample_fps`/`sampled_frames` 只统计 JPEG 抽帧；分析采样由 FrameJob 和 P3
worker 指标统计。重新编译后使用新的随机容器名、随机端口和随机密码启动
`postgres:17-alpine`，最终结果：

- 12/12 passed；
- 0 failed；
- 0 skipped；
- 总耗时 8.48 秒；
- 临时容器在 `finally` 中删除，测试 DSN 和随机密码未持久化。

P2 退出门禁已关闭，可以进入 P3。

## 7. 回滚

P2 未改变 PostgreSQL schema。安全回滚是恢复 P1 二进制，并保留现有 v2 数据库。
旧 include 路径仍有兼容 alias；若仅回退调用方，不需要数据迁移。
