# 63 —— 力回馈（Force Feedback）全链：配置 → 原生参数 → DirectInput

> **状态：** 静态链已打通到 DirectInput 调用点。三个滑条 ↔ 原生全局 ↔ 使用点已定案（✓）；
> 每帧更新的**实际频率**与**三个效果槽的 GUID 归属**仍缺（✗，需实测/继续读码）。
> 全部地址基于 `C:\Games\LASR\LASR.exe`（ImageBase 0x400000）。

## 0. 一句话结论

游戏只用 **DirectInput 8 的常量力（constant force）+ 一个 Spring + 一个 Damper**，
力值 = `clamp(int(RealStrength × physicsForce), ±10000)`，**每帧由带节流的固定步长循环推送**；
初始化时把 `DIPROP_FFGAIN` 设成满值 10000 并 **关掉方向盘自身的自动回正**（`DIPROP_AUTOCENTER=0`）。
→ 手感"卡住/阶梯"来自"常量力 + 节流推送 + 上限硬饱和"，不是驱动或基座的问题。

---

## 1. 配置层（VM 侧，已解过）

`java.game.frontend.gameOptions`（✓ 见 `out_pseudo/java/classes/game/frontend/gameOptions.java`）：

| UI | Config 字段 | 滑条 → 值 | 默认 |
|---|---|---|---|
| REAL STRENGTH | `Config.FFB_strength` | contextValue(0..10) / 10 → 0..1 | 0.5 |
| EFFECTS STRENGTH | `Config.FFB_strength_effects` | contextValue(0..10) / 10 → 0..1 | 0.5 |
| EMULATED STRENGTH | `Config.FFB_strength_emulated` | contextValue(0..40) / 40 → 0..1 | 0 |
| FORCE FEEDBACK 开关 | `Config.ForceFeedBack` | 下拉 Off/On | 1 |
| （无 UI） | `Config.FFB_offset_damper` | ctrl 文件 version ≥ 4 才有 | 0 |

序列化在 `java.io.ControlSet`（`save/controls/active_control_set1`）：
`FFB_strength` / `FFB_strength_emulated`（version≥1）→ `FFB_strength_effects`（version≥2，
**version<2 时 = FFB_strength**）→ `steer_controller_range`（读到即弃，version≥3）→
`FFB_offset_damper`（version≥4）→ `steeringLock` / Joy_*（version≥5、6）。

## 2. 原生配置读取（✓ 新解）

`0x4332c0` 起是一个巨大的"设置键 → 原生全局"读取函数，键名直接来自 .rdata 字符串表
（`ForceFeedBack` / `FFFB_strength`(=类型前缀 `F` + `FFB_strength`，真正被引用的是 `0x6ebc18`) /
`FFB_strength_effects` / `FFB_offset_damper` / `;FFB_strength_emulated`）；
读取器：`0x642d30`(int) / `0x642e30`(float) / `0x643170`(float+默认值)。

| 键 | 原生全局 | 换算 | 证据 |
|---|---|---|---|
| `ForceFeedBack` | `[0x744710]` | int 0/1 | `0x43330c`；使用点 `0x56d752`、`0x56dc30`（=0 则整段 FFB 更新被跳过） |
| `FFB_strength` | `[0x744714]` | ×**10000.0** | `0x43331d`–`0x433343`（常量 `0x6ebc14`=10000.0） |
| `FFB_strength_effects` | `[0x744718]` | ×10000（缺键则 = `[0x744714]`） | `0x433339`–`0x433380` |
| `FFB_strength_emulated` | `[0x743a9c]` | ×**0.005**（常量 `0x6ebbcc`） | `0x4333b2`–`0x4333c3` |
| `FFB_offset_damper` | `[0x8b0e94]` | ×10000 | `0x433375`–`0x4333a4` |

**使用点（决定手感归谁）：**

- `[0x744714]`（Real）→ 只在 **输入设备层** `0x56b2×–0x56b8×`：每帧把力推给 DirectInput 的地方。
- `[0x744718]`（Effects）→ 只在 **效果创建/参数循环** `0x56d588 / 0x56d5f3 / 0x56d859`：
  给"附加效果"配幅度。
- `[0x743a9c]`（Emulated）→ 只在 **轴力/曲线层** `0x5612a8 / 0x5615ba … 0x561bde`
  （与 `Controller.setAxisForce` → `0x560f40` 同一族的每轴"仿真力"通路）。
- `[0x8b0e94]`（offset damper）→ `0x56b869`（设备层，与 Real 同一段）。

## 3. DirectInput 层（✓）

- 唯一 `DirectInput8Create` 调用：`0x56e324`（设备管理器初始化）。
- 设备回调 `0x56b030`：`EnumDevices(DI8DEVCLASS_GAMECTRL)` → 每设备结构 **0x1050 字节**，
  数组基址 `0x8a0570`、计数 `[0x8b0e80]`（上限 16）。
