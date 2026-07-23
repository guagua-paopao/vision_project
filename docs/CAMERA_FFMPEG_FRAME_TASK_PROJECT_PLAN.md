# 共享 RTSP FrameHub 与摄像头抽帧任务——项目范围与实施规划

> 文档状态：方案 3 已确认基线 v0.2  
> 编写日期：2026-07-20  
> 当前阶段：仅完成方案设计，尚未开始业务代码实现  
> 已选方案：同一 WorkerHost 内共享一次 RTSP 解码  
> 配套设计：[CAMERA_FFMPEG_FRAME_TASK_DESIGN.md](CAMERA_FFMPEG_FRAME_TASK_DESIGN.md)

## 1. 项目目标

在现有 Four-Stage People Flow 项目中加入“共享 RTSP FrameHub + 摄像头抽帧任务”能力，使同一个 `camera_profile` 在同一个 `four_stage_worker` 进程内只建立一条 RTSP 连接、只执行一次 FFmpeg 解码，并把不可变最新帧同时提供给：

1. People Flow 推理会话；
2. 一个或多个摄像头抽帧任务；
3. 后续可扩展的其他实时视觉消费者。

摄像头抽帧任务支持创建、查询、修改、删除、启动、停止、运行状态、运行历史和最新帧。People Flow 与抽帧任务各自拥有独立的业务生命周期、采样频率、输出和状态，但共享底层摄像头连接与解码结果。

## 2. 最终选定方案

```text
RTSP Camera
    ↓ one connection / one decode
SharedCameraFrameHub
    ├── PeopleFlowSubscription → TensorRT inference
    ├── ExtractionSubscription A → 1 frame/second
    ├── ExtractionSubscription B → 1 frame/5 seconds
    └── future consumers
```

本期保留现有可执行程序：

- `four_stage_server`：HTTP API、SQLite 查询与任务定义管理；
- `four_stage_worker`：升级为多角色 `VisionWorkerHost`，同时承载 People Flow 和 Camera Task；
- 不新增 `camera_frame_worker` 进程。

“共享一次”在第一版中的精确定义是：**同一 `four_stage_worker` 进程内，同一 `camera_profile` 最多一个活动 `RtspCaptureReader`**。第一版部署仍以单 Worker 进程为基线；多进程之间的全局摄像头归属与跨进程共享不在本期范围。

## 3. 已冻结的设计决策

| 编号 | 决策 |
|---|---|
| D-01 | 摄像头任务指持久化抽帧任务定义，不是 Camera Profile 本身。 |
| D-02 | Profile 继续来自 `config/cameras.yaml`，URI 继续只来自 Profile 指定的环境变量。 |
| D-03 | 不新增 Worker 进程；现有 `four_stage_worker` 重构为同进程多角色 WorkerHost。 |
| D-04 | 每个 `camera_profile` 在 WorkerHost 内对应一个共享 FrameHub 和最多一个 RTSP reader。 |
| D-05 | People Flow 和抽帧任务通过 RAII Subscription 订阅 Hub，各自维护独立 sequence 游标。 |
| D-06 | Hub 发布 `shared_ptr<const FrameEnvelope>`；消费者不得修改共享 `cv::Mat`。 |
| D-07 | Hub 只发布最新帧，不保存无界帧队列；慢消费者跳到最新 sequence。 |
| D-08 | 抽帧使用 OpenCV `CAP_FFMPEG`，不启动外部 `ffmpeg.exe`，不直接接入 libav*。 |
| D-09 | FFmpeg backend 必须可验证，Camera Task 不允许静默回退到其他 backend。 |
| D-10 | People Flow 保留现有模型、跟踪、事件和输出语义，只把私有 reader 替换为 Hub Subscription。 |
| D-11 | 同一任务最多一个活动 Run；多个任务可订阅同一 Hub，不创建额外 RTSP 连接。 |
| D-12 | 最后一个订阅者释放后，Hub 等待 idle grace，再关闭 RTSP，防止频繁启停抖动。 |
| D-13 | 任务定义和历史存 SQLite；命令、租约、热状态和 WorkerHost 心跳走 Redis。 |
| D-14 | CRUD 与运行控制分离；正在运行的任务不可修改或删除。 |
| D-15 | 删除采用软删除，运行历史继续可审计，帧文件按保留策略清理。 |
| D-16 | 第一版输出 JPEG，支持 `latest/archive/both`。 |
| D-17 | 第一版只支持手动启停，不做 cron、自动启动和 Qt 管理页。 |
| D-18 | 第一版生产配置限制 `worker_num=1`；配置多 Worker 时 Camera Task readiness 失败并提示未支持。 |

