# 调试菜单补丁（零售版也能进开发菜单）

零售版《L.A. Street Racing》里有一套**开发期调试菜单**，被一个静态开关 `java.util.Config.majomParade`
挡着 —— 而这个开关在发行版里从未被置真。本目录给你一个**最小改动**的补丁：替换 `java/classes.zip`，
不碰 exe、不驻留内存、随时可回滚。

> 本仓库**不包含**任何游戏文件。补丁是把你自己机器上的 `java/classes.zip` 读出来、改 3 个字节、
> 再用游戏自己的 FLZD 压缩例程重新打包。跑一次脚本就得到成品。

## 打上补丁后能做什么

| 入口 | 位置 | 作用 |
|---|---|---|
| **Cheat 1 / Cheat 2** | 车库主菜单 | 当前车全部零件 / 全部车辆入库 |
| **Increase rank / Decrease rank** | 车库主菜单 | 按对手表直接改写 prestige（声望/排名） |
| **Win / Lose BastardRace** | 车库主菜单 | 翻转 `bastardRaceFinished`（下一次"混蛋赛"强制胜/负） |
| **win / loose** | 赛中 ESC 菜单（仅单人） | `letMeWin()` / `letMeLose()` 直接结算 |
| **控制台** | 任意界面按 `~` | 命令 `pop intro` / `pop outro` / `champ`（champ = 直接通关） |

另外 `CardComponent` 会在零件卡上叠加显示 prestige 数值，`ItemlistComponent` 会在日志里打印
`<undefined>` 零件的 id —— 都是同一个开关的其它读者，无害。

## 补丁内容：3 个字节

| 类 | 位置 | 改动 | 为什么 |
|---|---|---|---|
| `util/Config.class` | `Config.<clinit>`（`BOOL_LITERAL 0` + `PUTFIELD_STATIC 224`） | `0 → 1` | 打开 `majomParade` |
| `util/Config.class` | `Config.init()`（同一处） | `0 → 1` | `Config.master` 在发行版里是 **1**，`init()` 每次启动都会把它清回 false ⇒ 不改这里会被立刻抹掉 |
| `io/Input.class` | `Input.<clinit>`（`INT_LITERAL 184` + `PUTFIELD_STATIC 651`） | `184 → 41` | 控制台热键读的是 `RCDIK_RMENU`，但它的值是 184 = **右 Alt**（名字与编号不符的遗留）；41 = `~` 键。`--console-key 0` 可保持原键 |

## 用法

```bash
pip install unicorn            # 用游戏自己的 FLZD 例程打包，需要模拟执行
python tools/make_debugmod.py --game "C:\Games\LASR" --out classes_debugmod.zip
# 换键：--console-key 68 (F10) / 88 (F12) / 43 (\) / 59 (F1) / 57 (space) / 0 (不改)
```

安装 / 回滚：

```bash
cp "C:/Games/LASR/java/classes.zip" "C:/Games/LASR/java/classes.zip.bak"   # 只在还没有备份时
cp classes_debugmod.zip "C:/Games/LASR/java/classes.zip"
# 回滚
cp "C:/Games/LASR/java/classes.zip.bak" "C:/Games/LASR/java/classes.zip"
```

## 验证过的部分（可复现）

- 脚本从**原始** `classes.zip` 读出的两个类，补丁字节与我们手工核对过的完全一致：
  `Config.class` md5 `da86ea9c… → 4e00c4a1…`（2 字节）、`Input.class` md5 `2aa4e394… → b5a9bd61…`（1 字节）。
- 重打包后 **302/302** 未改动条目与发行版**逐字节相同**，整包 md5 `f37d17e6f7a7d45ef6d510653c381bce`（可复现）。
- 沙箱（本项目自研的 TUFA 解释器 `vm/tufa_vm.c`）真跑：`Config.majomParade` 从 `0` 变 `1`，
  再挂 `Config.load()` / `Config.init()` 两种驱动都保持 `1`；`Input.RCDIK_RMENU` 读到 `41`。
- **实机**：装进 `C:\Games\LASR\java\classes.zip` 后，车库多出五个调试按钮、赛中菜单多出 win/loose、
  按 `~` 弹出控制台，均正常；跑完一局、回主菜单无异常。

## 风险与边界

- 改的是**核心共享包**（含 `IVehicle` 等）。务必先备份；坏包的表现是游戏起不来，回滚即恢复。
- 这是"开发模式常开"，不是按键开关。要做成按一下切换需要往 `Init.handleHotkeyEvent` **插入**指令
  （会移动后续跳转目标），风险比改常量高一档，本仓库暂不提供。
- 调试按钮是开发期布局，可能会和正式 UI 有轻微重叠。
- 《L.A. Street Racing》的权利属于其权利人；本补丁只修改你自己机器上、你自己拥有的游戏副本。