- `SetCooperativeLevel(hwnd, 6)`（`0x56bc02`/`0x56c0e7`）= **DISCL_NONEXCLUSIVE|DISCL_FOREGROUND**
  → 别的进程仍可同时打开同一设备（对外部 FFB 工具很关键）。
- 属性：`SetProperty(DIPROP_FFGAIN=7, 10000)`（`0x56bb5c`–`0x56bb88`，跟踪名 `di_setFFgain`）、
  `SetProperty(DIPROP_AUTOCENTER=9, 0)`（`0x56bbae`，**关掉基座自回正**）、
  `SetProperty(DIPROP_BUFFERSIZE=1, …)`（`0x56c0ea`）。
- 效果 GUID 表在 .rdata（`0x70489c` 起，dxguid 全表；另有未引用的副本 `0x30489c`）。
  **初始化创建 2 个效果**：`GUID_Spring`（`0x7048cc`，`0x56bd89`+`call [vt+0x48]` CreateEffect）、
  `GUID_Damper`（`0x7048bc`，`0x56bed9`/`0x56bf15`）；`GetEffectInfo`(0x124 结构) 用于探测能力。
  DIEFFECT：`dwSize=0x38`、`dwFlags=0x12`(OBJECTOFFSETS|CARTESIAN)、`dwDuration=0`(无限)、
  `dwSamplePeriod=0x2710`(10 ms) 或 0、`cbTypeSpecificParams=0x18`。
- 跟踪名（用于实测计数）：`di_createeffect / di_start / di_setproperty / di_setFFgain /
  di_unacquire / di_getFFstate / di_FFreset`（字符串 `0x6f68d4…`，计数块 `0x8b0a78`/`0x8b0a70`）。
- FFB 子对象 `[dev+0xb98]`：3 个效果槽 `+0x14 / +0x3c / +0x64`（步长 0x28），析构 `0x56bfe0`；
  索引 ≥3 的额外效果在 `0x56d730` 释放，由循环 `0x56d510–0x56d6f0` 按运行时 GUID 表 `0x8e7c60`
  （下标 ×0x10）创建，数量 `[0x74471c]`。

## 4. 每帧推送（✓ 结构 / ✗ 频率）

`0x56b240(dev, force, force2)` 是主推送函数（唯一调用点 `0x56dbbe`，来自
`push [car+0xd14]` → 车辆对象的力值）：

```
1) 时间闸门：now - [ffbObj] 经 [0x8b0a70] 换算后与 [dev+0x1c] 比较，
   未到间隔 → 直接 return 0（不推送）。                 ← 阶梯感来源
2) comiss 0, [0x744714]：Real=0 → 三个分量全 0（送零力）。
3) 单轴设备([dev+0xb94]==1)：x = RawForce × [0x744714]；
   |x| ≥ 10000(0x6ebc14) → 归一化 ×10000（硬饱和）；   ← 上限饱和
   [esp+0xc] = cvttss2si(x)（转 LONG）。
   双轴设备：magnitude = round(sqrt(x²+y²) × [0x744714])，另存两轴分量。
4) 清 0x38 字节 DIEFFECT：dwSize=0x38, dwFlags=0x12, dwDuration=0,
   cAxes=[dev+0xb94], rgdwAxes=&[esp+0x10], cbTypeSpecificParams=4,
   lpvTypeSpecificParams=&[esp+0x10]（= DICONSTANTFORCE.lMagnitude），
   dwStartDelay=0 → SetParameters(effect[0]=[ffbObj+0x14], …)。
```

其它同族推送函数（同样带时间闸门、`dwSamplePeriod=0x2710`）：
`0x56b430`（`[car+0xd2c]`、`[car+0xd34]` 两个分量，调用点 `0x56dbaa`）、`0x56b770`（`[car+0xd54]`）。
外层驱动循环 `0x56d940–0x56dc1b`：以 64 位时间戳 `[ffbObj+0x18/+0x1c]` 累加、
`while (累加时间 > 步长) { 推送; }` = **定步长追赶循环**。

物理侧的力合成在 `0x461xxx / 0x464xxx / 0x471xxx`，累加进车辆对象
`+0x263c / +0x2640 / +0x2644 / +0x2648 / +0x264c`，并读一组力学参数全局
`0x742818 / 0x74281c / 0x742820 / 0x742828 / 0x74282c / 0x742830 / 0x742838 / 0x742840 /
0x74284c / 0x742850 / 0x742854`（多在 `0x4717xx`–`0x4722xx`，含 `sin/cos/atan` 取角）。
数值单位侧常量：`0x6ec184`=127（轴归一化，对应 `getInput()/128`）、`0x6ec6a0`=9.5493(=60/2π)、
`0x6ec6a4`=7000(rpm)、`0x6ea5ec`=deg→rad、`0x6ebb38`=0.2778(km/h→m/s)、`0x6ea6d4`=0.5、
`0x6ebd94`=100。

## 5. 未决（✗ 需要上机或继续读码）