## 4. 当前代码基础与必须重构的缺口

### 4.1 可复用能力

- `CameraProfile`：YAML Profile、环境变量 URI、TCP/UDP 元数据；
- `RtspCaptureReader`：`CAP_FFMPEG`、独立采集线程、open/read timeout、最新帧覆盖、断线重连；
- People Flow：GPU 推理、跟踪、计数、安防分析、快照与状态；
- Redis Stream：消费组、pending reclaim、租约、停止标记和 Worker 心跳模式；
- SQLite：WAL、busy timeout、异步写入和查询模式；
- Crow、日志、配置、CMake 和 PowerShell 启停脚本。

### 4.2 当前限制

- `PeopleFlowInferenceWorker::loop()` 消费一个命令后同步执行整个长会话，不能同时消费 Camera Task 命令；
- People Flow 会话内部直接创建私有 `RtspCaptureReader`，其他消费者无法复用；
- `RtspCaptureReader::getLatestFrame()` 修改单一 `delivered_sequence_`，该语义只适用于单消费者；
- 当前 `dropped_frames` 依赖单一 delivered cursor，不能代表多订阅者的丢帧；
- `OPENCV_FFMPEG_CAPTURE_OPTIONS` 是进程级环境变量，多 Hub 并发 open/reconnect 需要串行协调；
- `worker.max_concurrency` 当前只是心跳描述字段，不是实际调度器；
- 没有抽帧任务定义、版本、运行历史、输出保留和 API；
- 没有 Hub 引用计数、空闲关闭、订阅者指标和共享连接可观测性。

## 5. 项目范围

### 5.1 第一版必须交付

#### A. Shared Camera FrameHub

- `SharedCameraFrameHubRegistry`：按 `camera_profile` 管理 Hub；
- 每 Profile 最多一个 reader/open/reconnect loop；
- 不可变 `FrameEnvelope` 原子发布；
- 每订阅者独立 sequence 与消费指标；
- RAII subscribe/unsubscribe；
- 第一个订阅者启动摄像头；
- 最后一个订阅者释放后延迟关闭；
- 共享 capture 状态、分辨率、FPS、重连次数和最近错误；
- 并发 open 的 FFmpeg 环境参数协调；
- 最大活动 Hub 数限制。

#### B. People Flow 迁移

- 把现有 `processTask()` 拆为命令接收和独立 `PeopleFlowSessionRunner`；
- SessionRunner 从 FrameHub Subscription 获取帧；
- 保持现有推理 FPS、warmup、重连处理、计数、事件、快照和 Redis 状态；
- 保持一个 People Flow 活动会话的现有约束；
- People Flow 停止时只释放自己的 Subscription；仍有抽帧订阅者时不关闭摄像头。

#### C. 同进程多角色 WorkerHost

- People Flow 命令消费者线程；
- Camera Task 命令消费者线程；
- People Flow Session Manager；
- Camera Task Manager，默认最多 4 个活动抽帧 Run；
- 共享 Hub Registry；
- 共享但有界的 JPEG writer pool；
- 统一 WorkerHost 心跳、优雅停止和 stale recovery；
- 保留 `four_stage_worker.exe` 名称和现有启动入口。

#### D. 摄像头抽帧任务管理

- 创建、列表、详情、乐观锁修改、软删除；
- 启用/禁用；
- 手动启动、停止；
- 当前状态与运行历史；
- 最新 JPEG；
- `latest/archive/both` 输出；
- 抽帧间隔、JPEG 质量、最大尺寸和保留策略；
- 多个任务共享同一 Profile/Hub。

#### E. 数据、API 与运维

- SQLite task/run/frame/schema 表；
- Redis Camera Task 命令流、Run 状态、租约和停止标记；
- WorkerHost 统一心跳与 Hub 热状态；
- 只读 Hub 诊断 API：列出 Hub、连接次数、订阅者类型和 capture 指标；
- Bearer Token；
- 历史帧保留清理；
- 配置、脚本、测试、API 和运维文档；
- People Flow 全量回归。

