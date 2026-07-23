# Four-Stage People Flow + Shared Camera FrameHub

这是从完整视觉项目中提取的可独立构建小型仓库，保留一条完整链路：

`Qt/API → HTTP Server → Redis Streams → four_stage_worker → 共享 RTSP FrameHub → People Flow + 无 GPU 抽帧`

项目面向学习和完整流程体验，不以正式生产上线为目标。

## 保留范围

- 单个 `yolo11n-pose.engine`，同时提供人物框和 17 个 COCO 关键点；
- Redis Stream 命令队列、Worker 心跳和实时状态；
- TensorRT 10 + CUDA 加速推理；
- 人物追踪、过线计数、电子围栏、Pose 规则和时序 DEMO；
- 同一 Profile 的 People Flow 和多个摄像头抽帧线程共享一次 FFmpeg 解码；
- 通过稳定 `camera_id` 完成摄像头 CRUD；新增/修改/删除分别启动、替换、关闭抽帧线程；
- 内部抽帧 Run、latest/archive/both、保留清理与只读 Hub 诊断，不暴露抽帧任务 CRUD；
- `/camera-admin` Web 管理端、只读 Camera Profile 与共享 Hub 可视化；
- 运维 JSON/Prometheus 指标、长稳证据脚本、全局存储水位与配额；
- PostgreSQL 持久化、pg_dump/pg_restore 与 SHA-256 清单的离线备份/恢复；
- 只服务于本项目的 People Flow HTTP API；
- 同一 Qt 窗口中的画面、指标、四阶段卡片和事件表；
- PostgreSQL 人流/摄像头 Run 数据、确定性 C++ 测试和 Qt API 契约测试。

原项目的分类、分割、OBB、通用图片上传、通用视频/直播 Worker、离线数据集、
历史报告、测试视频、旧构建产物和其他 4 个 TensorRT 引擎均未复制到本仓库。

详细数据流见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

Camera 抽帧功能文档：

- [API 契约](docs/CAMERA_FRAME_TASK_API.md)
- [部署、监控、故障与回滚](docs/CAMERA_FRAME_TASK_OPERATIONS.md)
- [验收记录](docs/CAMERA_FRAME_TASK_ACCEPTANCE.md)
- [项目设计](docs/CAMERA_FFMPEG_FRAME_TASK_DESIGN.md)
- [M0–M11 开发日志](docs/development/README.md)
- [M11 Camera ID + PostgreSQL 设计](docs/CAMERA_INSTANCE_POSTGRESQL_DESIGN.md)

## 目录

```text
api/          Pose TensorRT C++ API
config/       Server、Worker 与摄像头注册配置
engines/      唯一保留的 Pose TensorRT 引擎和 SHA256
include/      运行时、业务和服务端头文件
plugin/       YOLO TensorRT 自定义插件
qt_client/    Qt Widgets 客户端
scripts/      构建、测试、启动和停止脚本
src/          Pose 运行时、四阶段业务、Redis Worker 和专用 Server
tests/        业务、存储和 GPU 引擎测试
tools/        Qt/API Mock 契约测试
```

## 环境要求

当前已验证环境：Windows x64、MSVC、Ninja、CUDA 13.3、TensorRT 10.16、
OpenCV 4.12、Redis、PostgreSQL 17、Qt 6.11 MinGW、vcpkg。

vcpkg 依赖声明位于 `vcpkg.json`：Crow、hiredis、libpq、nlohmann-json、spdlog
和 yaml-cpp。OpenCV、CUDA、TensorRT 与 Qt 作为外部 SDK 安装。

> TensorRT `.engine` 与 TensorRT/CUDA/GPU 架构相关。当前文件 SHA256 为
> `e911f38f1c0797453b09bb0d1b162aaa2b764dbb1db8a82c3a96d9fe4cd29e8f`。
> 在不同 GPU 或 TensorRT 版本上，建议重新导出引擎。

