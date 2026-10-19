# LASR 完全反编译 —— 结果总览（给人看的一页）

> 这一页是一次完整研究的工作区快照整理；数字来自实际扫描，不是手写。
> 正文细节见 `docs/65_VM_INTERPRETER.md`；**本仓库不分发下列任何反编译产物**，只给生成方法。

## 一句话

由 `LASR.exe` + `java/classes.zip` 反编译出的 TUFA 容器，经我们的管线**重新序列化后与原版逐字节相同**；
`java/classes.zip` 用游戏自己的压缩器重打后 **md5 与发行版一致**，装回游戏跑完整场比赛无差异。

## 四个等价判据（都有机器证据）

| 判据 | 结果 | 怎么复现 |
|---|---|---|
| 容器层：`.class → .tasm → .class` | **2,224 / 2,224 文件逐字节一致** | `tools/tufa_ir.py --roundtrip` |
| 源码层：`.class → .src → .class` | **2,224 / 2,224 逐字节一致** | `tools/tufa_src.py --roundtrip` |
| 打包层：重打 `classes.zip` | **304 / 304 条目一致，整包 md5 相同** | `tools/repack_classes.py` |
| 运行层：静态面 / 引导 | **classes 1,058 / fields 5,403，0 bad；`Init.<init>` 93,159,836 步 errs=0** | 解释器批处理 + 对比脚本（`docs/65 §10`） |

## native 层范围（550 条真工作面，全部有结论）

| 桶 | 数量 | 处理 |
|---|---|---|
| 语料真调用 + 有地址 | 549 | 158 条已实现并对拍；其余按引擎状态读/调用分类 |
| 语料真调用 + 无地址 | 1 | `GameRef.create(…GameType…)`：引擎未注册 ⇒ 不可达 |
| 声明但从不调用（死声明） | 1,086 | **0 调用点**（逐条对语料里的调用点）|
| 注册但容器未声明 | 430 | 341 无此方法 / 82 无此类 / 7 被 Java 体覆盖，全部 `push=1 call=0` |

## 产物怎么自己生成（仓库里不带任何反编译结果）

所有产物都由本机脚本从**你自己的**游戏副本现场生成；仓库只保留工具与结论。

| 产物 | 生成方式 |
|---|---|
| 明文类容器（`.class` 树） | 用 `tools/lasr_flzd.py` 的 FLZD 解包从 `java/classes.zip` 与各 `vehicles\*\classes.zip` 取出 |
| `.tasm` 文本（容器层） | `python tools/tufa_ir.py --emit-all OUTDIR`（往返校验：`--roundtrip`） |
| `.lasr.src` 源码形态（源码层） | `python tools/tufa_src.py --emit-all OUTDIR`（往返校验：`--roundtrip`） |
| 重打包的 `classes.zip` | `python tools/repack_classes.py`（逐条比对，未改动条目必须与发行版逐字节相同） |
| 静态字段真值 | `vm/tufa_vm.c` 编出来的解释器：`tufa_vm --clinit <类> --dump`（全量批处理见 `docs/65 §10`） |
| 伪码 / Ghidra C 伪码 | 不在本仓库；`docs/65` 里写明用哪套反编译器与参数，可自行复现 |

## 实机已验证（只读挂接 + 文件对拍）

| 现象 | 结论 |
|---|---|
| 生涯菜单条目数 | 只显示 1 个（`.bak` 不匹配 `*.sav`）|
| 单位 Kph↔Mph | `save/game/options +0xcc` 1↔0，双向 |
| 写盘时机 | 结算画面**不写**，点 Back to garage 才写（`lastSaveTime` 为证）|
| 快速比赛 vs 生涯挑战 | 前者不动计数、不写 chronicle；后者 `winSum+1 / raceSum+1 / prestige` 变、新增 chronicle |
| HUD | 逆行出 `WRONG WAY`；不撞车跑完一圈出 `CLEAN LAP` |
| `bestLapTime` | 单人恒 0（只有联机房间参数会写）|

## 明确的边界（不是缺口）

1. **202 条 engine-state native** 的数值读自引擎内存：产物里 native 仍是原版、引擎仍由原版写，
   沙箱返回 0 与“原版在沙箱里”等价 ⇒ 不进验收面（`docs/65 §10.57`）。
2. **整局逐帧等价**（渲染/物理）只能实机目视，超出本工具能力。
3. `docs/65 §9` 里剩余项（halt 位、行号表、`FUN_0041d270`、108 RAWREC）均已证明是**诊断/呈现管线**，
   没有 Java 侧读者（§10.70）。

## 从哪开始读

```
docs/00_INDEX.md              全部文档索引
docs/64_FULL_DECOMPILATION.md 反编译总纲（三层判据）
docs/65_VM_INTERPRETER.md     VM 重建全过程（本节 = 最近几轮）
docs/48_RACE_SESSION_RULES.md 一局比赛的规则（可直接做重制规格）
tools/                       往返编译器 / 重打包 / 补丁脚本
mods/debug-menu/README.md    调试菜单补丁（3 字节）
```