### 5.2 明确不在第一版

- Camera Profile 或 RTSP 密钥 CRUD；
- API 直接提交 URI、用户名或密码；
- 多 Worker 进程之间共享一条 RTSP 连接；
- 摄像头跨进程归属、Leader 选举和任务自动迁移；
- 跨机器共享内存、gRPC 帧总线、Redis 图像帧传输；
- Hub 历史帧 ring buffer；
- 保证每一源帧都被每个消费者处理；
- 定时计划、自动启动、日历规则；
- 视频录制、HLS/WebRTC、音频；
- PNG/BMP/原始视频包；
- Qt 或网页任务管理界面；
- 对抽帧结果自动触发新的推理流水线；
- 任务运行时热更新；
- 立即物理擦除已删除任务的 purge API。

### 5.3 后续演进

- 多进程 Camera Ownership Service；
- SharedFrame IPC/gRPC/共享内存；
- 多 Worker 自动调度与故障接管；
- Camera Profile + Secret Manager；
- cron 与自动恢复；
- 对象存储；
- Qt 管理页；
- Prometheus；
- libavformat/libavcodec 原生采集。

## 6. 关键行为约定

### 6.1 共享连接

- Hub key 为规范化后的 `camera_profile`，不是 URI；
- 第一个订阅者触发 URI 解析和 RTSP open；
- 后续订阅者立即复用相同 Hub；
- 停止任一消费者不影响其他消费者；
- 最后一个订阅者释放后进入 `idle_grace`，默认 5 秒；
- grace 期间出现新订阅者时取消关闭；
- grace 到期后停止 reader 并清除 URI 内存；
- Profile transport/config 变化只在无订阅者、下一次启动时生效。

### 6.2 最新帧语义

- Hub 发布单个不可变最新帧，不排队；
- 每个 Subscription 记录自己的 `last_seen_sequence`；
- 相同 sequence 不重复返回；
- 慢消费者只得到当前最新帧，中间跳过数量记入该订阅者指标；
- People Flow 和抽帧任务的“跳帧”分别统计，不能使用全局 dropped 值代替；
- Hub 的 overwritten 数量只表示新帧替换了旧发布帧，不代表错误。

### 6.3 独立业务频率

- 摄像头按源速率持续解码；
- People Flow 使用 `target_infer_fps`；
- 每个抽帧任务使用自己的 `frame_interval_ms`；
- 任一消费者变慢不会阻塞 capture thread 或其他消费者；
- 落后时不补处理、不突发保存历史帧。

## 7. 非功能要求

### 7.1 安全

- RTSP URI 只在 WorkerHost/reader 内存中短暂存在；
- API、SQLite、Redis、日志和 Git 中只出现 `camera_profile` 与必要的脱敏 URI；
- Camera Task API 全部要求 `YOLO11_CAMERA_TASK_ADMIN_TOKEN`；
- 输出路径完全由服务端构造；
- FFmpeg/OpenCV 异常对外使用固定清洗错误；
- 外部 `ffmpeg.exe` 不在范围内，避免 URI 暴露在进程命令行。

### 7.2 并发与隔离

- 默认最大活动 Hub：4；
- 默认最大活动抽帧 Run：4；
- 默认最大 People Flow Run：1；
- writer queue 有界，满时只丢本次待写帧；
- 共享帧只读；需要渲染或修改时消费者自行 clone；
- Camera Task 写盘异常不得使 People Flow 会话失败；
- People Flow 推理异常不得停止仍有抽帧订阅的 Hub。

### 7.3 性能

- 同 Profile 多消费者不增加 decode 次数；
- 共享发布不对每个消费者预先 clone 全帧；
- 消费者仅在需要处理时持有共享帧引用；
- 抽帧 resize/JPEG 不在 capture thread 执行；
- 无无界帧队列、任务队列或文件删除批次。

### 7.4 可观测性

