# HSRBox

把 PC 的崩铁模型 MOD（SRMI / 3DMigoto 那一套 ini + buf + dds）加载到 **Android 版《崩坏：星穹铁道》** 上的 LSPosed 模块。

本项目是 [GIBox](https://github.com/Yuanqi314/GIBox)（原神 Android Mod 加载器，GPL-3.0）的崩铁移植：模型转换器、3DMigoto INI 引擎、骨骼匹配、DDS 解码、LSPosed 注入、界面全部沿用，改的是**目标游戏**那一层——包名、引擎库定位、地址表、实体发现、角色表。

```
GIBox 的东西（原样可用）        HSRBox 改的东西
─────────────────────────      ─────────────────────────
migoto/  3DMigoto INI 引擎  →  不动
model.hpp  HSRBox 模型格式  →  不动
ModelConverter.java 转换器  →  不动（GIMI 与 SRMI 的 buf/ini 布局同源）
dds.cpp / 纹理管线          →  不动
vk_capture.hpp 诊断         →  不动
LSPosed 注入 / Compose 界面 →  不动
─────────────────────────      ─────────────────────────
offsets.hpp 原神 RVA 表     →  崩铁地址表（**空表起步**，按版本生成）
bootstrap.cpp 只认            →  按优先级认 libil2cpp.so / libunity.so /
  libyuanshen.so                  libhkrpg.so / libGame.so
角色表 8 位 AvatarID         →  4 位 AvatarID
```

## 现在能跑到哪一步

| | 状态 |
|---|---|
| 编译出 APK、LSPosed 里勾选生效 | ✅ 可以 |
| 导入 MOD、转换、骨骼匹配、贴图处理 | ✅ 可以（转换器与引擎跟原神那套同源） |
| 进游戏替换模型 | ⚠️ **要先生成这一版游戏的地址表**，见下 |
| 什么都没配就装上 | ✅ 不会崩：状态写 `offsets-pending`，日志里列清楚缺哪些项 |

默认是空表。空表时 `bootstrap` 在挂钩之前就返回，只写状态和一条 `offsets-pending` 日志——这是故意的，宁可什么都不做，也不要拿别人版本的 RVA 去 `DobbyHook` 一个 4 字节的地方，那样是必崩。

## 已知版本：国际服 4.6.54

实测（`com.HoYoverse.hkrpgoversea`，单 APK 490 MB，arm64-v8a 共 29 个 so）：

| | |
|---|---|
| 引擎库 | `libunity.so` 47 MB，`.text` 35 MB，**Build ID `e0f4489eec5b2a62`** |
| 托管库 | `libil2cpp.so` 460 MB，`.text` 363 MB，Build ID `8c76cb8b4f65ae2a` |
| metadata | `global-metadata.dat` 105 MB，头部 `4D 48 59 00`（`MHY\0`），**加密** |
| il2cpp 导出 | 只有一个函数 `il2cpp_get_api_table`；另外 3002 个数据符号是 32 位十六进制假名 |
| 反作弊 | `libanogs.so` `libanort.so` `libAstrolabe.so` `libastrolabe-crypto.so` `libAPMDump.so` `libMHYComboCrypto.so` |

结论：

1. **两套基址（已实现）**。引擎（`Mesh` / `Texture` / `Material` / `SkinnedMeshRenderer` / LOD 的原生入口）在
   `libunity.so`，托管侧的 `objectNew` / `arrayNew` / `typeObject` / `newString` / `pin` 在 `libil2cpp.so`。
   原神是 `libyuanshen.so` 单库融合，GIBox 只有一个 `Engine.base`；这里拆成了
   `Engine{base, managedBase}` + `abi::libOf(Offset)`：每个偏移查表前先定归属，`Engine::addr()` 按归属选基址。
   地址表按 **`libunity.so` 的 Build ID** 走，`Profile` 里同时记 `libil2cpp.so` 的 Build ID；两者对不上时
   只启用引擎侧，托管项落 `notFound`，不会拿错基址去调用。
2. **托管调用那条路走不通**。`libil2cpp.so` 只导出 `il2cpp_get_api_table`，`il2cpp_class_from_name`
   这些一个都没有，所以拿不到 il2cpp API；只能老老实实铺原生 RVA 表。（`il2cpp_get_api_table` 本身是
   混淆过的桩，`0x6dcdc4c → 0x6db1c14`，不是干净的 `adrp/add/ret`。）
3. **metadata 要先解密**。`MHY\0` 头 + 整份密文；`Il2CppDumper` 直接喂会报格式错。
   锚点：`libil2cpp.so` 里 `global-metadata.dat` 这个字符串在 `0x69b733`，`Assembly-CSharp` 在 `0x4fdbbe`。

## 先用模块跑一次 API 表探测

地址表还空着的时候，模块也能装、也能启动——它会在待定状态停住，不挂钩。顺手做一次探测：

装好 APK，LSPosed 里作用域勾上 `com.HoYoverse.hkrpgoversea`，启动游戏。`Loader` 会把 `libhsrbox.so`
解到游戏私有目录并加载，然后在引擎库出现 3 秒后调一次 `il2cpp_get_api_table()`
（`dlsym` 拿到的，不用理解它那堆混淆），把结果写到：

```
/storage/emulated/0/Android/media/com.HoYoverse.hkrpgoversea/HSRBox/api-table.txt
```

内容长这样：

```
libil2cpp.so base=0x... build_id=8c76cb8b4f65ae2a
libunity.so  base=0x... build_id=e0f4489eec5b2a62

il2cpp_get_api_table @ 0x... (libil2cpp.so+0x6dcdc4c/text)
fd7bbea9 ...

table = 0x... (libil2cpp.so+0x.../rodata)

idx  指针                落点                         开头指令
0    0x...              libil2cpp.so+0x.../text      aa0003f0 d65f03c0
1    0x...              libil2cpp.so+0x.../text      ...
```

看两件事：

- **`table` 落在哪**。在 `rodata`/`data` 且可读 → 真是一张表；返回 `(nil)` 或落在不可读段 → 这条路断了。
- **`text` 段里的指针有多少**。全是函数指针，就能按 Unity 的 `Il2CppAPI` 顺序去对 `il2cpp_runtime_invoke`。

`control.json` 里 `"apiProbe": false` 可以关掉这次探测（默认开，只在启动时跑一次）。

## 0. 先拿到对应版本的包

适配只需要 **APK 文件**，不用装、不用账号、不用进游戏——地址表是从 so 里静态读出来的。但版本必须**精确对上**：
表的 key 是 so 的 GNU Build ID，换一个构建号就对不上。

国际服包名 `com.HoYoverse.hkrpgoversea`。Play 商店现在发的是 split APK（AAB），
`libil2cpp.so` **不在 `base.apk` 里**，在 `split_config.arm64_v8a.apk` 里：

```
base.apk                              清单、assets、部分资源
split_config.arm64_v8a.apk            lib/arm64-v8a/*.so   ← 要的就是这个
split_config.xxhdpi.apk 等            分辨率资源
```

三种拿法：

```bash
# A. 手机上已经装了：拿到路径再拉出来（需要 Shizuku / root 能读 /data/app）
pm path com.HoYoverse.hkrpgoversea
#   package:/data/app/~~xxxx/com.HoYoverse.hkrpgoversea-yyyy==/base.apk
#   package:/data/app/~~xxxx/com.HoYoverse.hkrpgoversea-yyyy==/split_config.arm64_v8a.apk
adb pull /data/app/~~xxxx/com.HoYoverse.hkrpgoversea-yyyy==/split_config.arm64_v8a.apk

# B. 没装：直接向 Play 拉（需要 aas token）
apkeep -a com.HoYoverse.hkrpgoversea -d google-play -o split_apk=true .

# C. 第三方站：APKMirror / APKPure 上的 XAPK / APKS 就是这堆 apk 打的包，解压即得
```

拿到的是一堆 apk 就直接整堆丢给扫描器，它自己会挑 arm64 的 so；XAPK 先解压。

## 三步适配一版游戏

### 1. 体检

```bash
python3 tools/scan_game.py base.apk split_config.arm64_v8a.apk
# 或者给目录、给解压后的 .so，都行
python3 tools/scan_game.py ./apks/
python3 tools/scan_game.py libil2cpp.so
```

输出里看三件事：

- **哪个 so 是引擎库**。拆开的构造是 `libil2cpp.so` + `libunity.so`；单库构造是 miHoYo 自定制的一个 `lib*.so`。`bootstrap` 按 `libil2cpp.so → libunity.so → libhkrpg.so → libGame.so` 的优先级取第一个加载到的，实际用了哪个会写进日志的 `engine-lib` 一行。
- **Build ID**。这是地址表的 key，`readelf -n` 里那个 GNU Build ID。
- **有没有导出 `il2cpp_*`**。有 `il2cpp_class_from_name` / `il2cpp_runtime_invoke` 这一批，就能走托管调用，一个原生 RVA 都不用——那是另一条更省事的路（见文末 TODO）。

### 2. 填表

```bash
python3 tools/scan_game.py split_config.arm64_v8a.apk --template offsets.csv
# 顺手把 so 解出来给 IDA 用
python3 tools/scan_game.py split_config.arm64_v8a.apk --extract ./libs
```

`offsets.csv` 82 行，每行 `名字,RVA[,入口16字节]  # 属于哪个库`。行尾注释会标出这项该去哪个 so 里找
（9 项在 `libil2cpp.so`：`manager` `arrayNew` `objectNew` `typeObject` `classFromType` `wrapperFor` `newString` `pin` `unpin`，
其余 73 项在 `libunity.so`）。名字与 `app/src/main/cpp/offsets.hpp` 的 `enum Offset` 一一对应，别改名、别调序（往末尾加可以）。

- `RVA` 是相对模块基址的偏移（文件偏移基本就等于它，`PT_LOAD` 段内 `p_vaddr` 与 `p_offset` 通常同值，按 `p_vaddr` 算最稳）。
- 带第 3 列的 5 项（`componentsInChildren`、`gameObject`、`receive`、`lodBind`、`meshBones`）在挂钩前会逐字节比对开头 16 字节。填了更安全；不填就只做地址检查。
- 填 `0x0` 表示这一版还没找到，模块会把它算进 `pendingRequired`，但不会去碰它。

找地址的办法和原神那边一样：`IDA` / `Ghidra` 载入 so，`dump.cs` 里拿 `MethodIndex` 和签名，按 `il2cpp` 的 `MetadataUsage` 反查方法体；或者对已知版本做字节比对把 RVA 平移过来（`ADRP`/`B`/`BL` 之外的字面量会跟着漂，`bootstrap.cpp` 的 `codeMask` 就是干这个的）。

### 3. 编译进去

```bash
python3 tools/scan_game.py split_config.arm64_v8a.apk --map offsets.csv \
    --emit app/src/main/cpp/generated_offsets.inc
# 多个 so 时用 --lib 指定拿谁的 Build ID；也可以 --build-id 直接给
```

`generated_offsets.inc` 存在时，`app/build.gradle.kts` 会自动给 CMake 加 `-DHSRBOX_GENERATED_OFFSETS=ON`；`offsets.hpp` 用这个宏把这个文件 include 进 `namespace ms::abi`，`select()` 按实际加载的 Build ID 选表。换一个游戏版本就换一个 `.inc`，代码不用动。

## 编译

```bash
# NDK 27.2 + cmake 3.22
gradle :app:assembleDebug
```

`app/build.gradle.kts` 里 `abiFilters` 只有 `arm64-v8a`；`minSdk 26`。CI 在 `.github/workflows/build.yml`，推 tag 出 release。

## 目标与 ID 约定

| 服 | 包名 |
|---|---|
| 官服 | `com.miHoYo.hkrpg` |
| B 服 | `com.miHoYo.hkrpg.bilibili` |
| 国际服 | `com.HoYoverse.hkrpgoversea` |

三处都要跟着改的地方：`AndroidManifest.xml` 的 `<queries>`、`resources/META-INF/xposed/scope.list`、`res/values/arrays.xml` 的 `xposed_scope`，以及 `Control.java` 的 `GAMES` / `supported()`。

角色 ID 是 4 位 AvatarID（`1001` 三月七、`1102` 希儿、`1212` 镜流……）。内置表在 `Characters.java` 的 `BUILTIN`，收录到 1.x 主力；缺的直接往 `HSRBox/characters.json` 里写，同 ID 覆盖，不用重编：

```json
[{"id":1310,"name":"砂金","model":"aventurine","aliases":"aventurine 砂金"}]
```

## 目录

```
app/src/main/cpp/
  offsets.hpp        地址表与待定状态（本端口改的核心之一）
  bootstrap.cpp      LSPosed 入口：等引擎库、选表、挂钩
  runtime.cpp        运行时：扫实体、换 Mesh、贴图、骨骼、LOD
  model.hpp          HSRBox 模型格式（manifest / model.bin / learned.json / skeleton.json）
  migoto/            3DMigoto INI 引擎（变量、资源、drawindexed、Present、按键）
  logic_game.hpp     ini 逻辑 → 游戏内每帧选择
  vk_capture.hpp     索引缓冲 hash 诊断（只读，默认关）
  dds.cpp            DDS → RGBA
tools/
  scan_game.py       体检 + 生成地址表（本端口新增）
app/src/main/java/com/hsrbox/tool/
  Characters.java    角色表（本端口改成崩铁 4 位 ID + characters.json 覆盖）
  ModelConverter.java  转换器
  ...                界面与文件管理
```

## 已知缺口（下一步）

1. **地址表是空的**。这是唯一挡在"能替换"前面的东西，需要拿你自己的游戏包跑上面三步。
2. **实体发现仍是原神那条路**：`manager → entity → MonoVisualEntityTool → SkinnedMeshRenderer[]`。崩铁的类名和字段布局不同，等地址表有了，`Runtime::calibrate` 里那几个候选偏移要按崩铁的 dump 替换。更省事的做法是改成 `Object.FindObjectsOfType(SkinnedMeshRenderer)` + 预制体名匹配，代价是每帧多一次扫描，好处是几乎不依赖偏移。
3. **assets 里的哈希数据已清空**：`pc-hashes.json`（GIMI 索引缓冲 hash）和 `outfits.json`（原神时装）对崩铁无意义，`device-hashes.json` 是 9.9 MB 的原神数据，已置空。要用按 hash 匹配贴图/时装，得用 SRMI 的资产重新生成。
4. **托管调用后端**：如果体检发现 `libil2cpp.so` 导出了 `il2cpp_*` API，可以整条绕开原生 RVA——用 `il2cpp_class_from_name` 找 `UnityEngine.Mesh`，`il2cpp_runtime_invoke` 调 `set_vertices` / `set_triangles`，跨版本几乎不用改。这是崩铁比原神省事的地方。
5. `tests/` 下的 Java 单测还是原神时代的，没有跟进。

## 许可

GPL-3.0（派生自 GIBox）。见 `LICENSE`、`COPYING`、`THIRD_PARTY.md`。Dobby（Apache-2.0）、bcdec（MIT）、Compose-Settings（Apache-2.0）、LSPosed。
