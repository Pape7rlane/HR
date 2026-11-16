# HSRBox — 崩坏：星穹铁道 安卓模型替换

## 这一版

- 地址表来自 4.6.54 测试服（`libil2cpp.so` Build ID `8c76cb8b4f65ae2a`）的 dump.cs，
  60 项 Unity 托管方法 RVA 已就位。
- 缺项不再阻断模块：没有定位到的偏移返回 `ret` 空桩，调用立即返回，不会跳野地址。
  缺项清单写在 `status.json` 的 `detail` 里。
- 每帧驱动：优先游戏自己的 tick；未定位时退到 `Time.get_frameCount`，再退到 `getMesh`
  （`Runtime::tick` 自身按 100ms 节流，用高频方法触发是安全的）。

## 状态文件

模块在游戏的共享目录下写诊断文件：

```
/storage/emulated/0/Android/media/<游戏包名>/HSRBox/
    status.json      当前状态（ready / ready-no-tick / offsets-pending / buildid-mismatch）
    HSRBox Log.txt   运行日志
    gles-*.txt       GLES 顶点缓冲捕获
```

## 说明

仅支持 IL2CPP。只在自己拥有的设备与安装包上使用。
