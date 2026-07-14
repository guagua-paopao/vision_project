# Qt client

该目录是纯 Qt Widgets/Network 客户端，不链接 CUDA、TensorRT、OpenCV、
Redis 或 SQLite，也不会接收和保存 RTSP URI。

构建：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_qt.ps1 `
  -QtRoot D:\qt\6.11.1\mingw_64 -CleanFirst
```

连接真实后端由根目录脚本统一完成：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\start_demo.ps1
```

客户端每秒轮询会话状态、实时指标、JPEG 快照和四阶段安全状态，每 5 秒
查询一次过线事件。它只依赖 README 中列出的 9 个 People Flow API。