1. **实际推送频率**：`[dev+0x1c]` 的运行时值 & 追赶循环步长 → 决定阶梯台阶大小。
   实测法：dinput8 代理 DLL 记录 `IDirectInputEffect::SetParameters` 的时间戳/幅度，
   或读 `0x8b0a70`/`0x8b0a78` 跟踪计数块。
2. **三个效果槽的 GUID 归属**：`+0x14` 收到的是 4 字节类型参数（= 常量力），
   但初始化里看到 Spring/Damper 的 CreateEffect → 需确认 `+0x14` 是否另有独立创建点。
3. **Emulated 的语义**：已定位到"每轴仿真力"层，但"给谁用/什么条件下生效"
   （无 FFB 设备回退？虚拟轴？）未证。
4. `[dev+0xb94]`（cAxes 1/2）由哪支代码按设备能力赋值。
5. 效果创建循环的 GUID 表 `0x8e7c60` 由谁填充（运行时数据，文件里未初始化）。

## 6. 对外部力回馈（替代方案）的直接影响

- 游戏 **非独占** 打开设备 + `ForceFeedBack=0` 时整段 FFB 更新被跳过 →
  "关掉游戏内 FFB + 外部进程自己出力" 在机制上**可行**。
- 力值数据源有两条：① 直接读车辆对象的 FFB 累加槽 `+0x263c…+0x264c` 与
  `+0xd14/+0xd2c/+0xd34/+0xd54`（游戏自己算好的各通道力）；
  ② 用 `docs/44`、`docs/45` 已解的刚体/悬挂槽位自算（`getWheelSlip*`、`getWheelLoad`、
  `getLateralG` 等）。
- 另一条更省事的路：**dinput8 代理**（不改内存）——拦 `CreateEffect/SetParameters`，
  把 30–60 Hz 的阶梯常量力**插值/升频**到几百 Hz，顺手加滑移/载荷失真的甜点力。

## 7. 记录器与"暂停键"（本轮新增）

### 7.1 帧率关系（✓ 静态）

力回馈的驱动入口是 **`0x561d10(deltaSeconds)`**——每帧调用一次（唯一调用点 `0x43623f`，
在引擎帧更新代码里，压栈前还把 delta 做过上下限钳位）。它：
`delta*1000 → [0x890E04]`（**游戏自己算的"本帧毫秒数"**）、`call 0x56e490 → [0x890E08]`、
`[0x890E18]++`（帧计数），然后走对象链表调 `0x561520`，最后 `call 0x56dfd0`
（→ 每设备 `0x56d930` → 推流 `0x56b240/0x56b430/0x56b770`）。

⇒ **帧率是推送频率的上限**：低帧率时采样点变稀、台阶变粗；但台阶的下限由 `0x56b240`
内部那道时间闸门 + 定步长追赶循环（`0x56d940–0x56dc1b`）决定，不是"帧率越低推得越密"。
实测手段：代理日志里每条 `setparams` 都带 `frame_ms`，且 `state` 行给出主循环间隔。

### 7.2 记录器（`tools/ffb_proxy/`，32 位，以 `d3dx9_30.dll` 身份进进程）

**文件名必须避开 `dinput8.dll`** —— 原因见 §8（会把系统的力回馈代理 ffbroker 绑到我们身上）。
现在的做法：代理由游戏自己静态导入的 `d3dx9_30.dll` 进进程（332 个导出全部用 naked `jmp` 桩搬过来，
指针运行时从 `SysWOW64\d3dx9_30.dll` 解析），然后改写游戏对 `DirectInput8Create` 的入口
（首选代码级改写调用点 `0x5FAD30`，字节校验 `FF 25 18 70 6E 00` 通过才动；兜底线程等 IAT 绑好后改 IAT 槽）。

vtable 挂钩 + 转发，不改游戏代码、不写内存（只读 `+0x490E04` 帧时间）。记录
`createfx / setparams / start / stop / download / setprop / ffcmd / getffstate / enumeffects /
setdataformat / devinfo / state / btn / dicreate / createdevice / iat_patch / callsite_patch / d3dx9_resolve`。
判读脚本 `tools/ffb_log_analyze.py`。

**本机自检实测 ✓**（详见 §8.3）：332/332 导出解析、`iat_patch` 命中、`dicreate hr=0x0`、
枚举到 `MOZA R9 Base`（axes=8 buttons=128 povs=1 ff=1）、**`CreateEffect hr=0x0`**、
`SetDataFormat` 80 字节摇杆/256 字节键盘区分正确、**ESC 注入通路 OK**（读回 `ESC=0x80`）。
✗ 待上机：游戏内 `setparams` 的真实频率/幅度分布；暂停键端到端生效；`callsite_patch` 在 LASR.exe 上的实际命中。

### 7.3 暂停键：游戏里的真相（✓）

暂停不是"某个绑定项"，而是 **虚拟轴 `AXIS_MENU(36)` + `Hotkey.VIRTUAL`**：

