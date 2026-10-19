# 64 · 完全反编译总纲 —— 判据：重新编译后与原版功能完全一致

> 状态：⏳ 进行中（**M1 ✅ 闭环 · M2 ✅ 语料完成**；M3+ 未开工）
> 工具：`tools/tufa_ir.py`（TUFA ⇄ .tasm 无损管线）、`tools/ghidra/DecompileAll.java` +
> `analyzeHeadless`（原生层全量反编译）、REA/Ghidra 12.1.4（会话式辅助分析）
> 产物：`out_tasm/`（2,224 个 `.tasm` 源文件，32 MB）、`out_native_decomp/`（10,875 个 `.c` + `_index.csv`，30 MB）

---

## 0. 目标与等价判据

目标（用户 2026-10-16 指令）：**把 LASR 完全反编译；重新编译后的产物在功能上与原版游戏完全一致。**

游戏由三层构成，等价判据分层定义（结论必须可实证，「实测」与「推测」分开）：

| 层 | 内容 | 等价判据 | 现状 |
|---|---|---|---|
| ① TUFA 逻辑层 | 2,224 个 `.class`（2,242 个类映像 / 14,879 个方法体 / 384,212 条指令）——全部游戏规则 | **字节级**：反编译源 → 重编译 → 与原版 `.class` 逐字节一致 ⇒ 同一 VM 下功能恒等是平凡推论 | ✅ 已证明（M1） |
| ② 原生引擎层 | `LASR.exe` 3.6 MB（.text 2.9 MB / 10,875 个函数）：VM 解释器、渲染、物理、音频、FLZD、RPAK、输入/FFB、网络 | **功能级**：字节 matching 不现实（2006 MSVC 工具链/链接顺序不可复现）⇒ 判据 = 差分行为测试（见 §3 M4） | ⏳ 语料已齐（M2） |
| ③ 资产层 | RPAK / FLZD / zip / SDAT 容器 | 用原格式重打包**逐字节一致**（FLZD 已用游戏自己的 `flzd_pack` 证明，docs/60） | ✅ 既有闭环 |

「重新编译后的产物」的最终形态（M5）：重建的原生 exe + 从反编译源码重编的 `.class` +
按原格式重打包的原版资产 ⇒ 可安装、可运行、行为与原版一致。

---

## 1. M1 · 逻辑层：TUFA ⇄ `.tasm` 无损管线 ✅（2026-10-16 实测）

`tools/tufa_ir.py` 四个验证模式与结果：

| 模式 | 判据 | 结果 |
|---|---|---|
| `--roundtrip` | parse → IR → serialize == 原字节 | **2224/2224** 逐字节一致，0 失败 |
| `--text-roundtrip` | IR → `.tasm` 文本 → 重新 parse → serialize == 原字节 | **2224/2224**，0 失败 |
| `--emit-all out_tasm` | 落盘 `.tasm` → **从磁盘**重组 → 逐字节比对 | **2224/2224**，0 失败 |
| 单类抽查 | `Bet.class` 重组后 sha256 | `9e2879…078ff` 与原版一致 |

### 1.1 容器格式（本轮全量定案，含两处勘误）

```
file    = "TUFA" u32 ver(=4) u32 build(=0x1451e 全库常量)  chunk*      ← chunk 流精确铺满文件，无尾部填充
chunk   = tag(4cc) u32 size body
TUFA    = 内嵌类分隔块（size=4，body=build）⇒ 多类文件：7 个文件共 18 个内嵌类（GmRace/Track/Vehicle…）
CONS    = u32 count; count×entry；entry tag 只有 0(UTF8)/4(1×u32)/5(2×u32)/7(2×u32)   ← docs/10 已定案，本轮全量复核通过
FILD    = u32 n_static; n×4×u32; u32 n_inst; n×4×u32                                    ← docs/11
MTHD    = 与 FILD 同构的两组 20 B 记录（★ 勘误：docs/10 §2、11 §2 旧版「reserved+n」是 Bet 无静态方法的特例；
          本轮 2,242/2,242 铺满 + group1 全带 ACC_STATIC(0x8)、group2 全不带 ⇒ 语义钉死）
CLSS    = size/4 × u32（实测 24 B×2149 / 28 B×85 / 32 B×7 / 36 B×1，变长）
TREE    = u32 n; n×(u32 rec_size; 指令流)；指令 = u8 opcode(+u32 payload 当宽度 5)，
          宽度/名字表 = docs/08 §5.4，14,879/14,879 条记录精确落位，非法 opcode(0x00/0x0a/>0x55) 0 次
```

