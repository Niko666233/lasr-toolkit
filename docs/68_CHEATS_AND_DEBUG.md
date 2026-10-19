# LASR 作弊码 / 调试入口 —— 来自反编译产物的完整清单

> 来源：`out_pseudo/java/classes/**`（伪码）+ `out_tasm/**`（字节码指令流）+ `LASR.exe` 字符串与注册表。
> 全部结论都可在这三处复核，不引用任何外部资料。

## 0. 一句话

排行榜里能点出 **3 个**作弊码（4 次点选构成一个组合，前两位固定 `0,8`），效果分别是**送全部车**、**送当前车全部零件**、**当前车转宽体**。
三者都**不写存档计数、不改 prestige、不写 chronicle**，因此**对「风评」没有直接影响**。
另有 6 类调试入口（赛中组合键、两个调试菜单、控制台、命令行开关、引擎参数命令），其中多数被 `Config.majomParade` 这个**在容器里从未被置真**的开关挡着。

## 1. 排行榜作弊码：3 个

位置：PDA → Rankings 列表的行选中回调
`java.game.frontend.PDAPopup.TabList1_onChanged(Ljava.gui.Component;I)V`

```java
// 4 格环形历史，每次行选中把新行号压入末尾
cheat[0]=cheat[1]; cheat[1]=cheat[2]; cheat[2]=cheat[3]; cheat[3]=rowIndex;
```

字节码核对（`out_tasm/java/classes/game/frontend/PDAPopup.class.tasm`，`REC 4`，4539 字节）：
`(槽,常量)` 序列恰好三组 —— `(0,0) (1,8) (2,4) (3,5)`、`(0,0) (1,8) (2,7) (3,8)`、`(0,0) (1,8) (2,1) (3,9)`。
不是反编译器的结构化伪影（常量与比较指令都在流里）。

| # | 序列（列表行号，0 基） | 效果 | 前置条件 | 失败文案 |
|---|---|---|---|---|
| 1 | **0 → 8 → 4 → 5** | `Gamelogic.giveAllCars()`：把全部车辆入库 | 已拥有车辆数 **< 10**（`listVehicles()` 非空项计数） | `$41\|Wanna more, huh? Seek somewhere else!` |
| 2 | **0 → 8 → 7 → 8** | `Gamelogic.giveAllParts()`：当前车全部潜在零件入库 | 当前车存在且 **不是宽体** | `$43\|Impossible, sir!`（宽体车）/ 无车时静默返回 |
| 3 | **0 → 8 → 1 → 9** | `Gamelogic.convertToWideBody()`：当前车换成宽体版（车 id +`10<<16`，掉落旧车、重新入库并切到它） | 当前车存在且 **不是宽体** | `$45\|Already done, man!` |

成功时都伴随 `Pub\ItemMale.evt` 音效 + 顶部提示（`$40` / `$42` / `$44`）。

细节：
- 序列 1 的"是否已作弊过"判据是车辆数 < 10，**不是**标志位 —— 存档里车多于 10 台就永久失效。
- `giveAllCars()` 内部对每个宽体变体（`VID_x_WB`）调用 `removeCheatCar()`，避免同一辆车同时存在普通版与宽体版。
- 序列 3 会保留车的位置/朝向/父节点：先 `back2Store()` → `convertToWideBody()` → 再 `setParent/setMatrix`。
- 在 mainMenu 上触发时走的是 `mainMenu.cheatButton{,2}_onAction()`，即与调试菜单的按钮**同一个实现**（并顺带 `carTabComp.initCars()` 刷新车库列表）。

## 2. 对「风评」的影响：**没有直接影响**

游戏里与"别人怎么看你"相关的只有两套量：

### 2a. `Gamelogic.prestige`（声望 / 排名）

- 只在 `Challenge.apply(int)` 里被**替换**（不是累加）：
  `prestige = getOpponentPrestigeByStatus(status) + 1`，且要求最近一条 chronicle 的 `opRank < 100`；
  输/赢分别看对手状态的正负号。
- 玩家的名次 `getPlayerRank()` = 在 `opponents[]` 里按 prestige 从高往低数出来的位置。
- 三个排行榜作弊码**一处都不写** `prestige`，也不写 `races` / `raceSum` / `winSum`。