- 默认表：`ControlSet.define(1, AXIS_MENU, KEYBOARD, RCDIK_ESCAPE, 0,1, 0,1, 0)`
  —— group=1（常驻组）、设备=键盘、键码=ESC。
- 监听方：`IngameMenu` / `popupOkCancel` / `ResultsWindow` / **`Track`（比赛内暂停）** 全部
  `new Hotkey(AXIS_MENU, Input.VIRTUAL, ...)` ⇒ **只要控制集里 AXIS_MENU 有任意一条映射，
  按那个输入就暂停**。
- 设置界面 `controlOptions.java` 里根本没有 AXIS_MENU（✓ 与"游戏不支持绑定暂停"一致）。

两条解法：

1. **代理注入**（已实现）：把方向盘按钮注入成 DInput 键盘的 ESC（见 7.2）。
2. **给控制集加一条映射**（更原生，无后台进程）：
   `tools/ctl_edit.py add-pause --device-idx 2 --button N`
   —— 在 `save/controls/active_control_set1` 里追加 `group=1, vaxis=36(AXIS_MENU),
   device=<方向盘在文件设备表里的下标>, axis=N`。格式与往返一致性已实测
   （`ctl_edit.py verify` → 逐字节相同 ✓）。注意：动过文件后别在游戏里进 "Control Options"
   再改别的绑定（游戏会按自己加载的内容重写该文件，通常不会丢这条，但先备份）。



## 8. 力回馈全失效的根因：代理绝不能占用 `dinput8.dll` 这个名字（✓ 实测定案）

### 8.1 现象与排查

代理（当时叫 `dinput8.dll`）放进游戏目录后，**游戏里一点力回馈都没有了**。同一个自检程序：

| 情形 | `CreateEffect` 结果 |
|---|---|
| 系统真 `C:\Windows\SysWOW64\dinput8.dll` | 全部 `S_OK (0x0)` |
| 我们的代理（文件名 `dinput8.dll`） | 全部 `DIERR_INVALIDPARAM (0x80070057)` |
| 我们的挂钩 `[hooks] DevMisc=0 FFB=0 Effect=0`（整组关掉） | 仍然 `DIERR_INVALIDPARAM` ⇒ **不是挂钩的问题** |
| **同一份二进制，只把文件名改成 `lasrffb_test.dll`** | **立刻全部 `S_OK`** ⇒ 根因 = 模块名 |

### 8.2 机制

`C:\Windows\SysWOW64fbroker.dll` 是 Windows 自己的**力回馈代理**，它**也 import 了
`DINPUT8.dll`**。Windows 加载器解析导入时是按"模块基名"找已加载模块，**先加载的同名模块胜出**：
我们的代理既然占了 `dinput8.dll` 这个名字（且比真身先加载），ffbroker 就绑到我们身上，它的力回馈
通路随之失效 ⇒ 游戏每次 `CreateEffect` 都 `DIERR_INVALIDPARAM`。

> 顺带订正：先前把自检里 Moza `CreateEffect = E_INVALIDARG` 归因为"后台进程没有前台权限"，**错**，
> 就是这个名字冲突。现在自检即使在后台控制台进程里，`CreateEffect` 也是 `S_OK`。

### 8.3 新加载方式与实测证据

代理改名 **`d3dx9_30.dll`**（游戏自己静态导入它的 39 个函数）：

1. 真 `d3dx9_30.dll` 的 **332 个导出全部搬过来**：每个是一个 `naked jmp` 桩，指针在 `DllMain`
   里从系统副本解析（`SysWOW64\d3dx9_30.dll`）。两份文件 md5 完全相同
   （`e415862612e65f10d7d888443ecd7594`）⇒ 行为完全等价。
2. 入口挂钩两条：① 代码级改写游戏调用点 `0x5FAD30`（原 `FF 25 18 70 6E 00` = `jmp [0x6E7018]`，
   字节校验通过才改，否则只记 `callsite_skip`）；② 兜底线程等加载器把 IAT 绑好后改写 IAT 槽。
3. ffbroker 照旧绑到系统的真 dinput8.dll ⇒ 力回馈恢复正常。

实测（`tools/ffb_proxy/build/iat_test.exe`，一个像游戏那样**静态 import `DirectInput8Create`** 的测试程序）：

| 证据 | 日志/输出 |
|---|---|
| 332 个 d3dx9 导出解析 | `d3dx9_resolve path=C:\Windows\system32\d3dx9_30.dll ok=332/332` |
| IAT 被改写 | `iat_patch module=DINPUT8.dll rva=0x15000 orig=0x56D96FA0 ours=0x56D651D0` |
| 调用走我们的入口 | `dicreate hr=0x0 ver=0x800` |
| 设备创建 | `createdevice guid={7F1848C0-…} hr=0x0` |
| **力回馈效果创建** | **`createfx guid=ConstantForce hr=0x0 size=56 flags=0x12`** |
| 成品（游戏目录那份）复测 | 同上全过 + `boot dir=C:\Games\LASR pausBtn=37`（读到用户 ini 的按钮号） |