FILD 的 `extra` 字段 = 编译器缓冲区残留（docs/11 §1.3）：**照原样保存在 IR/`.tasm` 里**。
它对 VM 是惰性的 ⇒ 高层源码重编时可以置 0（功能仍恒等），但字节级判据要求保留。

### 1.2 `.tasm` 文本格式（可编辑的反编译源码，往返无损）

行式文本：`TUFA ver build` 头；`CONS/FILD/MTHD/CLSS/TREE` 块按文件顺序；指令用助记符 +
十进制/0x 载荷；`;` 注释携带解析出的名字（类名/方法名/字符串字面量/跳转目标/浮点值），
汇编时被剥离。注释里的字符串已展平（`\n` 等控制符转义）——未展平前 5 个含多行字符串的类
（CarTabListComponent/ItemlistComponent/Loading/GmReplay/Tyre）会把行切碎。

## 2. M2 · 原生层：Ghidra 全量反编译语料 ✅（2026-10-16 实测）

* REA 会话（Ghidra 12.1.4，provider=ghidra）：image base `0x400000`，段 `.text 0x401000–0x6e7000`，
  **procedure_count=11,180 / string_count=9,436**；目标引用 `CRC_release.pdb`（RSDS GUID
  38a07b49-…，无符号文件 ⇒ 全部函数无名）。
* `analyzeHeadless -postScript DecompileAll.java`：**DONE total=10,875 ok=10,875 fail=0**
  （Ghidra FunctionManager 口径；与 REA 的 11,180 差 305 = 外部/thunk 计数口径差）。
* 产物 `out_native_decomp/<VA>.c`（每函数一个文件，首行 `// func <VA> <name>`）+ `_index.csv`
  （addr,name,status,info 全 ok）。这是 M4 模块化重建的原料，不是最终源码。

### 2.1 M4 第一步 ✅：函数 → 模块归属表（`tools/native_modules.py` → `out_native_modules.csv`，2026-10-16）

* 方法：10,875 函数 / 20,991 条无向调用边（语料里 `FUN_xxxxxxxx` 引用）；种子四路 =
  docs 十六进制地址投票（按 doc→模块映射，地址归属到**包含它的函数** ⇒ 调用点标注调用方）+
  `out_native_methods.csv` 1,523 条 VM 桥接（→bridge）+ 导入 thunk 按 DLL 映射到功能模块 +
  手工核实地标（flzd 链/FFB 链/VM 分派/帧循环/RPAK 加载器）；模块标签用**半监督标签传播**
  （种子钉死、同步轮迭代、tie=contested），BFS 距离与最近种子只作溯源列。
* 结果：1,360 种子 → **9,006/10,875 已归属（83%）**；直方图 physics 4,236 / bridge 1,979 /
  render 688 / core 649 / audio 314 / vm 210 / ai 185 / assets 159 / replay 129 / input 126 /
  **net 109**（ws2_32 API 扫描新识别出的原生网络代码）/ contested 99 / scene 80 / ui 24 / packing 19。
* 验证：地标抽查 **8/8 OK**（flzd 链=packing、VM 分派 owner 0x653880=vm、FFB 三推送=input、
  帧循环=core、两个 RPAK 加载器=assets）；net-API 调用者 **22/22=net**。
  Ghidra 还自己恢复了 `LZW_expand_size`（0x401640）——FLZD 内层是 LZW 变体（推测，待 M4 复核）。
* **unassigned 1,869 = 1,528 个小连通分量（最大仅 19），大多挂在 .text 尾部 0x6dc000–0x6e4xxx**
  ⇒ 推测为 CRT/静态库例程与孤立小工具（含 `Unwind@*` 异常处理），**没有成建制的漏网子系统**。
* ⚠ 启发式警告：physics 占 39% 明显偏粗（docs/44 一篇就贡献 262 个种子，把车辆动力学/碰撞/
  共享数学都吸进来了）；归属表是**规划底稿不是结论**，M4 各模块开工前需人工复核边界
  （CSV 带 dist/seed 溯源列，逐函数可审计）。

