# 逆向记录：崩铁 4.6.54（国际服测试服）Android 端

本文记录 HSRBox 移植过程中**实际验证过**的结论。每一条都有日志或数据支撑，不是推测。
后续接手的人不用再重走这些路。

样本：`com.HoYoverse.hkrpgoverseacbtest`，APK 490 MB，arm64-v8a。

---

## 一、库的分工

```
libunity.so      47 MB   .text 35 MB    Build ID e0f4489eec5b2a62    引擎
libil2cpp.so     460 MB  .text 51 MB    Build ID 8c76cb8b4f65ae2a    托管
```

注意 `libil2cpp.so` 里有个独立的 311 MB 段叫 `il2cpp`，托管代码放在那儿，不在 `.text`。

**引擎库通过 `dl_iterate_phdr` 找，优先 `libunity.so`。** 这一点很关键：原神是单库融合，崩铁是分开的。

---

## 二、五条走不通的路

### 1. il2cpp 导出 API —— 反调试桩

`libil2cpp.so` 的 `.dynsym` 里只有 **1 个函数导出**：`il2cpp_get_api_table`，其余 3002 个是 32 位十六进制假名的数据符号。

`il2cpp_get_api_table` 的 RVA 是 `0x6dcdc4c`，入口是一条 `B 0x6db1c14`。地址计算、入口字节都验证正确（`0x17ff8ff2`），但**一调用就 SIGSEGV**。

启动后 15 秒、45 秒、75 秒各试一次，全部崩溃。进程靠 SIGSEGV 保护活了下来，函数本身拒绝对外调用。

**结论：这条路彻底死了。**

### 2. 内存里找明文 metadata —— 不存在

`global-metadata.dat` 磁盘上 105,568,912 字节，头部 `4D 48 59 00`（`MHY\0`），**只有前 0x200 字节加密**，之后熵降到 2.0 像是明文。

用 root 从 `/proc/<pid>/mem` 完整导出进程内存，两轮共 **2.7 GB / 1148 个区域**，扫描：

```
AF 1B B1 FA（明文 magic）        0 命中
磁盘 metadata 正文片段作探针      0 命中
```

**结论：内存中不存在明文 metadata。** 存储层做了变换，熵低不等于明文。

### 3. APS2 压缩重定位 —— 格式解不开

`libil2cpp.so` 的 `.rela.dyn` 用的是 Android `APS2` 压缩格式（4,957,163 字节），不是标准的 24 字节 `Elf64_Rela` 数组。

试过两种 group 语义，解析都能走到流末尾（长度对），但 offset 从**第一项就不 8 字节对齐**：

```
mode A: good=2705   bad=751520   groups=64371
mode B: good=886    bad=753339   groups=64371
```

**结论：Android 的 APS2 group 编码语义与常见理解不符，靠猜解不出来。**

对照：`libunity.so` 用的是**标准** `.rela.dyn`（类型 1027 = `R_AARCH64_RELATIVE`，143,992 条），那个好解。

### 4. CPU 蒙皮函数 —— 一次都没被调用

`libunity.so` 导出了真符号，`dlsym` 能直接拿到：

```
_s_SkinVertices_NEON              0x909d60
_s_SkinVertices4Bones_NEON        0x90a1cc
_s_TransformVertices_Strided_XYZN_0_NEON
_MultiplyMatrices4x4_NEON         0x90ae50
```

反汇编确认 `_s_SkinVertices_NEON` 就是蒙皮循环（读骨骼索引 → 取 4x4 矩阵 → `fmul`/`fmla` 累加）。

6 个函数全部挂钩成功，跑 60 秒。分项计数：

```
_s_SkinVertices_NEON               = 0
_s_SkinVertices4Bones_NEON         = 0
_s_SkinVertices2Bones_NEON         = 0
_s_TransformVertices_Strided_*     = 0
_MultiplyMatrices4x4_NEON          = 458,993      ← 全在这里
```

**结论：GPU 蒙皮。** CPU 只算骨骼矩阵，顶点变换在 shader 里。蒙皮函数是死代码。