- Hub：状态、backend、capture FPS、source FPS、open count、重连次数、分辨率、最新帧年龄、订阅者数量；
- Subscription：last seen sequence、consumed、skipped、last consume time；
- People Flow：保持现有状态指标，新增 hub/profile 信息；
- Camera Run：saved/dropped/skipped、writer depth、save FPS、last frame；
- WorkerHost：角色、活动 Hub、People Flow Run、Camera Run、writer queue 和心跳。

## 8. 实施阶段与工作分解

### M0：设计冻结与实现前检查（0.5 人日）

- 确认本文和详细设计；
- 冻结单 Worker 进程边界；
- 冻结共享最新帧而非 ring buffer；
- 冻结 API、schema 和默认配置。

退出条件：方案 3 的边界与验收无阻塞项。

### M1：共享帧数据模型与 Hub（2.0 人日）

- 新增不可变 `FrameEnvelope`；
- 改造 reader，移除全局 consumer cursor；
- 新增 `SharedCameraFrameHub`、Subscription 和 Registry；
- 新增引用计数、idle grace、最大 Hub 限制；
- 新增 `FfmpegOpenCoordinator`；
- 使用 Fake Frame Source + Fake Clock 完成并发单元测试。

退出条件：同 Profile 10 个订阅者只启动一次 source，各自 sequence 正确。

### M2：People Flow 迁移到 Hub（2.0 人日）

- 拆分当前阻塞式 `processTask()`；
- 新增 `PeopleFlowSessionRunner`；
- 从 Subscription 获取不可变帧；
- 在需要渲染时 clone，推理只读输入；
- 保持断线 warmup、状态、租约、事件和快照；
- 增加新旧实现等价回归。

退出条件：只有 People Flow 订阅时，行为和性能不低于现有基线。

### M3：VisionWorkerHost 多角色调度（2.0 人日）

- 保留 `four_stage_worker` executable；
- 新增 People Flow 和 Camera Task 两个命令消费者；
- 新增两个 Session Manager；
- 共享 Registry、writer pool 和统一 shutdown；
- 扩展心跳和 readiness；
- 防止任一长会话阻塞命令消费。

退出条件：People Flow 运行期间仍可创建、启动和停止抽帧 Run。

### M4：Camera Task Repository 与 Redis（1.5 人日）

- 新增 SQLite schema、CRUD、软删除、乐观锁；
- 新增 Run、Frame 元数据和活动 Run 唯一约束；
- 新增 Camera Task Redis 命令/状态/租约；
- 实现重复 START、PEL reclaim 和 stale Run 修复。

退出条件：重启 server/worker 后定义和终态可恢复，重复命令不重复订阅。

### M5：抽帧 Session、输出与清理（2.0 人日）

- 新增 `CameraFrameExtractionSession`；
- 实现单调时钟采样、独立 Subscription cursor；
- 实现 resize、一次 JPEG 编码、多目标写入；
- 实现 latest 原子替换、archive、元数据；
- 实现有界 writer queue 和 retention sweeper；
- 保证写盘故障与 People Flow 隔离。

退出条件：同 Hub 多种间隔任务输出正确，无额外 RTSP open。

### M6：HTTP API 与安全（1.5 人日）

- 实现任务 CRUD、start/stop/status/runs/latest-frame；
- 实现只读 `/api/v1/camera-hubs` 诊断接口，不提供 Hub 手工启停；
- 实现 Bearer Token、字段白名单、版本冲突和统一错误；
- server 合并 SQLite 定义与 Redis 热状态；
- health/ready 展示 WorkerHost 和 Hub 能力。

退出条件：API 契约测试全部通过。

### M7：集成、故障与交付（2.0 人日）

- 同 Profile 的 People Flow + 多抽帧任务端到端测试；
- 断流、恢复、分辨率变化、Redis/SQLite/磁盘故障注入；
- 现有 People Flow、安防、Repository、Qt 合约回归；
- 更新 CMake、脚本、配置、README、架构与运维文档；
- 形成连接数和资源验收记录。

退出条件：Definition of Done 全部满足。

### 预计投入

单人基线估算为 **12～15 人日**。相比独立 Worker 方案，新增投入主要来自：

- 现有 People Flow 阻塞循环拆分；
- 多消费者不可变帧模型；
- 同进程双命令消费者与 Session Manager；
- 共享连接生命周期和故障隔离；
- People Flow 等价性回归。

Qt UI、Camera Profile CRUD、多 Worker 跨进程共享不含在估算内。