## 3. 路线图（按价值排序）

### 3.0 M3 第一步 ✅：可逆 AST 上提器（`tools/tufa_lift.py`，2026-10-16）

* 设计契约：**每条方法体 lift 成 AST 后立即重发射验证**，与原始 (op,payload) 序列不一致
  就不接受（回退 Raw）⇒ 管线字节恒等由构造保证，**覆盖率是唯一诚实指标**。
* v1 实测：**8,435/14,879 记录（56.7%）lift+验证通过**；验证记录覆盖 131,516/384,212 条指令
  （34.2%）。渲染器 `--render` 输出 Java 风格文本，`Bet.<init>(IPart)` 的三元式与
  `out_pseudo` 真值**逐字符一致**。
* 本轮钉死的语义（都有双重证据）：
  - **`JMP_NE(0x18)` 在条件为 FALSE 时跳转**（fallthrough = true 臂）——out_pseudo Bet
    三元式 + getName 的 if-return 形态两处独立对表；渲染方向据此修正过一版。
  - **`JMP_EQ2(0x1a)` = switch-case**：弹 key、窥视其下的 value（留在栈上），链尾 `POP`
    回收（普查：前导 1,794×INT_LITERAL/515×FIELD_REF_STATIC，后继 1,663×INT/231×POP）。
  - **`SHORTCUT_AND/OR(0x3e/3f)` = 惰性 `&&`/`||`**：`lhs; SC; JMP end; rhs; end:`
    （972/972 后继都是 JMP）⇒ 折叠成 ShortCircuit 节点。
  - **数组字面量元素先压栈**：`e1..en; INT n; NEWARRAY r; ARRAY_INIT n`（我第一版按
    「元素后压」弹栈，1,002 条记录立刻 abort——被验证器全数抓住）。
  - `NEW; args; INVOKESPECIAL <init>` **可以没有 DUP**（无值构造），emit 里无条件补 DUP
    曾造成 1 条 6vs5 失配 ⇒ `had_dup` 标志。
* v2 工作清单（按 abort 计数排序）：**6,151 条语句级控制流栈下溢**（if/else 臂带 RETURN、
  循环——线性游走表达不了，需要 CFG 结构化，复用 docs/09/15 的结构化成果）；211 SHORTCUT
  非标准形；59 DUP 非 NEW 形态（赋值表达式/链式）；14 ARRAY_INIT 长度异形；7 DUP_X1/X2/DUP2；
  1 条 44v44 失配待剖。

### 3.0.1 M3 第二步 ✅✅：v2 结构化 + 两条语义翻案（2026-10-16 同日）

* **v2 实测：14,772/14,879 记录（99.3%）lift+字节验证通过；指令覆盖 357,315/384,212（93.0%）**，
  全量 1.3 s。剩余 107 条 = 21 栈洗牌指令（DUP2/DUP_X*）+ 9 SHORTCUT 异形 + ~77 条微失配
  （全部被验证器安全回退为 RAW，字节恒等不受影响）。
* **两条翻案（v1 的 6,151 栈下溢大头原来是它们，不是控制流）**：
  ① **非 `<init>` 的调用一律压返回槽（void 也压）**：全库普查 `void 调用; POP` 33,524 次、
    void 调用不带 POP **0 次** ⇒ 表达式语句 = `call; POP`（Discard）。
  ② **SHORTCUT_AND/OR 不弹 lhs**：真实形态 `lhs; SC; JMP end; <rhs 区以 ANDAND/OROR 收尾>; end:`
    ——SC 只是惰性标记，rhs 区在 lhs 之下把它合掉（Bet.getName 对表 out_pseudo 确认
    `(a||b) && c` 嵌套链）。ShortCircuit 节点存 lhs + 合并结果 R + rhs 指令尾（R.emit 必须
    以 lhs.emit 为前缀，否则拒绝折叠）。
* 新增结构节点：IfThen（含 RETURN 臂）/ IfElse / While（尾部后向 Goto 识别，基栈快照恒等）/
  DoWhile（后向条件跳 + 语句后缀捕获）/ DupRef+AssignDup+PutFieldDup（`E; DUP; STORE` 赋值留栈，
  DupRef.emit=[]，STORE 后把栈下原件替换成 DupRef 防二次发射）；ARRAY_INIT 允许长度≠元素数
  （部分初始化）。渲染修正：JMP_NE 条件跳渲染为 `if (cond)`（跳=假）。