产物：`tools/ffb_proxy/{lasr_dinput8.cpp, d3dx9_30_stubs.cpp, d3dx9_30.def, gen_d3dx9_stubs.py,
iat_test.cpp, selftest.cpp, build.bat, install.bat, uninstall.bat, lasr_ffb.ini, README.md}`，
编译产物 `build/d3dx9_30.dll`。`install.bat` 会**自动删掉旧版留下的 `dinput8.dll`**
并把游戏原本的 `d3dx9_30.dll` 备份成 `d3dx9_30.dll.bak`（`uninstall.bat` 还原）。

## 9. 实测：力回馈推送节奏与那个 ~1 秒时间闸门（✓ 实机日志 + 静态定位，2026-10-02）

### 9.1 用户实机日志（90 秒会话，7024 次 setparams）

代理在真游戏里挂钩成功：`callsite_patch,rva=0x1FAD30`（字节校验通过）+ `iat_patch,rva=0x2E7018` ✓，
力回馈恢复 ✓。同一会话按效果槽统计：

| 槽（创建序） | 效果 | 参数块 | 推送次数 | 间隔中位 | p10 | 速率 |
|---|---|---|---|---|---|---|
| 1 | **ConstantForce** | cb=4（DICONSTANTFORCE） | 36 | **1178 ms** | 1005 ms | **0.8/s** |
| 2 | **Spring** | cb=24（DICONDITION：offset/系数/饱和） | 52 | **1002 ms** | 1000 ms | **1.0/s** |
| 3 | **Damper** | cb=24 | 24 | 2058 ms | 1081 ms | 0.5/s |
| 7 | SawtoothUp | cb=16（DIPERIODIC） | 2152 | 17.8 ms | 4.4 ms | 56/s |
| 14 | Sine | cb=16 | 4746 | 4.4 ms | 2.0 ms | 226/s |

观测到的值形态：
* Spring：`offset|10000|10000|10000|10000|0`，offset 从 −10 平滑走到 −2792 再回正 ⇒ 方向盘路感的"目标"。
* ConstantForce：`539 → 3984 → 2914 → 371 → 2095 → 150 …`（大跳变）。
* 振动两路：Sine `幅度|0|0|周期µs`（9884→10140 µs ≈ 101→98.6 Hz）、SawtoothUp 周期 152→124 ms。

⇒ **转向力那三路（常量力/弹簧/阻尼）实测 ~1 Hz 才刷新一次，振动那两路是每几帧一次。**
这就是"卡着不动、然后突然跳一下"的定量来源；也解释了为什么 REAL 拉到任何值都卡 —— 与增益无关，
是刷新率本身。另外 `flags=0x140`（只更类型参数+方向，不含 gain）⇒ 日志里 `gain=0` 不等于静音；
FFGAIN 只在设备初始化时设过一次 10000 ✓、AUTOCENTER=0 ✓（与 §3 静态结论一致）。

### 9.2 闸门在哪（✓ 静态定位，与实测吻合）

`0x56B240(dev, force)` 开头（逐字节读出的原指令）：

```
56B267  QueryPerformanceCounter(&now)
56B272  fild [now] / fild [ffbsub+0]          ; −上次推送时刻（64 位 QPC 存在 FFB 子对象 +0）
56B278  mov  ecx, [dev+0x1C]                  ; ★ 闸门值
56B27F  fild [0x8b0a70]                       ; QPC 频率
56B285  fdivp                                 ; ΔQPC / 频率 = 秒
56B287  fmul [0x6e7684] (=1000.0)             ; → 毫秒
56B28D  fild [dev+0x1C] … fmul [0x6e7df0] (=0.001)   ; 闸门换算成毫秒 ⇒ [dev+0x1C] 单位是**微秒**
56B29E  fcompi st(1) / fstp
56B2A2  jbe  0x56B2AD                         ; ← 字节 76 09："闸门 ≤ 已过时间"才去推，否则 return 0
```

⇒ `[dev+0x1C]` ≈ 1 000 000 µs = **1 秒**（与实测 1000–1002 ms 吻合）。

三路推流由同一段逐帧代码调用（`calls_to`：`0x56db7f→0x56b770`、`0x56dbaa→0x56b430`、
`0x56dbbe→0x56b240`），力值取自车辆对象（`[car+0xd2c]`、`[car+0xd34]` 等）⇒ **力值本身是每帧算的，
被闸门卡住的只是"推送"这一步**（三路里只有常量力这一路带这个闸门；另两路逐帧推、靠"值变没变"自然限频）。

### 9.3 修法：把闸门那个字节改成无条件跳（代理内置，可关）

`tools/ffb_proxy` 的 `[patch] UngateConstant=1`（默认开）在 `DllMain` 里把
`LASR.exe+0x1EB2A2` 的 **`76 09`（jbe）改成 `EB 09`（jmp）**，只改内存、游戏文件一个字不动，
拔掉代理即恢复；日志会记 `ffbgate_patch,rva=0x1EB2A2`（字节不符则记 `ffbgate_skip`）。
之后游戏会用**它自己每帧算出的真值**推常量力 —— 不是插值、也不是外部力模型。

