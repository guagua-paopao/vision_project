# Four-Stage People Flow Qt Demo

这是从完整视觉项目中提取的可独立构建小型仓库，保留一条完整链路：

`Qt → HTTP Server → Redis Stream → RTSP Worker → CUDA/TensorRT Pose → 四阶段分析 → Redis/SQLite/快照 → Qt`

项目面向学习和完整流程体验，不以正式生产上线为目标。

## 保留范围

- 单个 `yolo11n-pose.engine`，同时提供人物框和 17 个 COCO 关键点；
- Redis Stream 命令队列、Worker 心跳和实时状态；
- TensorRT 10 + CUDA 加速推理；
- 人物追踪、过线计数、电子围栏、Pose 规则和时序 DEMO；
- 只服务于本项目的 People Flow HTTP API；
- 同一 Qt 窗口中的画面、指标、四阶段卡片和事件表；
- SQLite 过线事件、3 个确定性 C++ 测试和 Qt API 契约测试。

原项目的分类、分割、OBB、通用图片上传、通用视频/直播 Worker、离线数据集、
历史报告、测试视频、旧构建产物和其他 4 个 TensorRT 引擎均未复制到本仓库。

详细数据流见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

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
OpenCV 4.12、Redis、Qt 6.11 MinGW、vcpkg。

vcpkg 依赖声明位于 `vcpkg.json`：Crow、hiredis、nlohmann-json、spdlog、
sqlite3 和 yaml-cpp。OpenCV、CUDA、TensorRT 与 Qt 作为外部 SDK 安装。

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

先启动 Redis，然后运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\start_demo.ps1
```

脚本会隐藏输入 RTSP 地址，等待 Redis、Pose Worker 和 HTTP Server 就绪，
然后打开 Qt。Qt 中点击“检查服务”，再点击“启动会话”。

停止：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\stop_demo.ps1
```

RTSP 凭据只存放在进程环境变量 `YOLO11_CAMERA_ENTRY_URL`，不会进入 YAML、
Redis 消息、HTTP 请求、Qt 设置或 Git 提交。

## 测试

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\test_all.ps1
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