### 2b. `Bot.cheatFactor`（NPC 嘴里的"你这个作弊的"）

`Bot.analyseThis(RaceChronicle)` —— 注意传入的是 `Gamelogic.getChronicleAvg()`，
即**历次 chronicle 的平均值**，所以一次脏跑会长期留在风评里：

```
cheatFactor = 0
splineLeft > 10          -> 2      // 严重切赛道
splineLeft > 4           -> 1      // 切了
longestOffSplineTime > 4s -> +1    // 长时间在赛道外
crashes > pushes          -> -1    // 撞得多反而"像被欺负"
aggressivity<0 && aiLevel>0.5 -> +1 ; aggressivity>0.4 && aiLevel>0.5 -> -1
clamp 到 [0,2]
```
`prestigeFactor` 再按"你的 prestige 是否比该 bot 高 200"分档。
`botTalks[BOTTALK_*][prestigeFactor][cheatFactor][aggressionFactor]` 就是 NPC 说话的那张表
（`cheatFactor=2` 那档才是 `"I won't race with you. You cheat."` 之类的台词）。

**所以**：`cheatFactor` 的输入是"切赛道 / 出界多久 / 撞车次数"，与作弊码无关；
排行榜三码跑完，风评一个字段都不动。

### 2c. 唯一例外

`Gamelogic.cheatPushed`（两个调试按钮会置 `true`）**没有任何读者**（全语料 3 处：初始化 + 2 处写），
即游戏记录了"玩家用过作弊"却不消费它 —— 没有反作弊惩罚。

而**赛内**作弊（见 §3 第 1、2 条）会走正常结算 ⇒ 会像真赢一场那样改 `winSum/prestige`，
间接影响风评；那不是作弊码本身的功劳，是"结算成真赢"。

## 3. 其他作弊 / 调试方式（6 类）

| # | 入口 | 触发 | 作用 | 门槛 |
|---|---|---|---|---|
| 1 | `Track.checkCheats()` | 赛中同时按住 **PageUp + PageDown + Tab** | `gameMode.letMeWin()` 直接判胜（`cheatActivated` 保证一局只触发一次） | 仅 SINGLE |
| 2 | `Init.handleHotkeyEvent` case 6..11 | **Insert + Delete + 小键盘 `*`** 同时按住 | 翻转 `MakeBetPopup.mapChooseCheatActive` ⇒ 下注弹窗多出 3 个按钮（可选地图） | 无 |
| 3 | `IngameMenu.winCheat/looseCheat` | ESC 菜单里的两个调试按钮 | `letMeWin()` / `letMeLose()` | `Config.majomParade` 且 SINGLE |
| 4 | `mainMenu` 调试按钮组 | 车库界面 | `Cheat 1`=全部零件 / `Cheat 2`=全部车 / `prestigeUp` / `prestigeDwn` / `win`（翻转 `bastardRaceFinished`，hover 文案 "Win/Lose BastardRace"） | `Config.majomParade` |
| 5 | `ConsoleWindow`（`Init.console`） | **右 Win 菜单键（RCDIK_RMENU）** 打开，命令：`pop intro` / `pop outro` / `champ` | 播放开场/结尾动画；`champ` = `justWonTheGame=true` 并刷新主菜单（直接通关） | `Config.majomParade` |
| 6 | 命令行 / 引擎参数 | 启动参数 `-compile_all` → `System.compileAll(".\\")` 后退出 | 全量重编译 Java（开发用） | 无 |
| 7 | 引擎参数命令表（`LASR.exe` 字符串） | `.scp`/参数脚本 | `cheat_control`、`ai_cheat`、`camera_nocollision`、`notify`、`ai_info`、`osd_*`、`turn/velfactor/steerspeed/steerhelp/damagedef` 等 —— 引擎侧调试开关 | 需改数据文件 |

### `Config.majomParade` 的实际情况（重要）