✗ 待实测确认两点：① 推送频率是否真的升到帧率级；② 游戏每帧的力值是否真在变（若值本身 1 秒才更新，
还得往上游找）。ini 里设 `UngateConstant=0` 即可 A/B 对比手感。

### 9.4 日志体积

13 MB 的日志里绝大部分是 `state`（每帧多次 `GetDeviceState`）与 `acquire`（游戏疯狂 Acquire）。
现在默认 `[log] Noise=0`（不记 acquire/unacquire/devicedata）+ `StateMs=500`（state 行最小间隔 500 ms）
⇒ 一次会话降到几十 KB 级；要看原始节奏时把 `Noise=1` / `StateMs=0` 打开。

## 10. 第二轮实测（探针 + 空闲清零）与"冲线卡力"修复（2026-10-02）

### 10.1 闸门补丁确认生效（✓ 实测）

用户实机日志：`ffbgate_patch,rva=0x16B2A2,76_09_to_EB_09` ✓。常量力推送间隔中位 **1078 ms → 41 ms（快 26 倍）**，
且每次推送的值都在变（值变中位 = 推送中位）。手感"确实变了"，但**仍有卡顿、且卡顿频率不固定** ⇒
说明除闸门外还有一层限制（振动流是 4 ms 级，常量力仍是 24 Hz 级），后续自出力方案会绕开它。

### 10.2 探针（`[ffb] Probe=1`，✓ 实测命中）

把三条推流的调用点（`0x56DBBE→0x56B240`、`0x56DBAA→0x56B430`、`0x56DB7F→0x56B770`，E8 调用、字节校验后改写）
改成我们的观察函数。实测日志：`probe_patch,tag=c240/c430/c770` ✓ + `probe` 行（每路 10 条/秒）给出
**设备对象指针、三条通道值（+0xd14/+0xd2c/+0xd34/+0xd54）、[dev+0x1c] 闸门、[dev+0xb94/+0xb90] 标志**，
并 dump 设备对象（0x1000 字节，含 +0xb98 FFB 子对象与 +0xd14 通道区）与 FFB 子对象（0x200 字节）+ 指针扫描。
⇒ 这是"自出力"阶段 2 找车对象/字段的入口数据。

### 10.3 "冲线后力不清除"的原版 bug 与修法（✓ 本地验证）

日志证据：比赛末尾游戏把效果 `download/start` 一遍（`dur=0xFFFFFFFF` 无限时长）之后就再没有 `setparams`
⇒ 力永久保持（切出窗口 = 设备 unacquire 才消失）✓ 与用户描述一致。

**第一版修法失败**：`ZeroAllEffects()` 只传 `DIEP_TYPESPECIFICPARAMS` → 驱动一律返回
`0x80070057 (E_INVALIDARG)`（日志里 22 条 `idle_zero` 全是这个）⇒ 力照旧。

**修法（已本地验证）**：静音必须**照抄游戏最后一次 SetParameters 的完整形状**——
`pe->dwFlags`、`dwSamplePeriod`、`dwGain`、`cAxes`+`rgdwAxes`、`rglDirection`、`cbTypeSpecificParams`，
只把类型参数缓冲整块置 0，然后用**游戏当时的调用 flags** 提交。实测 `hr=0x3`（成功码，非错误）✓。
另外两条必须做对的事：
1. **效果销毁要清理注册表**：挂钩效果 vtable 的 `Release`（槽 2），引用计数归零即移除条目，否则悬空指针
   （游戏在菜单/换车/冲线时会反复 create/stop 效果，日志里 createfx 60 次）。
2. **自己发出的 SetParameters 不要再回灌空闲计时**（否则看门狗每 500 ms 空转一次）——用 `g_muting` 标志隔离。

`[fix] ZeroOnIdle=1 / IdleZeroMs=500`：游戏静默超过 500 ms ⇒ 把 3 秒内还在推力的效果参数整块清零
⇒ 冲线/暂停/菜单里方向盘自动松开；比赛恢复时游戏自己的推送会覆盖回来（不需要显式恢复）。

✗ 待实测：冲线时手感是否真的松开；探针 dump 是否够定位车对象。

## 11. 下一步：自出力（Custom FFB）设计草案（2026-10-03）

### 11.1 为什么必须自出力（本轮封板的证据）