## 构建

```powershell
cd D:\path\to\four_stage_qt_demo

powershell -ExecutionPolicy Bypass -File .\scripts\build_backend.ps1 -CleanFirst `
  -InstallDependencies `
  -VcpkgRoot D:\vcpkg `
  -CudaRoot D:\GPU13.3 `
  -TensorRtRoot D:\TensorRT-10.16.1.11 `
  -OpenCvDir D:\libs\opencv\build\x64\vc16\lib

powershell -ExecutionPolicy Bypass -File .\scripts\build_qt.ps1 `
  -QtRoot D:\qt\6.11.1\mingw_64 -CleanFirst
```

## 启动

先启动 Redis 和 PostgreSQL，然后运行。PostgreSQL DSN、RTSP 和管理员 Token
均由脚本隐藏输入，也可以预先设置进程环境变量：

```powershell
$env:YOLO11_POSTGRES_DSN = "host=127.0.0.1 port=5432 dbname=vision_project user=vision_app password=你的密码 connect_timeout=5"
```

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\start_demo.ps1
```

脚本会隐藏输入 RTSP 地址、管理员 Token 和 PostgreSQL DSN，等待 Redis、Pose Worker 和 HTTP Server 就绪，
然后打开 Qt。Qt 中点击“检查服务”，再点击“启动会话”。

摄像头管理页面：<http://127.0.0.1:8087/camera-admin>。管理员 Token 只保存在
当前页面内存中；Profile 来自部署配置并且在运行时只读。

停止：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\stop_demo.ps1
```

RTSP 凭据只存放在进程环境变量 `YOLO11_CAMERA_ENTRY_URL`，不会进入 YAML、
Redis 消息、HTTP 请求、Qt 设置或 Git 提交。

## 摄像头抽帧线程

当前演示配置已在 Server/Worker 两侧启用内部抽帧 Run。部署时必须保持两侧
一致，并在进程环境中设置
`YOLO11_CAMERA_TASK_ADMIN_TOKEN`。第一版必须保持 `worker.worker_num=1`。

抽帧对象线程运行在现有 `four_stage_worker` 内，不新增 `camera_frame_worker`，
也不调用 TensorRT/CUDA 推理。同 Profile 的 People Flow 和抽帧任务通过
进程内 FrameHub 共享唯一 reader；断流是摄像头级共享故障域，写盘/编码是
摄像头抽帧对象级独立故障域。外部只使用 `/api/v1/cameras` 和 `camera_id`；
`run_id` 仅用于内部执行审计。

## 测试

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\test_all.ps1
```

Camera Frame 发布门禁（重新构建、全部 CTest、Qt 合约和静态架构/安全检查）：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\verify_camera_frame_feature.ps1
```

长稳、备份和恢复：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\soak_camera_frame_feature.ps1 `
  -CameraId entrance_extract_01 -DurationMinutes 60
powershell -ExecutionPolicy Bypass -File .\scripts\test_postgresql_connection.ps1 `
  -ConfirmDisposableDatabase
powershell -ExecutionPolicy Bypass -File .\scripts\backup_runtime.ps1 -Label manual
powershell -ExecutionPolicy Bypass -File .\scripts\restore_runtime.ps1 `
  -BackupPath .\runtime\backups\vision_runtime_<stamp>.zip -ConfirmRestore
```

GPU 引擎独立冒烟测试：

```powershell
$env:Path="D:\GPU13.3\bin;D:\TensorRT-10.16.1.11\lib;D:\libs\opencv\build\x64\vc16\bin;$env:Path"
.\out\build\backend-Release\pose_engine_smoke.exe .\engines\yolo11n-pose.engine
```

## GitHub 上传前

`.gitignore` 已排除构建产物、运行数据、SQLite、日志和秘密文件。建议先确认：

```powershell
git init
git add .
git status
git commit -m "Initial compact four-stage Qt demo"
```
