# 项目性能、稳定性与准确率测试报告

测试日期：2026-07-27  
项目版本：`f6b93c6`（工作区另有未提交的摄像头配置改动）  
模型：`yolo11n-pose.engine`  
模型 SHA-256：`e911f38f1c0797453b09bb0d1b162aaa2b764dbb1db8a82c3a96d9fe4cd29e8f`

## 1. 结论

| 测试项 | 结论 | 说明 |
|---|---|---|
| 当前代码回归 | 通过，但集成项未全部执行 | 22 个 CTest：11 通过、0 失败、11 因缺少 PostgreSQL/Redis 测试环境而跳过；Qt、Web 契约测试通过 |
| 单张图像耗时 | 已实测 | 1920×1080 合成帧，模型输入 640×640；三轮短测平均约 3.08 ms/帧，P95 约 3.94 ms |
| 持续推理稳定性 | 通过 5 分 40 秒级验证 | 连续推理 100,000 帧，0 次异常，平均 3.398 ms，P95 4.236 ms |
| 封板整系统稳定性 | 通过 5 分钟级验证 | R8 记录为 57/57 有效采样、0 违规、0 推理失败；这不是 24/72 小时长稳结论 |
| 算法准确率 | 暂不能计算 | 仓库不包含原始图像/视频、人工标注或历史 Precision、Recall、mAP 数据 |

当前证据可以证明代码回归、GPU 引擎性能和 5 分钟级运行稳定性，不能据此宣称已经通过生产级长稳或算法准确率验收。

补充：同日已完成双摄像头真实链路 30 分钟测试。系统健康与稳定性通过，
但两路 10 FPS 吞吐目标未通过；详见
`docs/DUAL_CAMERA_FULL_CHAIN_PERFORMANCE_2026-07-27.md`。

## 2. 测试环境

- GPU：NVIDIA GeForce RTX 4080 Laptop GPU，显存 12,282 MiB；
- 驱动：581.80；
- CUDA：13.3；
- TensorRT：10.16；
- OpenCV：4.12；
- 推理方式：TensorRT，CPU 后处理；
- 测试输入：确定性生成的 1920×1080 BGR 合成帧，进入模型前缩放/预处理到 640×640。

合成帧没有产生人物检出，因此本次数据适合测量稳定的模型计算下界，不代表真实监控画面中多人场景的后处理耗时。

## 3. 当前代码回归