### 5. Vulkan 顶点捕获 —— 游戏不走 Vulkan

`libvulkan.so` 确实加载了（`graphics-api libEGL.so libGLESv2.so libGLESv3.so libvulkan.so`），`vkGetInstanceProcAddr` 也挂钩成功。

但跑 50 秒，`vk-buffers` 统计：

```
第 20 秒：buffer 0 个，其中 vertex 0 个
第 35 秒：buffer 0 个，其中 vertex 0 个
第 50 秒：buffer 0 个，其中 vertex 0 个
```

**一个 buffer 都没创建。** `libvulkan.so` 只是被别的 SDK 加载做能力检测，渲染主路径是 **GLESv3**。

---

## 三、走通的那条：GLES

### 挂钩

`glBufferData` 在 `libGLESv2.so` 里是**导出**的，`dlsym` 能定位，`DobbyHook` 能挂上：

```c
glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
```

`target == 0x8892`（`GL_ARRAY_BUFFER`）就是顶点缓冲，数据直接拷走。

### 结果

```
第 20 秒：顶点上传 133 次，索引 71 次，共 49,505 KB
累计 40 个缓冲
```

**49 MB 顶点数据，全部拿到。**

### 数据是真的

按 stride 12（纯位置）解析 `30_207600.bin`：

```
17,300 个顶点
异常 0/2000
包围盒  x[-2.04, 2.04]  y[-0.03, 6.00]  z[-0.00, 27.92]
相邻距离 中位 0.2447  90分位 2.29
```

坐标连续平滑，是正常网格。形状细长（x 窄、z 拉到 28）——**登录界面那列星穹列车的车厢**。

### 顶点格式

`glVertexAttribPointer` 抓到的声明（64 条里去重后的主流形态）：

```
[0] 3xFLOAT  @ 0x00    位置
[1] 4xUBYTE  @ 0x0c    法线（归一化压缩）
[2] 2xFLOAT  @ 0x10    UV
[3] 2xFLOAT  @ 0x18    UV2
stride = 32
```

**但要注意**：这个声明是在 `glBufferData` 时刻抓的，而游戏通常**先传数据、后声明布局**，所以抓到的可能是上一次绘制的旧配置。实际缓冲存在两种形态：

- **非交错纯位置**（stride 12，整块都是 float3）
- **交错完整格式**（stride 32）

判断方法：按 stride 12 解析后看包围盒是否合理、相邻距离是否平滑，而不是靠「法线模长接近 1」——后者对随机数据也有七成命中率，是伪信号。

---

## 四、工程里现有的工具

| 文件 | 作用 |
|---|---|
| `api_probe.cpp` | 符号解析、内存扫描、蒙皮 hook（前面几条死路的实现，保留作参考）|
| `gles_capture.cpp` | **GLES 顶点捕获，这条是活的** |
| `vk_hook.cpp` / `vk_capture.hpp` | Vulkan 钩子（已证明游戏不走，保留）|
| `tools/scan_game.py` | APK/so 体检，出 Build ID、导出符号、Unity 版本 |

控制开关都在 `control.json`：

```json
{
  "glesCapture": true,      // GLES 顶点捕获（默认开）
  "skinProbe": true,        // 蒙皮 hook 计数
  "vertexCapture": true,    // Vulkan 捕获（游戏不走，可关掉省事）
  "memScan": false,         // 内存扫描（会闪退，默认关）
  "apiProbe": false         // il2cpp API 探测（反调试桩，默认关）
}
```

---

## 五、下一步

链路已经通了：**GL 调用 → 二进制 → 可解析的几何**。

唯一缺的是**画面里没有角色**——抓到的全是列车、星球、小行星。

要做的事：

1. 下载完整游戏资源（几十 GB）
2. 进到有角色的界面（角色详情页最稳）
3. 用同样的工具抓一遍
4. 按 stride 12 解析，找到顶点数在 5000–50000 之间、包围盒紧凑的网格
5. 改一个顶点的坐标，看模型变不变形

第 5 步成了，就等于拿到了模型的写权限。
