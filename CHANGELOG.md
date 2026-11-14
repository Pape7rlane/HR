# 变更记录

## 0.1.0 — 从 GIBox 分出，对准崩铁

基于 GIBox 1.0.3（GPL-3.0）。这一版的目标是"结构上对得上崩铁、能编、装上也安全"，模型的真机替换等地址表填完。

### 改了什么

- 包名、`applicationId`、so 名、JNI 符号：`com.gibox.tool` / `libgibox.so` → `com.hsrbox.tool` / `libhsrbox.so`。
- 目标游戏：`com.miHoYo.Yuanshen` / `com.miHoYo.ys.bilibili` / `com.miHoYo.GenshinImpact`
  → `com.miHoYo.hkrpg` / `com.miHoYo.hkrpg.bilibili` / `com.HoYoverse.hkrpgoversea`。
  同步改了清单的 `<queries>`、`xposed_init` 的 scope、`xposed_scope` 数组、`Control.GAMES`、`supported()`。
- **引擎库定位**：不再写死 `libyuanshen.so`，按 `libil2cpp.so → libunity.so → libhkrpg.so → libGame.so`
  的优先级取第一个加载到的，实际用了哪个写进 `engine-lib` 日志。崩铁两种库布局（拆分 / 单库）都能对上。
- **地址表**（`offsets.hpp`）：去掉原神 7.2 的两套 RVA，改成按 Build ID 选表的空表 +
  `generated_offsets.inc` 插件位。空表时 `select()` 不报错，`pendingRequired` 列出缺的项，
  `bootstrap` 在挂钩前返回，只写 `offsets-pending` 状态——不会拿错的地址去 hook。
- **入口字节校验**：`codeMatches` 遇到空 hex 只做地址检查（不再当失败），未解析的项单独记进日志，
  `handoffChecks` 也补了 `found()` 判断，避免拿 `notFound` 去读内存。
- **角色 ID**：8 位原神 AvatarID → 4 位崩铁 AvatarID。`core.hpp`、`model.hpp`、`runtime.cpp`、
  `texture_diag.hpp`、`pc_hashes.hpp` 与 Java 侧的六处范围判断一起改。旧存档里的 8 位 ID 会被拒。
- **角色表**：`Characters.java` 换成崩铁名单（4 位 ID + 内部模型名），并支持从
  `HSRBox/characters.json` 覆盖/补充，不用重编。原神那 150 行表删掉。
- **assets**：`outfits.json`（原神时装）、`pc-hashes.json`（GIMI 索引缓冲 hash）、
  `device-hashes.json`（9.9 MB 原神数据）置空，等 SRMI 资产重新生成。
- **新增 `tools/scan_game.py`**：纯 Python、无第三方依赖的 ELF 体检器。读 APK 或单个 so，
  报架构 / Build ID / 导出符号 / `il2cpp_*` API / Unity 版本 / `global-metadata.dat` 是否加密，
  并从填好的 CSV 生成 `generated_offsets.inc`。
- **CMake**：`HSRBOX_GENERATED_OFFSETS` 开关；`generated_offsets.inc` 存在时 gradle 自动带上。
- `versionCode` / `versionName` 归零重记：`1` / `0.1.0`。

### 0.1.2 — API 表探测

- 新增 `api_probe.cpp/hpp`：在游戏进程里 `dlsym("il2cpp_get_api_table")` 调一次，把表头、每个指针的落点
  （模块 + 偏移 + text/rodata/data）、每个函数开头 4 条指令、以及 `B` 桩的跳转目标，写到 `api-table.txt`。
  只读，不挂钩，不依赖地址表——空表状态下也会跑，摆在 `pendingRequired` 检查之前。
- `control.json` 的 `apiProbe`（默认开）控制这次探测；默认在引擎库出现后等 3 秒再调。
- 启动顺序上，探测放在 UID 隐藏之前、待定检查之前。
- 起因：`libil2cpp.so` 460 MB 只导出 `il2cpp_get_api_table` 一个函数，其余 3002 个是假名数据符号；
  `il2cpp_class_from_name` 那批一个都没有。要是这张表里带着 il2cpp 运行时 API，整条原生 RVA 路子都可以不要。

## 0.1.1 — 双库

- 崩铁 4.6.54 实测：引擎在 `libunity.so`，托管在 `libil2cpp.so`，不是原神那种单库融合。
- `abi::Lib{Engine,Managed}` + `abi::libOf(Offset)`：9 项归 `libil2cpp.so`（`manager` `arrayNew` `objectNew`
  `typeObject` `classFromType` `wrapperFor` `newString` `pin` `unpin`），其余归 `libunity.so`。
- `Engine` 变成 `Engine{base, managedBase}`，新增 `Engine::addr(Offset)` 按归属选基址；
  `runtime.cpp` 里三处直接 `e.base+abi::at(...)` 的读法改成 `e.addr(...)`。
- `abi::Profile` 增加 `managedBuildId`；`abi::select(engineId, managedId="")` 双参。
  托管库对不上时只保留引擎侧并把原因写进 `pendingRequired`，不阻断加载。
- `bootstrap` 单独定位 `libil2cpp.so`，两个 Build ID 都记日志（`engine-lib` / `managed-lib`）。
- 引擎库优先级改成 `libunity.so` 在前（原先 `libil2cpp.so` 在前，会把 460 MB 的托管库当引擎基址）。
- `tools/scan_game.py`：模板按库标注归属、自动取两个 Build ID、`--managed-build-id` 可覆盖；
  体检时如果只看到 `il2cpp_get_api_table` 会直接说明托管调用那条路不通。

### 还没做

- 崩铁各版本的 RVA 表（要拿游戏自己的 so 跑扫描器）。
- 实体发现路径仍是原神的 `manager → entity → MonoVisualEntityTool`，字段偏移要按崩铁 dump 重定。
- `tests/` 下的 Java 单测还是原神时代的内容。