## 9. 预期代码交付物

规划新增文件：

```text
include/business/camera_frame_types.h
include/business/camera_task_repository.h
include/business/camera_frame_extraction_session.h
include/business/frame_artifact_writer.h
include/server/shared_camera_frame_hub.h
include/server/shared_camera_frame_hub_registry.h
include/server/frame_subscription.h
include/server/ffmpeg_open_coordinator.h
include/server/people_flow_session_runner.h
include/server/camera_task_manager.h
include/server/camera_task_queue.h
include/server/camera_task_http_controller.h
include/server/vision_worker_host.h
src/business/camera_task_repository.cpp
src/business/camera_frame_extraction_session.cpp
src/business/frame_artifact_writer.cpp
src/server/shared_camera_frame_hub.cpp
src/server/shared_camera_frame_hub_registry.cpp
src/server/ffmpeg_open_coordinator.cpp
src/server/people_flow_session_runner.cpp
src/server/camera_task_manager.cpp
src/server/camera_task_queue.cpp
src/server/camera_task_http_controller.cpp
src/server/vision_worker_host.cpp
tests/shared_camera_frame_hub_test.cpp
tests/people_flow_hub_regression_test.cpp
tests/camera_task_repository_test.cpp
tests/camera_frame_extraction_test.cpp
tools/camera_task_api_contract_test.py
docs/CAMERA_FRAME_TASK_API.md
docs/CAMERA_FRAME_TASK_OPERATIONS.md
```

规划修改文件：

```text
CMakeLists.txt
include/business/rtsp_capture_reader.h
src/business/rtsp_capture_reader.cpp
include/server/people_flow_inference_worker.h
src/server/people_flow_inference_worker.cpp
include/server/app_config.h
src/server/app_config.cpp
src/server/main_people_flow_worker.cpp
src/server/main_server.cpp
config/server.yaml
config/worker.yaml
scripts/start_demo.ps1
scripts/stop_demo.ps1
scripts/test_all.ps1
docs/ARCHITECTURE.md
README.md
```

不新增：

```text
camera_frame_worker.exe
main_camera_frame_worker.cpp
start_camera_frame_worker.ps1
```

## 10. 测试策略

### 10.1 Hub 单元测试

- 首订阅启动一次 source；
- 相同 Profile 多订阅不重复 start；
- 不同 Profile 各自独立；
- 每订阅者 sequence 独立；
- 慢订阅者跳帧不影响快订阅者；
- 共享 FrameEnvelope 不可变；
- 最后订阅释放后 idle grace 关闭；
- grace 内重新订阅取消关闭；
- open/reconnect 串行设置 transport；
- Registry 达到最大 Hub 数时返回容量错误。

### 10.2 People Flow 回归

- 相同输入帧下推理、计数和事件语义保持；
- target infer FPS、warmup 和 reconnect 行为保持；
- 停止 People Flow 但保留 Camera Task 时 RTSP 不断开；
- 停止最后一个 Camera Task 后 Hub 正确关闭；
- 抽帧 writer 堵塞不降低 People Flow 消费线程响应性。

### 10.3 Camera Task 测试

- CRUD、软删除、版本冲突；
- start/stop/status 幂等；
- 多任务共享 Hub；
- 不同采样间隔；
- latest 原子性；
- archive/retention；
- writer queue 满时丢弃策略；
- WorkerHost crash 和 stale Run 修复；
- URI/Token 不泄漏。

### 10.4 端到端与故障注入

- 一路 RTSP + People Flow + 3 个抽帧任务；
- RTSP server 观测到一条 client connection；
- TCP/UDP、断流恢复、鉴权失败、分辨率变化；
- Redis 短时不可用；
- SQLite busy；
- 磁盘满/只读；
- WorkerHost 优雅停止与强制退出；
- 4 个 Profile/Hub 并发资源记录。

## 11. 验收标准