* 性能护栏（都是实测踩出来的）：区域提升**记忆化**（key 含 base 节点引用 tuple，防 id 复用）、
  New 节点副作用**失败回滚**、无界臂**窗口上限 1500 指令**、每记录**步数预算 80n+40000**。
  没有这四条，「void 也压栈」让更多记录走深后区域尝试爆炸（一次全量跑 >420 s 超时）。
* **产物 `out_src/`**：2,224 个 `.lasr.java` 源文件（14 MB），2,207 个完全结构化、17 个含
  RAW 回退记录。与 `out_pseudo` 真值抽查（Track 的 roomUserInfoVector 循环）**逐行一致** ——
  两条独立管线互相验证。
* 折叠统计：if_then 6,291 / shortcut 2,010 / if_else 1,263 / while 996 / ternary 396 /
  switch 356 / do_while 31。

### 3.0.2 M3 收尾（源码形态）✅✅：`.lasr.src` 可编译源语言 + src→字节码编译器，2,224/2,224 字节往返（2026-10-16 同日）

* **定案：源语言 = `.lasr.src`（自定义严格 DSL，不是 Java 方言）**。理由：TUFA 是栈式 VM，
  字节级往返要求源码保留栈形态（DUP 别名、部分初始化数组、表达式内跳转），Java 方言做不到
  无损；DSL 每条语句一行、可 diff、可手改，重编器就是 `tufa_lift` 的同一套节点类（单一真值）。
* **工具 `tools/tufa_src.py`**（dump/compile/roundtrip/emit-all 四个 CLI）：
  `dump_src` = `.class` → lift AST → 严格单行 token 文本（`SRCREC`；失败记录回退 `RAWREC`
  逐指令文本）；`compile_src` = 文本 → 递归下降解析回同一套 AST → `tufa_ir` 序列化。
  **实测：`--roundtrip` 2,224/2,224 文件逐字节一致（14,771 结构化 + 108 RAWREC），4.8 s，
  连跑三次结果确定**；`--emit-all out_src_v2` 落盘 2,224 个 `.lasr.src`（2,167 个全结构化），
  再**从磁盘全量重编译比对 extracted/ 原件：2,224/2,224 逐字节一致（3.4 s）**。
* **解析器核心 = 指令游标（ctr）重放**：文本顺序≠布局顺序的地方必须显式管理游标，
  这是本轮踩坑全记录（每个都造成过字节失配，已修）：
  ① 条件结构吞跳转指令：IfThen/IfElse/While 的 cond 后 `ctr+=1`；Ternary 的 fall 前后各 +1；
  ② **SC 共享前缀**：`SC op lhs R` 里 R 的 emit 以 lhs 为前缀 ⇒ 解析 R 前把 ctr 拨回 start，
    R 尾部（offset ≥ len(lhs)）里嵌套的 Ternary/SC 绝对跳指 +2 补偿（SC-op+JMP 两条），
    且 **ShortCircuit.tail 必须惰性计算**（存切片会把 fixup 前的旧 JMPREL 索引冻死）；
  ③ **布局顺序 ≠ 文本顺序的三个节点**：`ArrayAccess.emit = idx;arr;ACC`（文本已改 idx-first）、
    `ArrayLit.emit = elems;INT n;NEWARRAY;INIT`（len 节点不参与 emit，解析时游标回滚）、
    `New.emit = NEW;[DUP];args;INIT`（解析 args 前 ctr += 1+had_dup）；
  ④ **While/DoWhile 的回跳目标（header）可以落在 cond 之前**（`while((x=foo())!=null)` 形态
    回跳进前面的赋值表达式中部）⇒ header 必须显式写进文本（`WHILE op header cond {…}`、
    `DOWHILE op header {…} WHILE cond`），不能派生；
  ⑤ DUP 别名编号 k 由 `ASSIGNDUP k`/`PUTFIELDDUP k` 定义、`DUPREF k` 引用（每记录独立编号；
    渲染端 dupreg 未注册即 Abort→RAWREC，安全回退）。