- 全容器里对它的写只有两处，**都是写 `false`**：`Config.<clinit>` 与 `Config.load()`。
- 它不是存档字段：`Config.save/load` 的字段序里没有它，改 `save/game/options` 打不开。
- `LASR.exe` 里**没有** `majomParade` 这个字符串（只有 `kismajomCheck` / `cheat_control` / `ai_cheat`）。
- 结论：**零售版正常流程下第 3–5 类入口进不去**，属开发期残留；能用的只有排行榜三码、赛中两个组合键、
  `-compile_all` 与引擎参数。

## 4. 复现命令

```bash
# 三条序列 + 效果
grep -n "this.cheat\[0\] ==" out_pseudo/java/classes/game/frontend/PDAPopup.java
# 字节码级核对（应出现 (0,0)(1,8)(2,4)(3,5) / (0,0)(1,8)(2,7)(3,8) / (0,0)(1,8)(2,1)(3,9)）
./.capenv/Scripts/python.exe - <<'EOF'
import re;from pathlib import Path
t=Path('out_tasm/java/classes/game/frontend/PDAPopup.class.tasm').read_text(encoding='utf-8',errors='replace')
b=re.search(r'^REC 4 \d+\n(.*?)^END',t,re.S|re.M).group(1)
print(re.findall(r'INT_LITERAL (\d+)\s*\n\s*FIELD_REF_QUICK 375\s*\n\s*ARRAY_ACCESS\s*\n\s*INT_LITERAL (\d+)',b))
EOF
# 赛中两个组合键
grep -n "pageUpPressed && this.pageDnPressed" out_pseudo/java/classes/game/Track.java
grep -n "insertPressed && this.deletePressed" out_pseudo/java/classes/game/Init.java
# majomParade 只被写 false
grep -rn "majomParade = " out_pseudo/java/classes/
```

## 5. 让零售版用上调试菜单：`majomParade` 补丁（已构建 + 已在沙箱验证）

### 5.1 为什么两处都要改

`java.util.Config.majomParade`（静态 int，**slot 8**）在发行版容器里被写 `false` 两次：

| 位置 | 方法 | 字节码形态 |
|---|---|---|
| 字段初始化 | `Config.<clinit>`（`REC 0`） | `BOOL_LITERAL 0` + `PUTFIELD_STATIC 224` |
| 启动重置块 | `Config.init()`（`REC 1`，`if (Config.master) { … }`） | 同上 |

`Config.master` 在发行版里 = **1**（沙箱 dump 实证），所以 `init()` 每次启动都会把标志重新清零 ⇒
只改 clinit 会在启动时被抹掉，必须两处都改。

### 5.2 补丁内容：2 个字节