| 编号 | 验收条件 |
|---|---|
| AC-01 | 同一 Profile 同时运行 People Flow 和 3 个抽帧任务时，Hub `open_count=1`，RTSP 测试端只看到一条连接。 |
| AC-02 | 停止任一抽帧任务不会中断 People Flow 或其他抽帧任务。 |
| AC-03 | 停止 People Flow 后，只要仍有抽帧订阅，RTSP 连接保持。 |
| AC-04 | 最后订阅者释放后，Hub 在 idle grace 到期后关闭连接并清理 secret。 |
| AC-05 | 慢消费者跳帧时，其他消费者的 FPS 和 sequence 不受阻塞。 |
| AC-06 | People Flow 现有计数、安防事件、快照、状态和 Qt 契约回归全部通过。 |
| AC-07 | People Flow 活动期间，Camera Task CRUD、start 和 stop 可响应，不被长会话阻塞。 |
| AC-08 | 任务 CRUD 持久化，PATCH 版本冲突返回 409，活动任务不可更新或删除。 |
| AC-09 | 重复 start/START 命令不创建第二个 Run Subscription。 |
| AC-10 | 稳定 25 FPS 源、1000 ms 间隔运行 60 秒保存 58～62 帧，无补偿突发。 |
| AC-11 | latest 并发读取始终是完整 JPEG；archive 和保留清理不越过受管目录。 |
| AC-12 | Camera Task 写盘失败不会终止同 Hub 上的 People Flow。 |
| AC-13 | RTSP 断流时所有订阅者看到一致 Hub 状态，恢复后各自继续，无重复 reader。 |
| AC-14 | 非 FFmpeg backend 时 Hub 失败，不静默 fallback。 |
| AC-15 | URI、用户名、密码和 Token 不出现在 HTTP、SQLite、Redis 或普通日志。 |
| AC-16 | 配置 `worker_num>1` 且启用 Camera Task 时 readiness 明确失败，防止伪共享部署。 |
| AC-17 | 4 个不同 Profile、每个至少 2 个消费者并发，无死锁、无无界内存增长。 |

## 12. 主要风险与控制

| 风险 | 影响 | 控制 |
|---|---|---|
| People Flow 重构回归 | 计数或安防行为变化 | 先迁移到 Hub，再做任务功能；固定输入等价测试 |
| 共享 `cv::Mat` 被修改 | 消费者相互污染 | `shared_ptr<const FrameEnvelope>`；渲染前 clone |
| 多 Hub open 竞争全局 FFmpeg env | transport 串线 | `FfmpegOpenCoordinator` 串行 set+open |
| 抽帧写盘拖慢推理 | People Flow FPS 降低 | 独立有界 writer pool；满时丢抽帧作业 |
| Hub 被一个消费者错误停止 | 全部业务中断 | Subscription 引用计数；消费者无权直接 stop reader |
| 多 Worker 产生多连接 | 不满足一次解码 | 第一版强制 worker_num=1，后续做 camera ownership |
| 最后订阅频繁抖动 | RTSP 反复握手 | idle grace + 取消关闭 |
| Hub 失败影响所有订阅者 | 同摄像头全部暂停 | 明确共享故障域、统一 reconnect、消费者独立业务状态 |

## 13. 发布与回滚

### 发布

1. 备份现有配置与 SQLite；
2. 默认 `camera_tasks.enabled=false` 部署新 server/worker；
3. 只运行 People Flow，完成 Hub 迁移金丝雀与回归；
4. 开启 Camera Task API，创建一个 `latest` 任务；
5. 同时运行 People Flow，确认 Hub connection/open count 仍为 1；
6. 开启 archive 和其余任务；
7. 记录 CPU、内存、GPU、capture FPS、infer FPS、writer queue 和磁盘。

### 回滚

- 第一层：设置 `camera_tasks.enabled=false`，停止全部抽帧 Run，People Flow 继续使用 FrameHub；
- 第二层：回退到旧 `four_stage_worker` 二进制和旧配置；
- 新 Camera Task DB 和输出文件保留，不自动删除；
- 不执行 schema downgrade 或递归清理。

## 14. Definition of Done

- 方案 3 相关代码完成且不新增 Worker 进程；
- 同 Profile 单连接验收有可观测证据；
- People Flow 等价回归通过；
- CRUD、运行、输出、清理、安全和恢复测试通过；
- 现有全部测试通过；
- API、配置、状态机、运维和故障说明完整；
- 无硬编码 URI、Token、密码或用户可控输出路径；
- 文档与实际 API、schema、线程模型一致；
- 无开放 P0/P1 缺陷。