* 原版把转向力推成"常量力 + 弹簧偏移"，刷新率被它自己的时间闸门限制，且**冲线后仍持续输出非零力**。
* 我们能把单个效果静音（三级兜底：按创建轴数只给类型参数 → 照抄游戏 DIEFFECT 形状 → `dwGain=0`；
  实测 `Spring/Damper hr=0x0, cb=48, useAx=2` ✓，暂停时方向盘确实松开 ✓），但：
  * 设备级 `DISFFC_STOPALL` 在**非独占**获取下被拒（`hr=0x80040205`）；
  * 效果的 `Start/Stop` 同样被拒；
  * 冲线时游戏**并不静默**（仍 ~24 Hz 推力）⇒ "静默才介入"的触发条件永远不成立。
  ⇒ 给原版打补丁无法解决"冲线卡死"，只有"力由我们自己算"才能让"比赛结束 = 力归零"必然成立。

### 11.2 形态：做在代理进程内（不是独立进程）

理由：① 已经在进程内、已经能挂钩游戏自己的力回馈函数与每帧时机；② 只有同进程才能干净地**压掉游戏自己的力**
（不转发它的 `SetParameters`，或把它的效果增益压 0）；③ 不需要跨进程找指针。

### 11.3 阶段划分

| 阶段 | 内容 | 状态 |
|---|---|---|
| 1 | 探针：抓设备对象/FFB 子对象/每帧现场（找车对象入口） | ✓ 已交付（`[ffb] Probe=1`） |
| 2 | 定车辆状态字段（速度/转向/RPM/横向加速度/出界/碰撞） | ⏳ 用 10 Hz 密集采样 + 用户录像 (`tools/replay_dynamics.py` 解出 298 帧真值) 逐点对齐 |
| 3 | 自定义力模型：常量力=转向扭矩、弹簧=回正、Sine=转速震动、Damper=阻尼 | 待阶段 2 |
| 4 | ini 切换"游戏力 / 自出力"、比赛结束归零（顺带修掉冲线卡死）、打包 | 待阶段 3 |

### 11.4 阶段 2 的对表方法（已验证有效）

1. 用户跑一段并存一份 `.rpl` 录像；
2. `tools/replay_dynamics.py <rpl> --json out.json` → 逐帧 speed / yaw_rate / lat_accel / 轮速 / 前向轴；
   （scratch 里的 `rpl_vs_mem.py` / `rpl_align.py` / `rpl_slide.py` 是做"内存字段↔录像"对齐的脚手架）
3. 代理侧用 **10 Hz 密集采样**（`dense,idx=N,...`，只在有 FFB 推送的 3 s 内记）拿到同一时段的候选字段数值；
4. 按录像时间轴（`time_s`）逐点相关 ⇒ 只承认 `|r| ≥ 0.95` 且多个字段落在**同一对齐窗口**的结论。
   ✗ 教训：只按"值域相近 + 滑窗相关 0.8"就认字段是不行的（本轮试过，各字段窗口互不一致 = 伪相关）。

### 11.5 已知的坑（写进 skill 了）

* 力值通道在 **FFB 子对象 `[dev+0xb98]`**（+0xd14/+0xd2c/+0xd34/+0xd54），**不在设备对象上**。
* 探针/挂钩都要 naked 转发或字节校验 + SEH，且别用 `
` 之类转义在 heredoc 里拼 C 代码。
* 游戏一次会话 create/stop 效果几十次 ⇒ 效果注册表要跟 `Release` 同步。

## 12. 自出力 v1「接管输出」——已实测成功（2026-10-03）

### 12.1 为什么不是"另起炉灶"而是"接管"

原版推流的两个病根：① 转向三路（ConstantForce cb=4 / Spring cb=24 / Damper cb=24）被 `[dev+0x1c]`
的时间闸门限到 **~1 次/秒**（`[patch] UngateConstant` 只解了常量力那一条）；② 即使解了闸门，驱动侧仍是
**24 Hz 级的阶梯**，方向盘会有"一跳一跳"的颗粒感。⇒ 不需要知道车状态，只要**把输出节奏拿过来**：
游戏推来的类型参数只当**目标值**，由我们按固定频率 + 一阶平滑（可加增益/曲线）重新推给驱动。

### 12.2 实现（`lasr_dinput8.cpp`）

* 挂钩 `IDirectInputEffect::SetParameters`（槽 6）：`[ffb] Custom=1` 且类型参数长度 ∈ {4, 16, 48} 时，
  把参数拷进该效果的"目标"缓冲（`FxInfo::tgt`），**不转发**给真函数（返回 `DI_OK`），日志标 `custom_takeover`。
* `CustomThread` 按 `CustomHz` 推：幅值型（常量力 4 / 周期力 16）走 **增益 → 曲率 → 一阶平滑**；
  条件型（Spring/Damper 48 = 每轴 6 个 LONG：偏移/正系数/负系数/正饱和/负饱和/死区）走**逐元素平滑**
  （施加增益/曲率会破坏语义，所以只平滑）。
* **首帧直接对齐目标**（`primed`），避免从 0 渐入；目标超过 500 ms 未更新（游戏没在推）则撤下 `primed`
  并停止推流 —— 与 `[fix] ReleaseOnCfIdle` 的释放逻辑天然一致。
* 我们自己发出的 `SetParameters` 会再次进钩子（`g_muting` 隔离），默认**不记日志**（`[log] LogOwn=0`）。