* 108 条 RAWREC 精确分布（`RAW_REASONS` 诊断槽实测）：76 emit 微失配 + 21 栈洗牌
  （0x2b/0x2c/0x2d）+ 9 SHORTCUT 异形 + 1 栈终态非空（Gamelogic T82 rec68）+ 1 DUP 形状
  可 lift 不可渲染（IVehicle T72 rec54，DUP 别名先被 CALL 消费）。**RAWREC 不损失任何字节**
  ——往返 100% 靠它兜底；结构化率 99.27%。

| 里程碑 | 内容 | 验收判据 |
|---|---|---|
| **M3 逻辑层上提** ✅ | `.tasm` → 高层源码（吸收 `out_pseudo/` 既有结构化伪码做参照），实现 src→`.tasm` 编译器 | **已达成（§3.0.2）：`.lasr.src` DSL，2,224/2,224 类重编逐字节一致（结构化 99.27%，余 RAWREC 兜底）** |
| **M4 原生层模块化重建** | 按已钉死的地标切模块：FLZD(`0x1D00/0x1850/0x1570`)、TUFA VM 解释器(分派 `0x653900`)、物理桥(docs/44)、渲染(docs/22–30)、音频(docs/31–34)、输入/FFB(docs/63)、RPAK(docs/52)、网络(dedicated server)；从语料逐模块重写成可编译 C/C++ | 每模块差分测试：FLZD 逐字节复现、`.rpl` 录像真值对表（`tools/replay_dynamics.py`）、dedicated server TCP 12346 协议对打、渲染截屏对拍 |
| **M5 整机集成** | 新 exe + 重编 classes + 原版资产重打包 → 完整可安装游戏 | **实机对比原版**（需用户机器）：启动/生涯/比赛/回放/FFB 手感/联机 |

## 4. 未解清单（✗）

* ✗ ~~M3 高层源语言形态未定~~ **✅ 已定案（§3.0.2）**：`.lasr.src` 严格 DSL + src→字节码
  编译器（`tools/tufa_src.py`），**2,224/2,224 文件字节级往返实测通过**（含从磁盘全量重编译
  比对），语料 `out_src_v2/`。残留长尾 = 108 条 RAWREC（76 微失配 / 21 栈洗牌 / 9 SC 异形 /
  1 栈终态 / 1 DUP 渲染形状）——**不阻塞字节往返**，只影响「可读源码覆盖率」（99.27%），
  可延后攻坚。M3 逻辑层主线（可编辑源形态 + 重编字节一致）已闭环。
* ✗ ~~原生层「函数 → 模块」归属表未生成~~ **✅ v2.1 已生成**（§2.1，`out_native_modules.csv`）；
  残留：physics 需细分（车辆动力学/碰撞/共享数学）、99 个 contested 与 unassigned 前几大分量
  需人工点名（CRT? FMV 播放? 未知工具链?）。
* ✗ ~~`dedicated server.exe`（300 KB）未入库~~ **已判定（2026-10-16，`tools/ds_triage.py` 实测）**：
  **独立代码基，不是 LASR.exe 的子集** —— TUFA/JVM/编译器字符串 0 命中（`JavaMachine::loadClass`、
  `JVM::addClass`、`Cannot parse class`、`CRC_release.pdb` 全无）；导入仅 `winmm/ws2_32`
  （无 d3d9/dinput8/fmod ⇒ 无渲染无输入无音频）；`.text` 16 B 块仅 9.2% 出现在 LASR 映像里
  （量级 ≈ CRT/库例程）；自带 `dedicated`/`Standalone` 字符串；`.data` vsz 1.29 MB（BSS 重型，
  典型服务器构建）。⇒ 联机功能复刻需要**它自己的反编译语料**（.text 仅 255 KB，成本小），
  排在单机功能一致（M3/M4/M5 主线）之后。
* ✗ CLSS 28/32/36 B 变体里第 7–9 个 u32 的语义未定（不阻碍往返；疑与接口/内部类有关）。
* ✗ 原生重建的工具链选择（目标「功能一致」不要求字节一致 ⇒ 可用现代 MSVC/MinGW，但 FMOD
  （`fmodex.dll`）与 `d3dx9_30.dll` 是外部依赖：继续动态链接原版 DLL 还是等价替换，M4 时定）。
* ✗ M5 实机验收清单待写（按用户习惯：逐条操作单）。