```
Config.class  19166 B  md5 da86ea9cf6454e3c94074683edb1442b
  offset 0x2a7f : 00 -> 01      (clinit 里 BOOL_LITERAL 的载荷)
  offset 0x310b : 00 -> 01      (init 里同一处)
补丁后        md5 4e00c4a134b62d4c5ecffcedd01aa091     长度不变
```
生成器 `tools/majom_patch.py`：`.class → IR → .tasm 文本 → 替换 → 汇编 → 断言"只有这 2 字节变、且都是 00→01"
→ 用游戏自己的 `flzd_pack` 重新压缩该条目 → 重建 zip（其余 304 条目必须与发行版逐字节相同）。

产物：`scratch/classes_majom.zip`（304 条目，顺序与发行版一致）。

### 5.3 沙箱验证（tufa_vm 真跑，不是静态断言）

```
./vm/tufa_vm.exe --cp scratch/corpus_majom --index vm/class_index.txt     --clinit java.util.Config --file-root C:/Games/LASR --dump
```
| 语料 | `java.util.Config  majomParade  I  8` 的值 |
|---|---|
| `extracted`（发行版） | **0** |
| `scratch/corpus_majom`（补丁后） | **1** |

再挂 `UNI_DRIVERS=java.util.Config.load` / `java.util.Config.init` 两种驱动跑，值都保持 **1**
（`steps=572 errors=0 native_stubs=6`）⇒ 启动路径上不会被重置。

### 5.4 安装 / 回滚

```bash
# 安装（先确认备份在）
cp C:/Games/LASR/java/classes.zip C:/Games/LASR/java/classes.zip.modbak   # 二次保险
cp scratch/classes_majom.zip      C:/Games/LASR/java/classes.zip
# 回滚
cp C:/Games/LASR/java/classes.zip.bak C:/Games/LASR/java/classes.zip
```

### 5.5 打开后能看到什么

- 车库（mainMenu）：多出 **Cheat 1 / Cheat 2 / Increase rank / Decrease rank / Win-Lose BastardRace** 五个按钮
- 赛中 ESC 菜单（仅单人）：**win / loose** 两个调试按钮
- 任意界面按 **右 Win 菜单键（RCDIK_RMENU）**：弹出 `ConsoleWindow`，命令 `pop intro` / `pop outro` / `champ`
- 附带副作用（同一开关的其它读者）：
  - `CardComponent`：零件卡上叠加显示 prestige 数值
  - `ItemlistComponent`：日志打印 `<undefined>` 零件的 id（只在日志里）
  - `ChallengeOffer.createAICar`：对手车列表为空时改走"全 VID 随机"分支（正常对局不会走到）

### 5.6 已知边界

- 只改 `classes.zip`，不碰 exe、不驻留内存；`classes.zip.bak` 随时回滚。
- 该开关是"开发模式"，游戏没有反作弊读取它（`cheatPushed` 无人消费），所以改它不会触发任何惩罚逻辑。
- 若想要"按需开关"而不是常开，需要在 `Init.handleHotkeyEvent` 里插入一条 `Config.majomParade = !Config.majomParade`
  的分支 —— 那是**插入指令**（会移动后续跳转目标），比改常量风险高，需要单独做并跑全量往返闸门。

## 6. 控制台按键改绑（`/`~` 键）

### 6.1 原按键其实是"右 Alt"，不是 Menu 键

`Init.<init>` 里控制台热键用的是 `java.io.Input.RCDIK_RMENU`，而该常量的值是 **184 = 0xB8**
—— DirectInput 里 0xB8 是 **DIK_RALT（右 Alt）**，不是 `DIK_APPS`(0xDD) / `DIK_RMENU`(0x9D)。
所以老键盘上"没有菜单键"其实不影响：**按右 Alt 同样能开控制台**（名字与编号不符，是开发期笔误级别的遗留）。

### 6.2 改绑做法：只改一个字节

`java.io.Input.<clinit>`：`INT_LITERAL 184` + `PUTFIELD_STATIC 651`

| | 值 |
|---|---|
| 原 | `184`（0xB8 = 右 Alt） |
| 新 | `41`（0x29 = DIK_GRAVE，即 `~` / `` ` `` 那个物理键；不受 Shift 影响） |

**为什么不直接改 `Init` 里的引用**：`Init` 的 CONS 池里只有 SYSRQ/RMENU/INSERT/DELETE/NUMPADSTAR 五个
`RCDIK_*` 引用，没有 GRAVE，要加得往 CONS 里插两条表项；而 `RCDIK_RMENU` 全语料**只被这一处热键读取**
（`grep -rn RCDIK_RMENU` 仅 `Init.java:38` + 定义本身），所以直接改它的值是最小改动、不碰结构。

### 6.3 已验证

```
./vm/tufa_vm.exe --cp scratch/corpus_mod --index vm/class_index.txt --clinit java.io.Input --dump
java.io.Input  RCDIK_RMENU  I  143  <41>        # 补丁后；语料原值 184
```
字节级：`Input.class 16041 B  md5 2aa4e39… -> b5a9bd6…`，**只改 1 字节**（0x35da：0xB8 → 0x29）。
`Config.class` 那两处不变（md5 4e00c4a…）。

### 6.4 产物

`scratch/classes_debugmod.zip` —— 304 条目，顺序与发行版一致，**302 条未改动条目重新打包后与发行版逐字节相同**，
两条改动条目解压后与我们补丁字节相等，整包 md5 `f37d17e6f7a7d45ef6d510653c381bce`。

生成器 `tools/mod_debug.py`（`--console-key N` 可换键）：
```
41 = ~ / backtick      43 = \        57 = space
68 = F10              88 = F12      59 = F1
```