执行：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\test_all.ps1
```

结果：

- CTest 共发现 22 项；
- 11 项执行并通过；
- 11 项按 `SKIP_RETURN_CODE=77` 跳过，主要原因是当前进程未提供 PostgreSQL DSN/一次性集成基础设施；
- 0 项失败；
- Qt API 工作流契约通过；
- Camera Admin Web 契约、Token、CSP 和 DOM 检查通过。

因此，“当前可执行测试全部通过”成立，但不能表述为“包含数据库在内的 22 项全部实际执行通过”。

## 4. 单张图像耗时

每轮先预热 30 帧，再连续测量 500 帧：

| 轮次 | 平均耗时 | P50 | P95 | P99 | 最大值 | 折算吞吐 |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 3.145 ms | 3.110 ms | 4.067 ms | 5.123 ms | 14.224 ms | 317.926 FPS |
| 2 | 3.015 ms | 3.017 ms | 3.740 ms | 4.886 ms | 5.730 ms | 331.713 FPS |
| 3 | 3.069 ms | 3.059 ms | 4.011 ms | 5.221 ms | 9.774 ms | 325.875 FPS |
| 三轮均值 | 3.076 ms | 3.062 ms | 3.939 ms | 5.077 ms | — | 325.171 FPS |

三轮平均阶段耗时约为：

- GPU 预处理：0.301 ms；
- TensorRT enqueue：1.120 ms；
- D2H 与流同步：1.649 ms；
- CPU 后处理：约 0.001 ms；
- 端到端模型 Pipeline：3.075 ms。

`enqueue` 是异步提交，实际 GPU 等待的一部分会计入后续 D2H/流同步阶段。上述“端到端”仅覆盖 `detector.infer()`，不包含 RTSP 解码、业务跟踪、绘制、JPEG 编码、数据库和 HTTP 回调。

## 5. 持续推理稳定性

连续运行 100,000 帧、预热 50 帧：

| 指标 | 结果 |
|---|---:|
| 连续运行时长 | 约 339.8 秒（5 分 40 秒） |
| 成功处理帧数 | 100,000 |
| 异常输出 | 0 |
| 平均耗时 | 3.398 ms |
| P50 | 3.402 ms |
| P95 | 4.236 ms |
| P99 | 5.517 ms |
| 最大耗时 | 24.734 ms |
| 折算吞吐 | 294.297 FPS |

长跑平均耗时比三轮短测均值高约 10.5%，但全程完成且无崩溃或推理错误。该结果验证的是单模型持续推理，不等价于完整 Server/Worker/RTSP/PostgreSQL/Redis 链路长稳。

## 6. 封板 R8 数据复核

封板记录路径：`reports/r8/20260727T023805Z/summary.json`。

整系统 5 分钟观测结果：

- 57 个采样全部有效；
- 采样错误、Readiness 失败、Hub 不变量失败均为 0；
- 最大丢帧为 0，最大推理失败作业为 0；
- 4 条 Camera Pipeline 共用 1 个 Hub；
- 2 个推理 Worker，20 秒完成 700 个作业，聚合推理速度 35 FPS；
- 最大进程 CPU 8.34%；
- 最大进程工作集 464.06 MiB；
- 最大 GPU 显存 1,831 MiB；
- 最大 GPU 利用率 44%；
- 最大 GPU 温度 56°C；
- 断流重连后 sequence 恢复增长；
- Worker 重启后无孤儿 FFmpeg 进程，并恢复新 Run。

这些数据可作为本次测试的封板基线。因观测窗口只有 5 分钟，不能用于证明不存在小时级内存泄漏、句柄泄漏或磁盘累计问题。

## 7. 准确率评测缺口

当前仓库明确未保留原项目的离线数据集、测试视频和历史报告，现有测试主要验证几何映射、跟踪/计数规则和服务契约。它们能证明逻辑符合预期，但不能代替真实样本准确率。

补测时应按算法层分别统计：

| 算法层 | 建议指标 |
|---|---|
| 人体检测 | Precision、Recall、F1、mAP@0.5、mAP@0.5:0.95 |
| 姿态关键点 | OKS mAP 或 PCK |
| 人流计数 | IN/OUT 方向准确率、绝对误差、MAE、漏计率、重复计数率 |
| 电子围栏/动作告警 | 事件 Precision、Recall、F1、每小时误报数、检测延迟 P50/P95 |

至少需要提供：

1. 与实际部署摄像头视角一致的图片或视频；
2. 人框、关键点、过线方向和告警起止时间等人工真值；
3. 清晰的匹配规则，例如检测框 IoU 阈值、事件时间容差；
4. 白天/夜间、遮挡、拥挤、逆光和不同距离等场景标签。

## 8. 复现单帧基准

构建后执行：

```powershell
$env:Path = "D:\GPU13.3\bin;D:\TensorRT-10.16.1.11\lib;D:\libs\opencv\build\x64\vc16\bin;$env:Path"
.\out\build\backend-Release\pose_engine_benchmark.exe `
  .\engines\yolo11n-pose.engine - 500 30
```

使用真实图片时，将 `-` 替换为图片路径：

```powershell
.\out\build\backend-Release\pose_engine_benchmark.exe `
  .\engines\yolo11n-pose.engine D:\dataset\sample.jpg 500 30
```

若要形成生产级稳定性证据，应在真实 RTSP、PostgreSQL、Redis 和回调服务全部启用时执行现有长稳脚本至少 24 小时，并保留 JSONL 采样和 summary：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\soak_camera_frame_feature.ps1 `
  -CameraId <camera_id> -DurationMinutes 1440 -CaptureHostTelemetry
```