### 12.3 参数与实测（用户 A/B）

| ini | 含义 | 默认 |
|---|---|---|
| `[ffb] Custom` | 0 = 原样转发（保底）；1 = 接管 | 0 |
| `CustomHz` | 我们的推送频率 | 250 |
| `CustomTau` | 一阶平滑时间常数 ms | 20 |
| `CustomGain` | 幅值增益 % | 100 |
| `CustomCurve` | 曲率（100 = 线性，越小中间段越细） | 100 |

用户实测（2026-10-03）：`Custom=1` 后"力回馈明显变顺了"；`CustomHz=250 / CustomTau=20`
"**完全感觉不到力回馈卡顿了**" ✓✓。日志核对：125 s 内 `custom_takeover` 12991 次、
**我们自己的推送 31594 次（= 253 次/秒，与 250 Hz 精确吻合 ✓）**、`release_on_cf_idle` 18 次
每次都在 ≤30 ms 内被 `release_clear,cf` 解除 ✓、无异常。

### 12.4 v1.1（同一轮）

* 接管扩到 **cb=48（Spring/Damper）** ⇒ 回正力的 1 Hz 阶梯也一起消掉。
* `[log] LogOwn=0`：不再记录我们自己的推送 ⇒ 125 s 的日志从 6.3 MB 降到 ~1/3。

### 12.5 仍未解（✗）

* **车辆状态字段仍未定案**：10 Hz 密集采样 vs 录像真值对表，50 ms 细搜最高 |r| 仅 0.68
  （伪相关陷阱踩过两次）⇒ v2「我们自己的物理模型 / 转速→震动」暂时不依赖它：
  转速可以从游戏振动效果的**周期**反推（日志实测 9884→10140 µs），幅度曲线靠 `CustomGain/Curve` 即可。
* 原版"冲线卡力"已由 `[fix] ReleaseOnCfIdle` 修掉 ✓（§11.1 的封板结论仍然成立：那是给游戏路打补丁的死角）。

## 13. v1.2 勘误与新增旋钮（2026-10-03）

### 13.1 勘误：Spring/Damper 的推送长度是 **cb=24**，不是 48

*  实测（用户日志的 cb 分布）：游戏推的转向三路 = `ConstantForce cb=4`(236 条) / `Sine·SawtoothUp cb=16`(8745) /
  **`Spring·Damper cb=24`(89)**；`cb=48` 只出现在**我们自己**的静音调用里（`idle_zero`，因为创建时 cAxes=2）。
*  ⇒ v1.1 写的 `cb ∈ {4,16,48}` 漏了 24 ⇒ **回正力其实一直没被接管**（用户上轮觉得"没问题"属安慰剂）。
  v1.2 已补上 `cb=24`（DICONDITION = `[偏移, 正系数, 负系数, 正饱和, 负饱和, 死区]`，走"逐元素平滑"分支）。
*  教训：**"创建时的轴数" ≠ "推送时的 cb"**，判断接管目标要按日志里真实的 `cb=` 分布来，别按创建参数推。

### 13.2 原版力本来就很小（这是"没有摩擦感"的根因）

实测游戏推送的幅值（满量程 ±10000）：

| 效果 | 样本 | 幅值 |
|---|---|---|
| Sine | `params=105\|0\|0\|14585` | **105**（≈1%） |
| SawtoothUp | `params=196\|0\|0\|133815` | **196**（≈2%） |
| ConstantForce | cb=4 单值 | 同量级（1~2%） |

⇒ 不是我们没有摩擦/力，是原版给的量就极小。**两个直接可用的放大手段**：

*  `[ffb] CustomGain`（幅值 ×N%）——因为输入先归一化到 ±1 再乘，所以 `CustomGain=500` 就是 5 倍，不会削顶。
*  `[ffb] CustomCurve`（越小越放大**小力**）——`CustomCurve=70` 时 1% 的输入 → `0.01^0.7 ≈ 4%`，
  即小力放大 4 倍、大力几乎不变 ⇒ "细节增强"。

### 13.3 v1.2 新增

| ini | 含义 | 默认 |
|---|---|---|
| `[ffb] DamperGain` | 摩擦（Damper 的正/负系数 ×N%） | 100 |
| `[ffb] SpringGain` | 回正（Spring 的正/负系数 ×N%） | 100 |
| `[log] AxisLog` | 记 `daxis` 行（100 ms，前 8 轴 + POV），用于标定"转向轴" | 1 |

`daxis` 行的用途：**转向不足（前轮打滑）时力应"低→高→低"** —— 要复刻这个曲线必须有"转向输入"这个量，
所以先采一段轴数据认下哪个轴是方向盘（以及左右满舵值），下一步再用 `转向角 + 力值趋势` 实现：
**转向在增大而力不再增/反而降 ⇒ 判定打滑 ⇒ 按 `Understeer` 百分比衰减**（先默认 0=关，标定后再开）。
