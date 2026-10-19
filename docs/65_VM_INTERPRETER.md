# 65 · TUFA VM 解释器 —— M4 重建蓝图（解释器核心已全读）

> 状态：✓ 核心闭环（`FUN_00653880` run-loop 逐 case 读完 + 调用链 4 层读完）；✗ 外围辅助函数 12 个未逐一读（§6 清单）
> 工具：`out_native_decomp/00653880.c`（Ghidra 语料）、`tools/dis_va.py`、`out_vm_handlers.csv`（本轮新增）
> 产物：`out_vm_handlers.csv`（85 槽 → 81 唯一 handler → 包含函数映射）

## 0. 一句话结论

**整个字节码解释器 = 一个 27.8 KB 函数 `FUN_00653880`（`run(ctx, budget)`），85 个 opcode
handler 全部内联为 switch case；调用链 `INVOKE → 6537a0(解析) → 6536d0(虚分派) → 6532d0(建帧/原生桥)`
也已读完。** 重建所需的全部结构性事实（上下文/帧布局、栈模型、调用约定、返回槽机制、终止标志）
本轮全部钉死，可以开始写 C 重建版并差分对拍。

## 1. run-loop：`FUN_00653880(ctx, budget)` = 0x653880

* 入口：`ctx.pc==0` 或 `flags&4`（已在跑）→ 异常 `FUN_006478f0`；`env+0x3c` 保存/恢复当前 ctx 链
  （**env = [ctx+0x18]**，线程/环境对象）；`pc = [frame+0x98]`（帧里保存的续跑点）；`flags |= 4`。
* 主循环：`pc=[ctx+0x28]`，`switch(*pc)`；**宽 5 的 case 走 `caseD_1: pc+=4` 再落到公共 `pc+=1`；
  宽 1 的 case 直接 `pc+=1`** —— 与 docs/08 的宽度表逐位吻合（第三套独立证据）。
* 终止：`while ((flags & 0xfa) == 0)` ⇒ **halt 位 = 0x02/0x08/0x10/0x20/0x80**（异常/中止类，
  具体哪位对应什么 ✗ 未读）；收尾 `flags &= ~4`、`[frame+0x98] = pc`（断点续跑）、恢复 env 链，
  返回耗时（`FUN_00567bf0` = 性能钟；**param_2 = 时间预算**，进出处各取一次钟）。

### ctx（方法执行上下文）布局（本轮钉死）

| 偏移 | 含义 | 证据 |
|---|---|---|
| +0x0c | 帧栈容量 | RETURN case 的越界检查 |
| +0x10 | 帧栈数组（caller frames） | RETURN 弹回上一帧 |
| +0x14 | 帧栈深度 | RETURN `-1` 后取帧 |
| +0x18 | env（环境/线程对象） | `env+0x3c`=当前ctx、`env+0x28`=GC/写屏障开关 |
| +0x24 | **当前帧**（§2） | 所有 case |
| +0x28 | **pc**（指令流指针） | 分派点 `mov edi,[esi+0x28]`（docs/08 §5.1） |
| +0x30 | flags：bit2=running、0x40=finished、halt 掩码 0xfa | 入口/出口/循环条件 |

## 2. 帧（frame，0xAC=172 B，池分配）与两个栈

* **帧池 `FUN_00653130`**：freelist（`pool[4]` 头插）+ arena（64 帧/块 × 0xAC，`malloc(0x2b00)`，
  realloc 扩容块表）；释放走 `FUN_00653070` + 全局 freelist `DAT_008ddb84`（RETURN case 16）。
* **帧链接 `FUN_006530e0(ctx+0x0c 向量, frame)`**：把帧压进 ctx 帧栈（{cap,base,count} 向量）。

| 帧偏移 | 含义 |
|---|---|
| +0x00/04/08 | **locals 向量** {cap, base, count}（`FUN_00652f80` 压槽 / `FUN_00652f60` 弹槽 / CLEARN 循环弹） |
| +0x4c/50/54 | **操作数栈向量** {cap, base, sp}；TOS = `base[sp-1]` |
| +0x98 | pc 保存位 / 新帧的字节码入口（双用：调用时=code entry，被调用期间=caller 续跑点） |
| +0x9c | **this**（FIELD_REF_QUICK / PUTFIELD_QUICK 的隐式接收者） |
| +0xa0 | 常量/符号环境（`+0x14` = 符号表；`+0x98` 曾存入口） |
| +0xa4 | `[method+0xc]`（方法名/签名 ref） |
| +0xa8 | 方法信息（`+0x20` = 签名字符串，首字符 `'V'`=void） |

* **向量通用件**：push=`FUN_00652fc0(ctx,v)`（容量不足 `FUN_00652c80(cap+sp+1)` 扩容：
  <17 槽用**帧内联缓冲**（vec+0xC 起），否则 realloc+memmove）；pop=`FUN_00652f10(ctx)`
  （**空栈 pop 静默返回 0，不抛**——重建必须保留此行为，它兜住了 void 调用后的仪式性 POP）；
  peek=`FUN_00652f40(ctx)`；带写屏障的存值=`FUN_00652c50(ctx,slot,v)`（v≠0 且 `env+0x28` 开 →
  `FUN_00647720(v)` GC 登记）。

## 3. 逐 opcode 语义（run-loop 逐 case 摘录；名字用 docs/08 定案表）

**硬规则（重建判据级）：**
1. **跳转基址 = 指令自身位置**：JMP/JMP_NE/JMP_EQ/JMP_EQ2 全部 `pc = 指令地址 + [指令地址+1]`
   （case 0x17 直接 `puVar5 + payload`；0x18/19/1a 用 `pc + [pc+1]`，此时 pc 还指着指令头）。
2. **JMP_NE(0x18) = 弹栈为 0 才跳**；**JMP_EQ(0x19) = 弹栈非 0 才跳**；
   **JMP_EQ2(0x1a) = switch 惯用语**：弹 key、peek 值，`key==值` 才把值也弹掉并跳。
3. **SHORTCUT_AND(0x3e)/OR(0x3f) 的 ±1/±5 双路径**（M3 折叠语义的原生根源）：
   AND：peek≠0 → `pc+=5`（**跳过紧随的 JMP**，落入 rhs 区）；peek==0 → `pc+=1`（执行 JMP → end）。
   OR 对称（peek==0 → +5）。⇒ 字节码形态恒为 `lhs; SC; JMP end; rhs…; end:`。
4. **RETURN(0x16)**：有签名且非 'V' → 先弹返回值；清 locals（652f60）；帧释放回池；
   从 ctx 帧栈弹回 caller 帧；`pc=[caller+0x98]`；**只要有签名就压返回槽**（void 压的是
   未定义寄存器值——native 路径压 0）；pc==0 → `flags|=0x40` 收工。
   ⇒ 与 M3 翻案①「非 init 调用一律压返回槽（void 也压）」完全一致：**压槽发生在 RETURN/native
   桥，不在 INVOKE**；编译器发射的仪式性 POP 由「空栈 pop 静默返 0」兜底。
5. **CAST(0x01) = 纯 no-op**（case 1 只走 pc+=5）；INSTANCEOF(0x02) = 弹对象、非空则
   `656e90`(取描述符)+`6561e0`(判定) 后压结果。
6. **二元运算就地改 TOS**：pop b，`a=base[sp-1]` 原地写回 ⇒ a=先压者。整数比较语义
   （0x4a..0x4f）：IEQ `a==b`、INE `a!=b`、IGT `a>b`、IGE `a>=b`、ILT `a<b`、ILE `a<=b`；
   浮点（0x50..0x55）同构。移位掩码 `&0x1f`。IDIV/FDIV/IMOD 除零 → `FUN_006478f0` 异常。
7. **SADD(0x2e) = 宽字符串拼接**：弹两对象取串指针（`656550`）、长度（`6675de`）、malloc、
   `wcsncat`、`FUN_00644cf0` intern 后压回；任一为 null → 压 0。
8. **字面量组**：0x03 NULL 压 0；0x04–0x07 共用 handler（压 `[pc+1]` 4 字节）；0x08 STRING =
   `656e90`+`644c40`（串池查找）压串对象；0x09 RID = `656f40`。
9. **locals 组**：LOCAL_LOAD(0x0b) 带界检查压 `slots[pay]`；LOCAL_CREATE(0x0c) = locals 压 0 槽
   （`652f80(frame,0)`，无载荷）；LOCAL_STORE(0x0d) 弹值存 `slots[pay]`（非 0 走 GC 屏障）；
   LOCAL_CLEAR(0x0e) 弹一个 local；LOCAL_CLEARN(0x0f) 循环弹 pay 个。
10. **字段组**：INSTANCE/STATIC 先 `6570a0` 查解析、-1 则 `657820` **懒解析**再查；
    STATIC 经 `6570d0(ref)→6492e0(符号表,desc)` 拿类、`656080` 拿静态槽；
    INSTANCE 弹对象（null → NPE 异常 + 压 0）`656550` 拿字段槽；**QUICK 变体不弹对象，
    用 `[frame+0x9c]` 的隐式 this**。PUTFIELD 三变体对称（弹值 + 弹/隐式对象 + `652c50` 写）。
11. **数组组**：NEWARRAY(0x22) = `656e90` 取描述符数 `'['` 前缀得维数 → 弹维数入
    `aiStack_408` → 逐维 `647e60`/`656170` 分配嵌套数组 → 压结果；ARRAY_INIT(0x24) =
    弹数组、弹 pay 个元素逐个 `6565d0` 定位写回（带 GC 屏障）、**最后把数组压回**；
    EMPTYDIMS(0x25) = 压 pay 个 `-1` 哨兵；ARRAY_ACCESS(0x26) = **弹 arr、弹 idx**（idx<0 或
    arr==0 → 异常+压0）`6565d0` 取元素槽压值；ARRAY_STORE(0x23) = **弹 value、arr、idx**。
12. **NEW(0x27)**：`6492e0(符号表, ref)` 解析类，失败压 0；成功 `656100` 实例化后压对象。
    **构造由后续 INVOKESPECIAL 完成**（NEW 不调 init）。
13. **类型转换**：F2I(0x1b) = `x=(float)(int)x`（截断回浮点）；I2F(0x1c) = `x=(float)(int)x`
    的 int→float；I2S(0x1d) = 格式化（模板 `DAT_006e7c54`）+ intern；F2S(0x1e) = `"%.3f"` + intern。
14. **DELETE(0x28) 无实现**：handler = 0x653d9b = 非法 opcode 兜底路径（与保留槽 0x0a 共用）
    —— 全库 0 次发射，一致。**0x0a/0x28/0x00/>0x55 全部落兜底**。
15. **栈洗牌**（M3 长尾的 21 条 RAWREC 就是它们）：DUP_X1(0x2b)=pop,pop,push×3 回插；
    DUP_X2(0x2c)=pop×3,push×4；DUP2(0x2d)=push TOS ×2；DUP(0x2a)=peek+push；POP(0x29)=pop。

## 4. 调用链（INVOKE 0x10 / SPECIAL 0x11 / STATIC 0x12 → kind 0/1/2）

```
case 0x10-12: FUN_006537a0([frame+0xa0]+0x14 符号表, ref, kind)   ; 返回 0=已切帧(pc 别动) 非0=pc+=5
 └ 6537a0: 6570a0(ref) 查解析(-1→657820 懒解析重查) → 6570d0(ref) 描述符
           → 6492e0(符号表,desc) 找类(找不到→"Thread::callmethod: no such class"异常)
   └ 6536d0(ctx, 类, ref, kind): m=654e90(ref) 取 Method
       kind&3==0(虚调用): 接收者 = 操作数栈[sp-argc-1](argc=[m+0x1c])；null→"null.methodcall (%s.%s%s)"异常；
         接收者类≠声明类 → ([m+0x10]==1 时 654d00 先规范化) → 654e90 重取 = **虚分派**
     └ 6532d0(ctx, m, kind&2): 两条路
        A) 解释方法(([m+0x14]&0x100)==0 且 code!=0):
           [caller帧+0x98]=pc 保存续跑点 → 653130(ecx=[0x8ddb84] 全局帧池) 取帧 → 65f450 构帧
           → 6530e0(&ctx+0xc, 帧) 帧入栈 → 帧[0xa4]=[m+0xc]
           → 非 static: **652f80(新帧,0) 先在 callee locals 压 0 占位槽，返回槽号（=this 的
             locals 下标；capstone 0x65344c-0x653455 定案）**
           → 循环 argc 次: ctx.frame 暂切回 caller、从其操作数栈弹出、压入 callee locals
             ⇒ locals=[this, argN…arg1]（右到左）⇒ **形参第 k 个 = local_(n+1-k)**（docs/48 规则的 VM 级根源）
           → 非 static: 再弹接收者 → 帧[0x27](+0x9c this) + locals[this槽号]
           → ctx.frame=新帧; pc=帧[0x26](+0x98=字节码入口); return 0
        B) 本地方法(flag 0x100): 652a80(ctx,[frame+0xa0],kind,m) 把实参装配进 20 字节栈缓冲
           → **调用约定 = `fn(&argsbuf)` 单指针入参**（capstone 0x653351：`lea eax,[esp+0x14];
             push eax; call esi`）；返回按签名 [m+0x20] 首字符走跳转表 0x65358c/0x6535a0：
             'F' 走 ST0(fstp) / 'I','L','[' 走 eax / 'V' 丢弃
           → **有签名就压返回槽（'V' 压 0）**；无签名(0)不压
           → pc==0 且 flags==0 → flags=0x40; return 1
        C) code==0 的解释方法(抽象?): 弹掉 argc+this 个栈值(置0)、有签名压 0 槽
        SEH: 整段包 __try(ExceptionList) ⇒ 异常经 6478f0 走 SEH/长跳
```

* **本地方法注册表 = 0x652910**（docs/54 已解：25 个 Replay 原生方法就在里面）。
* native 桥 `652a80`（338 B）✗ 未逐行：实参如何按签名装配进 20 B 缓冲（最多几参、浮点槽位）待读。

## 5. `out_vm_handlers.csv`（本轮新增产物）

85 槽 → **81 个唯一 handler VA，全部内联在 `FUN_00653880`（27,849 B，语料 `00653880.c`）**；
仅两组共享：`0x654080`（0x04–0x07 字面量压栈）、`0x653d9b`（0x0a 保留 + 0x28 DELETE = 兜底）。
列：slot/opcode/name/width/handler/func/delta/corpus。

## 6. 辅助函数清单（重建时的模块边界）

| VA | 角色 | 状态 |
|---|---|---|
| 0x652fc0/652f10/652f40 | push / pop(空返0) / peek | ✓ 已读 |
| 0x652c80 | 向量扩容（内联 16 槽 ↔ heap） | ✓ 已读 |
| 0x652f80/652f60 | locals 压槽 / 弹槽 | ✓ 已读 |
| 0x652c50 | 写值 + GC 写屏障 | ✓ 已读 |
| 0x653130/6530e0/653070 | 帧池取 / 帧栈压 / 帧释放回 freelist | ✓ 已读 |
| 0x653880 | **run-loop 本体** | ✓ 全读 |
| 0x6537a0/6536d0/6532d0 | callmethod 解析/虚分派/建帧+native桥 | ✓ 已读 |
| 0x65f450 | 帧构造（双向量内联 16 槽） | ✓ 已读（§6.1） |
| 0x6570a0/6570d0/657820 | 解析缓存读 / 描述符读 / **懒解析器** | ✓ 已读（§6.1） |
| 0x6492e0 | loadClass（带缓存，miss 走 649170） | ✓ 已读（§6.1） |
| 0x656080/656550/6565d0 | 静态槽 / 实例字段槽 / 数组元素槽 | ✓ 已读（§6.1） |
| 0x656100/656750 | 实例化外壳 / Instance::initialize | ✓ 已读（§6.1） |
| 0x656170/654ef0 | 数组分配 / 数组数据块 | ✓/✗（外壳已读） |
| 0x644c40/644cf0 | 串池按下标取 / intern | ✓ 已读（§6.1） |
| 0x647720 | GC 写屏障（脏标记+登记，数组递归） | ✓ 已读（§6.1） |
| 0x655100 | **Class::initialize（类链接器，19 KB）** | ✓ 头部已读（§6.1；own-method 合并尾部 ✗） |
| 0x654e90/654d00/654cf0/654d90 | Method 取 / 规范化 / 名字串 / 按名+签名找方法 | ✓/✗✗✓✗ |
| 0x6561e0/656e90/656f40 | instanceof(→645ad0+655f90) / 池串 / RID | ✗/✓/✗ |
| 0x6478f0/644f30 | 异常入口 → 报给 env 当前 ctx | ✗ |
| 0x652a80 | native 20B 缓冲装配 | 部分（§6.1） |
| 0x656b70/656b00 | 分配钩子（疑 GC 触发点） | ✗ |
| 0x567bf0/567cc0 | 性能钟 / 格式化 | ✗ |

### 6.1 对象模型与解析链（2026-10-16 第二刀读完）

**符号表/常量池**（`[frame+0xa0]+0x14`，即 CONS 块的运行时形态）：`+8` 条目数、`+0xc` 条目指针数组。
条目 `+4` = **类型标签**：`0` = C 字符串（`+8` = 指针）、`4` = 指向串条目的 ref、`5` =
**NAME_AND_TYPE**（`+8` = 名字 ref、`+0xc` = 描述符 ref、`+0x10` = **解析缓存**，-1 = 未解析）、
`7` = 间接层。读取器：`6570a0` = 读缓存、`6570d0` = 读名字/描述符 ref、`656e90` = 类型 0 取串。

**懒解析 `657820`**（JVM 式）：解出描述符串，**首字符 `'('` = 方法** → `6492e0` 加载类 +
`654d90(名字,签名)` 找 Method → 写回缓存 `+0x10`；否则 = **字段** → `649330` + `6589d0`。
失败抛链接错误：`"Referenced field '%s' no longer exists in class '%s'. Recompile all classes if possible!"`。

**loadClass `6492e0`**：`6571a0` 缓存查 → miss：`657130/656e90` 取类名 → `649170` 加载/定义 →
`6571d0` 存缓存。

**Class 对象**（链接器 `655100` = Class::initialize，幂等 flags bit0）：

| 偏移 | 含义 |
|---|---|
| +0x00 | 类数据块（TUFA CLSS 运行时映像：`+0x40` 超类名 ref（<0=无）、`+0x44` 接口数、`+0x48` 接口名 ref 数组、`+0x54` 自有字段块（`+0xc` = 自有字段数）） |
| +0x04 | 名字（串对象；`654cf0` 可取 C 串） |
| +0x08 | flags（bit0 = 已链接） |
| +0x0c | env |
| +0x10 | =1 表示接口（非接口类实现接口时校验，错误串 "Class::Class: Interface %s extends Class %s" → 日志缓冲 `DAT_008dcb48`） |
| +0x14 | 所属符号表 |
| +0x18 | **字段总数** = 超类继承 + 自有 + 数组类再 +1 |
| +0x1c/+0x20 | 实例计数（`656750` 创建时两处 +1） |
| +0x28 | 超类对象（递归链接） |
| +0x2c | 接口向量 {cap,base,count} |
| +0x34 | 静态字段槽向量（`656080`：写前先跑 `655100`） |
| +0x3c/40/44 | **虚方法表** {cap,base,count}（`654e90`：ref bit30=1 走这张） |
| +0x48/4c/50 | **静态/special 方法表**（bit30=0 走这张） |

链接流程（已读部分）：解超类 → 解接口（校验接口标志）→ 字段总数 → 递归链接接口 →
方法表从超类 {+0x3c..} / {+0x48..} 克隆（own-method 覆盖/追加的合并尾部 ✗ 未读完）。

**Instance 对象**（`656750` = Instance::initialize）：

| 偏移 | 含义 |
|---|---|
| +0x00 | class 指针 |
| +0x04 | 字段容量（= class[+0x18]） |
| +0x08 | **字段数组基址**（malloc/realloc n×4，`thunk_00581cd0` = memset 清零） |
| +0x0c | （错误消息里用作界，疑与 +0x04 同源） |
| +0x14 | 对象 flags：`0x10` = 已登记、bit1 = GC 脏/已登记、bit3(bit8) = 特殊（创建时清）、bit0 = 钉住/入池 |

**数组** = 类名以 `'['` 开头的 Instance：`fields[DAT_008dcb30]` = 长度、
`fields[DAT_008dcb28]` = 数据块 `{+0 count, +4 base, +8 ?}`（`654ef0(len)` 分配并清零）；
元素槽 `6565d0` 双界检查（长度字段 + 数据块 count），错误串 `Instance::getArrayElement: ...`。
`DAT_008dcb28/30` = 长度/数据两个字段下标的全局常量（初始化点未读 ✗，推测 0/1）。

**帧构造 `65f450`**：locals 向量 = `{cap=16, base=frame+0xc 内联缓冲, count=0}`；操作数栈向量 =
`{cap=16, base=frame+0x60 内联, count=0}`（超出走 `652c80` 堆扩容）；`frame[0x26]`=code 入口、
`[0x27]`=this、`[0x28]`=符号环境、`[0x2a]`=Method ⇒ **0xAC 字节帧全图钉死**
（+0x00 locals 向量 / +0x0c locals 内联 16 槽 / +0x4c opstack 向量 / +0x60 opstack 内联 16 槽 /
+0x98 pc / +0x9c this / +0xa0 symenv / +0xa4 method-ref / +0xa8 method）。
**帧释放 `653070`**：堆缓冲 free 回内联（cap 复位 16），帧体回池不 free。

**GC 写屏障 `647720`**：仅对类名 `'L'`/`'['` 开头的值生效：flags 含 0x10 且 (flags&9)==0 →
置 bit1 + `65a0e0` 登记；数组递归遍历元素。触发点：`652c50` 写槽、`655100` 前检查。
分配钩子 `656b70→656b00`（✗ 未读，疑 GC 触发）。

**串池**：`644cf0(cstr)` = intern（`659c80` 查 → miss：`656100` 分配 String 对象 + `65a080`
拷宽串写入 `DAT_008dcb2c` 单例的字段）；`644c40(symtab,idx)` = 按池下标取（`659de0` 查）。
String flags `+0x14`：bit3 创建时清、bit0 = 钉住；池满策略看 `[pool+0x28]/[0x2c]==3/[0x1c]`。

**异常 `6478f0(env, fmt, ...)`** = `644f30([env+0x3c] 当前 ctx, fmt, va)` —— 错误报给运行中
的 ctx（SEH/长跳细节 ✗ 未读）。所有越界/空指针/除零都走这里（消息串全在语料里可 grep）。

**Method 对象**（用法汇总）：`+0x00` code（解释 = 字节码入口 / native = 函数指针）、
`+0xc` 名字ref、`+0x10` 标志（==1 时虚分派先 `654d00` 规范化）、`+0x14` flags（bit8=0x100 = native）、
`+0x18` 声明类、`+0x1c` **argc**、`+0x20` 签名字符串（首字符 'V'/'F'/'I'/'L'/'['）。

**native 桥补充**：`652a80` 把 `{code, [method+0x18], kind, symenv, method}` 五字段写进 20 B
栈缓冲，native 以 `fn(&buf)` 单指针入参（实参仍从 VM 栈由 native 自取？装配细节 ✗）。

### 6.2 第三刀：类链接合并、native 注册、行号表、VM 引导（2026-10-16）

**Method 对象定案**（`655100` 里 `malloc(0x24)`=36B）：`+0x00` code（解释方法 = TREE 记录基址
（按记录索引查 `[class+0x24]` 记录表；native = 函数指针）、`+0x04` flags（0x100=native）、
`+0x08` 记录索引/native 表项、`+0x0c` 名字（宽串指针）、`+0x10` 签名（宽串指针）、
`+0x14` 来源定义项、`+0x18` 定义类、`+0x1c` **argc（签名解析数出）**、`+0x20` **返回串指针
（签名 `')'` 之后，无则 0 = void）**。签名解析 switch（`I/F/V`+1、`L…;`+1、`[I`+1、`[L…;`+1）。

**虚表合并规则（重建关键，已定案）**：own 方法逐个 `654d90(名,签名)` 在继承表里找：
**找到 → 原地覆盖该槽（override 保槽号）；找不到（返 -1）→ 追加**（cap*2+2 扩容）。
两轮：`[数据块+0x60]` 虚组 → 类表 `+0x3c`，`[数据块+0x58]` 静态组 → 类表 `+0x48`。
`654d90` 返回编码 = **虚表命中 `idx|0x40000000`、静态表命中 `idx`、未找到 `-1`** ——
这就是运行时 method-ref 的 bit30 编码来源（与 `654e90` 取法完全互证）。

**native 实现查找**：hash(类名,方法名) `% 0x305`（773 桶）入 `[env+0x10]+4` 哈希表（12B 项：
{hash, 条目, next}），命中后还比对签名串与 `FUN_0041d270()`（版本/组 id）；失败日志
`"Class::init: native implementation missing for <class>.<method><sig>"`，Method.code=0
⇒ 调用时走 §4 的 C 路径（弹参压 0）。**21 个注册函数在引导期灌表**（`6627a0/662610/662360/
661d20/661870/4da7a0/4040b0/404560/404c60/404ec0/405220/405420/406270/406a10/406d30/405720/
409a20/408df0/40db90/40e7d0/40f8a0/40ffc0/411a10` —— 完整 native API 面，逐个解码 = M4 各模块
与逻辑层的接缝清单；Replay 的 0x652910 注册表是其中一员，docs/54 已解）。

**接口映射**：超类接口并入本类接口向量（去重）；对每个接口 × {虚,静} 建映射向量
（类表 `+0x54/+0x60`，每接口一组）：接口第 i 个方法 → `654d90` 在本类表找 → 存槽号，
找不到日志 `"Unimplemented interface method in class …"`。调用期 `654d00(class, 接口对象, ref)`
= 在接口向量定位下标 → 用映射表把接口相对 idx 换成本类表 idx（错误串是匈牙利语
「nincs ilyen interface benne」—— Invictus 团队痕迹）。

**instanceof/继承判定 `655f90(a,b)`**：`class+0x10` 是种类判别（0=类/1=接口）；类走 `+0x28`
超类链、接口扫 `+0x2c` 向量并递归；种类不匹配日志 "Class::isInheritedFrom: class/interface
mismatch error"。

**finalize**：`DAT_008dcb3c` = 引导期解出的 `Object.finalize ()V` 全局 ref；链接尾部若类表里
存在该 ref → `flags |= 4`、`class+0x1c = (code==0)`（有无实现）。

**`<clinit>` 触发点（定案）**：链接器尾部 flags bit1 首次置位时：取 `[数据块+0x4c]` = 静态
初始化记录索引 → `65f450` 建帧（名 `"<static field init>"`）→ **建线程 `"THRD-CLASSVAR-INI"`
（`6531b0(env,10,1,…)`）→ `653880` 直接跑完**。即 TREE 记录 0 在类首次链接时同步执行。

**异常 = 打印后继续（非 SEH 展开）**：`6478f0(env,fmt,…) → 644f30([env+0x3c] 当前ctx,…)`：
`"Runtime error in thread '%s'\n"`（线程名 `[ctx+0x20]`）+ 消息 + **逐帧栈回溯**：帧栈倒序，
`frame+0xa4` ref → `654cd0` 得 `类.方法`；`frame+0xa8`（方法信息）非空时取 `[+4]` = **行号表
ref**，`pc − 记录基址` = 记录内偏移，查 (offset,line) 对表（`655faf0` 解行号表 blob）→
`"%s\n%s:%s (line:%d)"`，无行号表则 `"(offset:0x%04x)"`。**打印完返回，调用点继续执行**
（各 handler 出错后压 0 兜底）；`652c40` 只是 `[frame+0x98]=pc` 保留现场。
⇒ TUFA 容器里**存着行号调试信息**（记录级 line table + 源文件名），重建时可原样保留。

**VM 引导 `FUN_00652240(env)`**：注册四个内建类型伪类 `'I'/'F'/'L;'/'V'`（`DAT_008dcb1c/40、
18、24、20`）；`DAT_008dcb38` = `"Ljava.lang.String;"`、`dcb34` = String 类对象；
**数组长度字段 idx `DAT_008dcb30`=0、数据块字段 idx `DAT_008dcb28`=1、String 值字段 idx
`DAT_008dcb2c`=0（定案）**；env 字段：`+0x10` = THRD-FIN 终结器线程、`+0x4c` = Thread 类、
`+0x3c` = 当前 ctx；随后 21 个 native 注册函数 + `652d30`。

**分配器定案**：Instance 头 = **0x18=24 B slab**（`656b00`：freelist + 20000 块/480 KB chunk），
`656100(class)` → `656750(hdr, class, 0)`；数组数据块 = `{cap, base, count}` 12B（`654ef0`）。

### 6.3 第四刀：native ABI 定案 + 完整 native API 面（2026-10-16）

**native 调用约定（`652ab0` 实测定案，✗「实参到达路径」已销）**：

```
native 原型:  u32 fn(NativeBuf *buf)          // 'F' 签名的返回走 ST0
取参:        FUN_00652ab0(buf, &this, &arg1, &arg2, …)
  buf[0] = isStatic（0 ⇒ 第一个 vararg 收 this：先从 VM 栈多弹一个）
  buf[1] = Method（argc = [buf[1]+0x1c]）
  行为: 从 VM 操作数栈弹 argc 个值入 local_400[256]（上限 256 参），再逆序写回
        各 vararg 指针 ⇒ 恢复源码顺序（栈上 arg1 最深）
返回: 'I'/'L'/'[' = eax；'F' = ST0；'V' = 丢弃并压 0（§4 B 路径）
```

实证样本：`Object.getClass`（0 参取 this）、`Replay.flagObject(II)`（this+2 int，读
`DAT_00770fc0` 回放对象链 `[+0x34]`，节点 `+0x30`=id、`+0x28`=flag 槽）。

**完整 native API 面（`tools/vm_natives.py` → `out_vm_natives.csv`）**：全语料扫
`FUN_00652910(类,方法,签名,fnptr)` 注册点 ⇒ **1,156 条注册 / 110 个类 / 28 个注册器，
0 未解析、0 重复**。已并入 `out_native_modules.csv` 的 module 列（832 命中；324 个小胶水
native 不在归属表里）。头部类：`java.gui.Component` 108、`java.gui.Gauge` 74、
`java.game.parts.Chassis` 53、`WheelRef` 41、`MetaServer` 31、`Sound` 29、`GfxEngine` 28、
`System` 17、`Replay` 16。注册器规模：`0x5c37e0`=697（GUI 控件全家）、`0x488b60`=98、
`0x40db90`=55、`0x426fc0`=51、`0x488b60/0x4a6d40`=bridge 主力。
**这张表 = M4 逐模块重建时「逻辑层期望的原生行为」的权威清单**：每行给出类.方法(签名)→
实现 VA → 所属模块 → Ghidra 语料文件，重建一个模块就按行对拍。

对账注记：docs/54 说 Replay 有 25 个 native 方法（类内声明口径），注册表实际只灌 16 ⇒
差额走「native implementation missing → 调用弹参压 0」容错或另有注册名，用到时再核。
Ghidra 已符号化 4 个：`vm_file_open/vm_gfxengine_init/vm_system_timewarp/vm_thread_start`。

## 7. 重建与差分判据（M4 该模块的验收）

1. **对象模型先行**：类/字段槽/方法/串池的运行时表示（§6 ✗ 组的前 8 个函数）——下一刀。
2. C 重建 `tufa_vm`：ctx+帧+两栈+85 case，逐条对照 §3 硬规则。
3. **差分测试**：用 unicorn（`.capenv` 已有）把原版 `FUN_00653880` 与其辅助函数在受控内存里
   跑真记录（如 `out_src_v2/` 里任一类的方法），逐步对拍重建版的 (pc, sp, locals, TOS)；
   或者最低成本先行：**用重建 VM 执行 `<clinit>` 记录，对 `out_init.json` 常量真值**（既有语料）。
4. 时间预算/性能钟行为（param_2）只在联机/回放节奏相关，功能对拍可先置 ∞。

## 8. 未解（✗）

* halt 位 0x02/0x08/0x10/0x20/0x80 各自被谁置位（异常路径 `FUN_006478f0 → 644f30` 未读；
  SEH `ExceptionList` 帧内 `local_c` 结构未解）。
* ~~`FUN_00653130` 在 callmethod 语境返回的 `local_24`~~ **✅ 已定案（capstone）**：653130 返回
  **帧指针**；Ghidra 把两个变量合并了——`652f80(帧,0)` 的返回值才是 this 的 locals 槽号（=0）。
* 解释型 void 方法 RETURN 压进返回槽的具体值（未定义寄存器 → 实际是上次残留；重建可用 0，
  行为差异仅在「POP 后继续用该值」的病态路径——全库普查 void+POP 后无消费者即可豁免）。
* ~~native 桥 `652a80` 的实参装配细节~~ **部分定案**：20 B 缓冲 = `{code, 声明类, kind,
  symenv, method}` 元数据五字段，native `fn(&buf)` 自取上下文；**实参如何到达 native 仍未见**
  （疑经 symenv/栈直读，需读一个具体 native 实现，如注册表 0x652910 里的 Replay 方法）。
* ~~符号解析链与 CONS 块的关系~~ **✅ 运行时侧已定案（§6.1）**：CONS = 类型标签条目表
  （0=串/4=串ref/5=NAME_AND_TYPE/7=间接），解析缓存挂条目 +0x10；**静态侧**（.class 文件里
  CONS 槽如何编成这些类型）仍属 docs/08 §8 遗留。
* ~~类链接器 `655100` 的 own-method 合并尾部~~ **✅ 定案（§6.2）**：找到覆盖槽、找不到追加；
  ~~`DAT_008dcb28/30`~~ **✅ =1/=0（引导函数 `652240` 写入）**；~~`654d90`/`655f90`/`654d00`~~
  **✅ 已读（§6.2）**；~~`644f30` 异常机制~~ **✅ = 打印栈回溯后继续执行，非 SEH 展开（§6.2）**；
  ~~`656b00` 分配钩子~~ **✅ = Instance 24B slab 池（非 GC）**。
* ~~native 实参到达路径~~ **✅ 定案（§6.3）**：`652ab0(buf,&this,&arg1,…)` 弹栈逆序填 vararg
  指针，buf[0]=isStatic、buf[1]=Method；上限 256 参；'F' 返回走 ST0。
* 仍 ✗：halt 位 0xfa 的置位者（错误路径不置位 ⇒ 疑为线程 stop/breakpoint 专用）；行号表 blob
  格式（`655faf0` 解析器）；`FUN_0041d270`（native 查找比对的版本/组 id）；docs/54 的 Replay
  「25 native」与注册表 16 条的差额（用到时再核）。

## 9. C 重建差分闭环（vm/tufa_vm.c，2026-10-16 第五刀）

**结果**：1,066 类 `<clinit>` 全量执行零崩溃；对 out_init.json 常量真值差分
**2,713 ok / 0 bad / 0 missing**（数值+字符串+数组+bool/null+括号数+符号引用自洽），
剩余 skip=1,592 ctor 表达式（需对象同一性语义）+ ~700 方法调用型表达式。
批量统计（native 第一批落地后）：**375,821 steps / 0 errors / 0 oob**，
native_impl 6,198 + 桩 13,084（Part 渲染建造机家族，全 void）。

**八个致命坑（全部实测修复，重建 VM 必读）**：
1. **tree_slot 是 0 基记录索引**（MTHD 行第 4 列直接下标 recs[]，不是 1 基）。
2. **签名参数计数的 `[` 处理**：`([I)V` 里 `[` 后必须前进指针再数 1 个参数；
   `continue` 不前移 = 死循环（全批挂死的元凶）。
3. **代码尾 = 正常 RETURN**：空记录（Object.<init>，len 0）与跑出记录尾都按
   RETURN 语义收帧（retc=='V' 也压返回槽），不是错误。原版空方法被调用亿万次。
4. **调用方 pc 必须在 callee 入帧前推进**：INVOKE 成功压帧后，调用方帧的 pc
   要先 +5，否则 callee RETURN 回来重执行同一条 INVOKE → 无限递归烧预算。
5. **ARRAY_ACCESS(0x26) 弹栈序 = 先 arr 后 idx；ARRAY_STORE(0x23) = value、arr、
   idx**（00653880 case 直读定案；此前 §3 第 11 条记反，已改）。弹反 = 把小整数
   当 Inst* 解引用 → 段错误。
6. **数组类的 `length` 字段**：NT 引用 (数组类, length:I) 必须解析到 fields[0]，
   合成 '[' 类没有 FILD 表，需要特判；否则 r_idx 悬空读出错位字段。
7. **野指针守卫**：native 污染（桩返回 0/垃圾）会让对象引用型栈值变成非 NULL
   垃圾。所有 Inst 解引用点（inst_field/arr_elem/str_w/instanceof/虚派发/dump）
   必须过 inst_new 时注册的指针有效性表，错误+压 0 继续，绝不允许进程崩。
8. **SHORTCUT_AND/OR 的跳过路径 = ip+6，不是 ip+5**（跳过 SC 自身 1B + 紧随的
   JMP 5B）。原版 pc 是预增寄存器（puVar5=ip+1）：caseD_a = puVar5+1 再 caseD_1
   +4 ⇒ 净 ip+6；写成 ip+5 会落进 JMP 载荷末字节 ⇒ `illegal opcode 0x00` ×18,669。
   极性：SC_AND peek!=0 → 跳 rhs（继续求值），false 落进 JMP→end；SC_OR 相反。

**继承方法的记录与常量池（第 9 坑，递归炸帧的元凶）**：方法的代码记录在
**声明者类**的记录表里——`call_method` 必须 `owner = member_owner(decl, m)` 后用
`owner->recs[tree_slot]` 建帧，且帧的 symenv（f->cls）= owner（代码里的 NT 引用
按 owner 的常量池解析）。用 NT 指名类（decl）的记录表会把「子类调继承方法」映射
到错误记录（实测：CalculatePhysRadius 拿到 staticinit 的码 ⇒ 512 层递归，
511=MAX_FRAMES-1，configureType 被灌 5,621 次）。修后全批 steps 5.5M→243K、
errors 37K→242。

**native 实现层（第一批 14 个，2026-10-16）**：
- 分派 = owner 类名+方法名+desc 精确匹配 `g_natives[]`；实参从栈右到左弹出存
  argv[]（正序），this 单独弹；返回值 retc!=0 才压。
- **Class 对象表示**（forName=FUN_00662410 / getClass=FUN_006626c0 定案）：
  java.lang.Class 实例 fields[0] = 原生 Class*（643b30(obj, dcb2c=0, cls)）；
  Class.class 声明字段数为 0 ⇒ 需要 inst_field_grow 扩容写入。
- 已实现：Class.forName/newInstance(FUN_00662500：inst_new+同步跑<init>()V)/
  getName、Object.getClass、System.arraycopy/exit(String)、File.exists、
  ResourceRef.newNative(FUN_004115c0：fields[0]==0→12B清零句柄)/id(FUN_00410860：
  handle[2]?node[10]:0)/set(String,I)(FUN_004103d0：仅校验，静默no-op)/
  duplicate+RenderType.duplicate(FUN_004114b0：句柄null→0，否则新wrapper+新12B句柄)、
  Matrix.newNative(FUN_00405530：64B清零)/set(LAB_00405590：**仅返回this，
  T*R*S 数学链 567e00/562900/562750/563520 未建模**——字段名串在 0x6e7d48..58)。
- 效果：errors 242→**0**（duplicate 消灭 84 个 null.methodcall；SC 修复消灭
  18,669 个 illegal opcode 与 2,658 个假 System.exit）；native_impl 6,198/批。
- 剩余 13,084 桩 = Part 渲染类型建造机家族（configureType 10,043/
  createTypeBegin·End 1,342/configureVisual 592/configureMesh 451/
  configureTexture 437，全 void、只写原生侧类型表）+ 长尾 —— 归 M4 原生层重建。
- 注册表勘误：out_vm_natives.csv 有漏（configureType/forName 等 328 条未录）；
  直接扫语料 `FUN_00652910("cls","name","sig",FN)` 四文本参可得 398 条，
  与旧表并集才是全集；旧表独有的是 GHIDRA 把字符串折成 &DAT_ 引用的场景。

**其他定案**：native 桩只在 retc!=0 时压返回槽；RETURN 的 have=（有 Method 且
retc!=0），retc=='V' 压 0、无 retc 不压；7 个多段 .class 文件按内层 TUFA 标记
切段注册全部 18 个内嵌类（class_index 2,242 条）；错误打印限流 200 条 + 每类
steps/errs/nat 计数（否则 96M 行 stderr）。

**差分器**（vm/vm_diff_init.py）：读 vm_dump.txt（TSV：类/字段/类型/idx/u32/
float[/str:…|arr:N 元素…]，字符串元素带引号、空格转义 \s、引号转义 \q）；
真值侧支持 true/false/null、括号数、"{...}" 数组（含括号元素、[F 按 IEEE 位
模式转 float 比较）、符号表达式对 VM dump 自洽求值。

### 9.1 方法级执行（2026-10-16 第三轮）

- `--call Class.method --desc D --args "a|b"`：静态/实例方法单次调用（this 用
  `new:Class`/`null`；数组参数 `F:1,2,3` 形式），run_steps 从 run_record 抽出复用。
- `--construct Class --desc D --args …`：inst_new + 真跑 `<init>` + 按超链顺序
  dump 全部实例字段（FLD 行，含 str:/arr: 列）。
- **构造器的 desc 在容器里不带返回类型**（`(I)` 而非 `(I)V`）——按 `(I)V` 找会
  ctor not found；普通方法带返回类型。
- **f32 逐步舍入是等值判据的一部分**：Interpolate(0.25,0.1,0.9,-3,7) 期望值必须
  每个中间量都 f32 舍入（q=f32(f32(x-x0)/f32(x1-x0)) → p=f32(q*f32(y1-y0)) →
  f32(y0+p)），double 中间量会差 1 ulp（0xBF8FFFFF vs 0xBF900000）。
- **游戏自己的 Math.PI = 0x40490FD9（3.14159226），不是 IEEE π（0x40490FDB）**
  ——期望值链要用 dump 出来的位模式。
- `vm/vm_method_tests.py`：11/11 过——覆盖 f32 乘加链/F2S "%.3f"/SADD/[F 传参/
  ARRAY_ACCESS/比较分支/FDIV/switch 形 if-else+回退返回/null 进出/静态调用约定。
- `vm/vm_ctor_sweep.py`：out_init.json 1,662 条 ctor 表达式 → 字面量/可解析符号
  参数子集逐条 --construct，断言 errors=0；desc 由 .tasm M 行 + 参数类型匹配自动
  挑选（唯一候选才跑）；嵌套 new/数组字面量参数 = 二期。
- GameType("system.rpk",21) 实测走到 System.exit("invalid resource reference")
  ⇒ 资源包系统是下一块必须实现的 native 域（ResourceRef.set→资源表校验链）。
  **已解（最小模型）**：链路 = GameType(String,I) → ResourceType(String,I) →
  ResourceRef(String,I) = `this.set(lib, type)`（native FUN_004103d0：
  resolve(name,type)→resID 绑进 handle，handle+8=node、node+0x28=resID，
  id()=FUN_00410860 读它）。实现 set(String,I)/set(I)/createNativeInstance：
  按 (name,type) 注册确定性合成 resID（0x10000 起）——Java 侧契约 id()!=0 达成，
  **真 rpk resID（docs/52/57 目录 index=(group<<16|type)）留给资源模块重建轮**。
  又一例伪码空体陷阱：ResourceType.<init>(String,I) 伪码 = 裸 return，真实
  REC 3 = 委托 ResourceRef.<init>(String,I)（结构化重建吞 super 委托链）。

## 10. Unicorn 原生引导对拍线（tools/uni_harness.py + uni_entry.py + uni_boot.py）

目标：让真 LASR.exe 的**原生代码**在 unicorn 里从 PE 入口真跑起来（CRT 引导 →
WinMain → 5f9020 数据引导 → 434cd0 VM 引导），与 tufa_vm 逐步对拍，替代
"只跑 VM 字节码"的局部真值。IAT 全部 306 个导入重定向到 HOOK_BASE 槽位，
Python 侧实现 Win32 语义；文件 API 直通真实安装目录（roots=C:\Games\LASR）。

### 10.1 已验证事实（本轮实测）
- PE 无 BOUND_IMPORT/DELAY_IMPORT；IAT=0x6e7000(0x500)。入口 0x567990 =
  引导壳：拷贝 [0x7422dc..0x7422e4]、设 [0x742000]=0x5678f0，然后
  call 0x66918f = WinMainCRTStartup（VS2005 系 CRT：探测
  InitializeCriticalSectionAndSpinCount/FlsAlloc/FlsGetValue/FlsSetValue/FlsFree）。
- WinMain 调用点 = 0x66930b `call edi`；CRT 致命错误出口：
  `_NMSG_WRITE` 入口 0x673b78（参数=消息号，9=R6010 区段；表 0x75c434），
  `_setargv`=0x67411c（失败→fatal 8），`_setenvp`=0x673ee9（失败→fatal 9，
  读全局 [0x8de0a8]=ANSI 环境块）。
- 数据引导主链 FUN_005f9020：InitLZW(0x54d550，用低地址 0xF0000 区做字典) →
  FUN_0054bbe0("system.rpk")（置资源管理单例 @0x743778；失败→566920
  "Data is missing. (distr. v0.5.2)"）→ InitCarConstants → FUN_00434cd0
  （VM 引导 0x652240 + native 注册 + Config）→ acceptBuild(1183)/version 校验
  → 0x436130/0x551cd0/0x435de0/0x54baa0/0x4342f0 → 条件 0x4c3b40
  （[PTR_0x7432c8+0x1c0]!=0）→ 0x4b1740 → 0x54c3a0。build 1183, Jan 24 2007,
  distr v0.5.2（启动日志字符串）。
- FUN_005604d0 = 递归目录扫描器（*.zip 找档案 + /* 递归子目录），游戏启动
  早期把整个数据树扫一遍（数万次 FindFirst/FindNext，非死循环——限额别设太小）。

### 10.2 harness 关键坑（都已修，勿回退）
1. **stdcall 参数清理**：hook 返回时必须 esp += 4+4*argc（W32_ARGCOUNT 表），
   否则调用方 epilogue 全错位（症状：ret 到 0x72f4f0 之类垃圾地址）。
2. **GetProcAddress 必须按 WinXP 忠实返回 NULL**（FlsAlloc/
   InitializeCriticalSectionAndSpinCount 当年不存在，CRT 有回退路径）；返回
   假指针会让 CRT 以错误调用约定执行它 → 栈失衡无限递归（HeapAlloc×26 万）。
3. **环境块不能为空**：GetEnvironmentStringsW 返回空块 → CRT 转换 cch=0 →
   _setenvp 返回 -1 → R6009/R6010 致命退出。已注入 16 条 XP 风格环境。
4. **低 4MB 必须预映射 RW**：fs:[0] SEH 模拟（fs base 写不进 unicorn 1.x/2.x
   UC_X86_REG_FS_BASE 时为 0）+ LZW 字典在 0xF0000 区。
5. **FindFirstFileA 必须做通配符过滤**（fnmatch，忽略大小写；`*.*`/`*` 全匹配
   并附 "." 项），否则游戏按 *.zip 拿到整个目录清单 → 逻辑放大。
6. HeapReAlloc(hHeap,dwFlags,lpMem,dwBytes)：p=arg(2)、size=arg(3)；
   GlobalAlloc(uFlags,dwBytes)：size=arg(1)。
7. 未实现 API 返回 0 的隐患：GetLocalTime 不填 SYSTEMTIME → 调用方把栈垃圾
   当函数指针（症状：eip=栈地址）。时间族已实现真值。
8. 调试三件套：ring_on() 指令环（--ring，慢但给最后 48 个 eip）、
   _NMSG_WRITE/MessageBox 断点吐调用链、api-flood 限额 + find_pats 统计。

### 10.3 全链打通实测（run29，2026-10-16）

`uni_entry.py` 从真 PE 入口跑到 **0x436130**（5f9020 序列里 434cd0 之后两步），
4,057,748 次 API 调用，唯一未实现 API = FormatMessageA（已补桩）。实测顺序：

```
567990 引导壳 → 66918f WinMainCRTStartup（_setargv/_setenvp/_cinit/170 静态初始化器
  含 FMOD 音频管理器 ctor 5664b0、zip 档案扫描器 560790：递归全树挂到 32 个档案）
→ WinMain=FUN_005677d0（CRCMutex→GlobalMemoryStatus→GetSystemInfo→FUN_00566a10）
→ 5f9020：54d550(InitLZW) → 54bbe0("system.rpk") → 4b16a0 → **434cd0 VM 引导**
  （loadClass 走 55f8d0 档案查找→6635a0 zip fopen→classes.zip 真读 java/lang/
  String.class、Thread.class…）→ acceptBuild(1183)/version ✓ → 436130 →
  [DX 检查 MessageBox 停：Direct3DCreate9 桩返回 NULL]
```

本轮新钉死的事实：
- **WinMain = FUN_005677d0**（Ghidra `_WinMainCRTStartup` 伪码末行 `iVar2 =
  FUN_005677d0(pHVar1,uVar6,uVar3,uVar4)`）；线性反汇编 `call edi`@0x66930b 是
  失步误读（又一次「线性反汇编不可信」实例）。
- **FMOD 全部导出都是 __stdcall**：C API（F_API，FMOD_Memory_Initialize 5 参）
  与 C++ '?'-mangled 方法（QAG/YG）都由 callee 清栈。把 FMOD_Memory_Initialize
  当 cdecl 会让音频 ctor 的 ret 弹到 0（栈差 20 字节）——esp-trace 逐点定位。
- **CRT fopen 链会调 GetFileType 验柄**：返回 FILE_TYPE_UNKNOWN(0) → CRT 关柄
  → fopen NULL → zip 打不开 →「Class not found: java.lang.String」。GetFileType
  必须对 file 柄返回 FILE_TYPE_DISK(1)。
- zip 懒打开链：55f8d0（'/'→'\'、按 strrchr 前缀匹配档案、DAT_00890df0=档案数/
  df4=路径串块/df8=懒开柄表）→ 6637a0 → 6635a0（vtable=6642e0 填 7 个文件回调，
  [0]=6641e0=fopen(path,"rb"/"r+b"/"wb")）→ 667156 → 0x66d879(CRT _open) →
  CreateFileA。**exe 只导入 CreateFileA（无 W 版）**。
- 档案路径拼接用 '/'（DAT_006f5d08），查找路径 "./java/lang/String.class"；
  classes.zip 内部条目 = "java/lang/String.class"（带 java/ 前缀，实测命中）。
- 静态初始化器调用序可用 _initterm 断点（0x668881 XI 表 / 0x6688af 主表）跟踪；
  __security_init_cookie=0x676bc7；FMOD 音频 ctor=0x6e5690→0x5664b0。
- 调试增强：`--ring` 惰性武装（在指定 lookup 处才开环，避开树扫描千万指令）、
  low-exec/data-exec/fetch-wild 三种野跳转护栏（都吐栈链）、hook-bug 护栏
  （忘 ret() 立即报）、api-flood 限额 UNI_FLOOD 环境变量、audit_hooks.py
  （重复定义/漏 ret 静态审计——重复定义坑：旧 w32_GetSystemTimeAsFileTime 覆盖
  新版且忘 ret()，导致 hook 无限重入+esp 爬升）。

✗ 待办：DX9 检查绕过（假 d3d9 vtable 或分支 patch）→ 551cd0/435de0/54baa0/
  4342f0/4b1740/54c3a0 走完 = 数据引导全绿；然后 434cd0 之后用真 VM 跑指定类
  clinit/方法，与 tufa_vm 逐步对拍（pc/sp/locals/TOS/字段图）。

### 10.4 真 VM ⇄ 重建真值 差分套件（tools/uni_oracle.py，2026-10-16 首跑）

**方法**：引导到 data-boot-complete（0x436130 断点）后，对每个目标类依次
`649170(ecx=env, "类名"C串, verbose)`（load/define，**不**链接）→ 若
`[cls+8] bit0==0` 再 `655100(ecx=cls)`（Class::initialize：链接 + 尾部同步跑
`<clinit>`，THRD-CLASSVAR-INI ctx）→ 读静态槽比对。

**关键结构（本轮实测钉死）**：
- VM env = `[0x771050+0x104]`（0x771050 = 434cd0 的 ecx 宿主对象，.data 静态）。
- Class 对象：`+0x00` 数据块、`+0x04` 名字串、`+0x08` flags（bit0 已链接、
  bit1 clinit 完成、bit2 finalize 相关 → 实测链接后 =0x7）、`+0x0c` env、
  `+0x14` 符号表、`+0x18` **实例**字段数、`+0x28` 超类、`+0x2c` 接口向量、
  **`+0x34` = 指向 12B 静态槽向量的指针**（向量 = {count, base, 0}，槽在 base，
  与 656080 的 `esi=[cls+0x34]; [esi]=count; [esi+4]=base` 完全互证）。
  静态字段数源头 = `[[数据块+0x5c]+0xc]`；clinit 记录索引 = `[数据块+0x4c]`。
- 6492e0（loadClass 带缓存）要 **String 对象 ref**，C 串走 649170 直连。
- 649170/655100 都是 __thiscall（ecx=this），649170 另有 2 个栈参（ret 8）。

**首跑结果（26 类计划 = out_init.json 线性字面量 clinit ∩ .lasr.src FILD STATIC
槽序）**：batch1 24/26；修复两项后 **batch5 终跑 = 26 MATCH / 0 MISMATCH / 0 ERR**
（--reg-gui + MessageBox 非致命 + F 槽 ±1ulp 容差；8,027,780 次 API 调用）。
- `java.lang.Math.PI`：真 VM = **0x40490FD9**，out_init expr '3.141592' 重打包 =
  0x40490FD8 —— **out_init 的 F 字面量是截断十进制（有损）**，真值以容器位模式
  为准（tufa_vm 用 dump 位模式 0x40490FD9 ✓ 与真 VM 一致）。oracle 对 F 槽
  容差 ±1 ulp 并 WARN。
- **ISticker 卡点 = GUI native 注册器跑得太晚**：真 VM 报
  `"!Class::init: native implementation missing for java.gui.Texture.create()V"`
  （0x5c37e0 注册器 697 条属于引擎初始化 0x4c26c0→0x589e10 阶段，在
  data-boot-complete 之后）。oracle `--reg-gui` = 停点后手动
  `5c37e0(ecx=env)` 提前灌表 → ISticker 5 槽全对。
- **MessageBoxA 钩子必须非致命**（log+IDOK）：VM 错误框后游戏自己会继续
  （missing-native 时 Method.code=0 容错）；抛停会把 VM 丢在 clinit 中途，
  全局态污染导致后续类野跳（batch2 的 11 连败根因）。UNI_MSGBOX_FATAL=1 恢复旧行为。
- 大样本全对：GameRef 54 槽、CustomEventTypes 37、Chassis 23、ResourceRef 23、
  PartDecal 21、CarDifferential 12、ControlSet 12、Anim 11、String 9（引导期已
  链接，直接读）……
**意义**：out_init.json（tufa_vm 差分 2,713/2,713 的基准）本身对上了真 VM ⇒
真 VM ≡ out_init ≡ tufa_vm 传递闭合（字面量子集）。这是"重编译产物功能一致"
的第一条端到端真值链。

**下一步**：① 扩大计划面——非字面量 clinit（符号引用/数组/嵌套 new）用真 VM
槽值直接当期望（比 out_init 表达式求值更强）；② 方法级对拍：65f450 建帧 +
653880 跑指定方法（tufa_vm --call 的 11 条测试逐一在真 VM 重放）；③ ISticker
类"触发档案重扫"的依赖缺失根因；④ DX9 绕过后走完 5f9020 尾链。

### 10.5 全闭合差分（真 VM × tufa_vm 全静态面）（2026-10-16 深夜）

**tools/uni_dump.py**：停点后对 vm/vm_dump.txt 的 **1,066 个 clinit 类**逐类真 VM
load+link（真 clinit 执行）→ 静态槽全 dump → out_realvm_dump.jsonl
（**append+flush 断点续跑**）。字段级解码：I/F=裸 u32（**F 比位模式不比十进制，
绕开 out_init FD8 有损**）；String=真 VM 实例深读；数组=元素级。

**tools/vm_diff_real.py**：vm_dump.txt ⇄ 真 VM dump 差分，三口径：
1. **clinit 写字段**（全部 2,224 个 .lasr.src 的 `F 0` 记录 ∪ out_init 类 id +
   NT 表反解 (类,字段) 集合）→ 失配 = **真等价性缺陷**（红）；
2. 非 clinit 写字段 → **runtime-drift 信息桶**（真 VM 停点前跑过引导方法，如
   Gamelogic.actualRunningGearFlags 由 rec9 方法写 0x22 vs clinit-only 的 0
   ——不是缺陷）；
3. 引用类型裸值 → **nullity 比较**（两堆指针天然不同）；String/数组内容 →
   文本/元素级。

**String 布局实证**（strdbg 直读 0x2024bd48）：Instance{+0 class,+4 cap,+8 字段基}；
**interned String 的 fields[0] 直接指宽字符缓冲**（"CANCEL\0" 内联，非 char[]
实例！）——运行时构建的 String 才可能挂数组对象（首字 heap 区间判别双路径）。
数组实例：fields[0]=len、fields[1]=数据块{cap,base,count}（654ef0）。
类名读取：[inst+0]=class → [class+4]=名字 String 实例 → 同法解码。

**子集验证（5 类）**：Bet/Math/CommonStrings/Driver/Gamelogic → 189/193 ok，
4 例外全归口径（3 引用数组 + 1 runtime-drift）；String 静态全对（CommonStrings
53 条含长文本）。**全量 1,066 类跑批结果见 10.6。**

已知边界：
- `--reg-gui`（0x5c37e0，697 GUI natives）+ `--reg2`（0x488b60，parts 桥 98 条）：
  停点后引擎期注册器提前灌，否则相关类 clinit 报 missing-native（非致命但字段留默认）。
- 真 VM 每类 load≈秒级（大类含依赖重扫更久）；1,066 类全量 ≈ 30-90 min
  （jsonl 断点续跑抗崩）。
- 深比较二期（引用字段结构逐层展开、运行时 char[] String）未做。

### 10.6 tufa_vm 三缺陷修复：clinit 记录号 = CLSS[5]（2026-10-17 凌晨）

全闭合差分（§10.5）第一批就抓到**真缺陷**——但缺陷在 tufa_vm 侧，共三处，全部已修：

1. **★ clinit 记录号不是恒 0，而是 `CLSS[5]`**（CLSS chunk 第 6 个 u32；6/7/8/9 词长通用）。
   全语料验证：out_init.json 里 1,058 个 `<clinit>` 记录号与 CLSS[5] **1058/1058 全吻合，零例外**。
   **31 个类**的字段初始化块（`this.x=…`，构造器共用的 TREE 记录）占了 record 0，把真 clinit
   挤到 #1/#8/#13（GameMode@1、java.lang.String@8、java.game.item.Item@13、Init、HUD_Interface、
   IEngine/IMuffler/IPaintjob…）。旧版 tufa_vm 的 "record 0 无人认领即 clinit" 启发式对这 31 类
   **整个跳过 clinit** ⇒ 静态全零（GameMode.ST_COUNTDOWN=0 vs 真值 1）。
   修法：link_class 取 clinit_rec=CLSS[5]（nclss≥6），run_record 加 rec_idx 参数
   （旧签名第 3 参是 thisv——第一次补丁把记录号传成了 thisv，trace 一眼看穿）。
2. **vm_diff_init.py 只比对 `#0` 记录** ⇒ 这 31 类的 clinit 从未进差分——"2,713/2,713 全绿"
   是幸存者偏差。改为按 `method=="<clinit>"` 选记录。教训：**全绿先查覆盖面**。
3. **intern_w 把 java.lang.String synth 掉**：字符串驻留懒初始化 `synth_class("java.lang.String")`
   抢注全局类表 ⇒ 批处理里任何先于 String 触到字符串的类（实测 Bot）都会让真 String.class
   永远不被解析（[prog] steps=0、TCF_* 全 missing）。修法：优先 cls_find/load_class 真类，
   load 不到才 synth；fields[0]=宽字符缓冲的布局与真 VM 一致（§10.5 实证），无需改。

修后回归：**vm_diff_init = 1,058 类 / ok 2,797 / missing 0 / bad 0 / errors 0**
（steps 375,771→407,408：新增的正是 31 类真 clinit）。names0.txt 同步重建为
"out_init 里所有有 `<clinit>` 的类"（1,058；旧 1,066 里混着 8 个只有字段初始化块的类）。

### 10.7 真 VM 全量 dump 的车辆族崩溃：野跳进堆 + 护栏缺口（2026-10-17 凌晨）

**现象**：全量 dump 到 `vehicles.*`（1,058 类里 **921 个是 vehicles**，java.* 只 137）就崩——
`OSError: exception: access violation writing 0x1A400424000`（48 位**宿主**地址！）抛自
`uc_emu_start` 内部，随后 `UC_ERR_RESOURCE` 结束。单类隔离复现 ⇒ 不是状态累积。

**ring 铁证**（UNI_RING=96，指令环最后 96 条）：
`0x20abf9da 0x20abf9dc 0x20abf9de … 0x20abfa98` —— **在堆区（0x20000000+）以 2 字节步进
连续"执行"**，即把数据当代码跑；崩溃前最后 API = 一连串 `HeapAlloc` + `SetFilePointer/ReadFile/
ReleaseSemaphore`。⇒ **车辆零件类 clinit 经真 native（Part.configureType/createTypeEnd）间接
调到未初始化子系统的函数指针，跳进堆数据**；宿主的 access violation 是 unicorn JIT 翻译垃圾
字节码时写炸自己的 TB，所以不是 UcError 而是 OSError。

**护栏缺口（已修）**：`dataexec` 只挂了 `0x6E7000..映像末` 与 `0..0x10000`（低地址），
**堆/栈区没护栏**。给 `uni_harness` 补挂 `HEAP_BASE..+128MB` 与 `STACK_BASE..+16MB`
（同一 dataexec 处理，`_dedump` 语义不变）⇒ 野跳变成**可捕获的 StopEmulation**，
不再撞崩 emulator，也不需要 4 分钟一次的进程内重启。
另记两条工具护栏（本轮加的）：`uni_dump --retry-errors`（清掉 error 记录后重跑）、
崩溃时进程内 `reboot_vm()` + 毒类计数（≥2 次硬崩转 poison-skip）、`UNI_RING`/`UNI_SKIP_PREFIX`
环境变量；**`--fresh` 会删整个 jsonl——排查时别用**（本轮就这么丢过一次 251 条结果）。

**结论与下一步**：车辆族要在真 VM 里验证，必须**把停点推到引擎初始化之后**
（现在停在 0x436130 数据引导完；DX9 伪设备绕过后可继续跑 5f9020 尾链 → 4c26c0/589e10
引擎+GUI 注册），否则 configureType 的引擎全局是空的。
在此之前，车辆族的等价性只能靠 tufa_vm 侧（stub 版 clinit 全绿）+ 原生实现层（M4-C）分头看。

### 10.8 M5-diff 全量结果（2026-10-17 凌晨）

**真 VM 侧全量灌出**（`tools/uni_dump.py`，停点 0x436130 后逐类 load+link）：
1,058 类 → **914 ok / 144 err**。144 个 err **全部**是 `data-exec` 野跳（§10.7），
分布在 vehicles 的 6 个车系（Fantasy_Corus/Quaddro、Fujin/Hornet、Phoenix_RS/Takura_Tornado），
其余 ~777 个车辆类全部灌出成功，**单次跑批零崩溃**（护栏 + 进程内重启每 200 类 + 毒类计数）。

**三口径差分终值**（`tools/vm_diff_real.py`）：见下节实测数字（1,058 类定向重灌完成后）；
`bad` 里剩下的唯一语义项是 `Track.lastEngineVolume` —— 它的 clinit 调
`Sound.getVolume(CHANNEL_ENGINE)`，真引擎返回 1.0f、tufa_vm 桩返回 0，属
「clinit 依赖 native 返回值」，已在 skill 记为已知类）。runtime-drift 桶 22 条（真 VM 停点前
跑过引导方法所改，如 `java.io.Input.activeControlFileId`、`Gamelogic.actualRunningGearFlags`）。

**本轮三个"假失配"（全部是工具侧，不是语义差）**：
1. **0 长数组没有数据块**：真 VM 里 `blk == 0`，旧 read_array 返回 None → 空 `{}` 字面量
   （14 个 frontend 类的 stringTable）被误报成 null。修：blk==0 → 返回空表。
2. **`[L` 元素解码**：不能用未验证的 `class_name()`（`[class+4]=名字` 假设）做门 →
   改 `read_string_checked()`（可打印性校验）先试 String、失败再试 Class 路径
   （`Driver.drivers`/`Gamelogic.rankingNames`/`Trial.legendNames`… 42 类）。
3. **dump 转义集**：tufa_vm 打印 `\s \q \t \n \r \ \uXXXX`，差分器的 unesc 少解一种
   就是一个假失配（`Replay.CRLF` 真实 `\r\n` vs vm `\r\n`）。
   工具：`tools/arrfix.py`（扫出解码未完成的记录 → 从 jsonl 里摘掉 → 定向重灌）、
   `uni_dump --class-file`（按文件给类清单，避免命令行长度/引号事故）。

**下一步（车辆族 144 类）**：真解 = 停点后移到引擎初始化之后（5f9020 尾链 → 4c26c0/589e10），
让 `Part.configureType` 走真实现时有完整子系统；过渡手段 = 注册后把易崩 native 的
fnptr 改成桩（hook FUN_00652910 的注册参数并替换 fnptr —— 签名 5 参 thiscall，
约定待验：看 FUN_00483f50 结尾是 `ret` 还是 `ret n`），这样拿到的是「Java 层 clinit 语义」
对比（与 tufa_vm 的桩口径一致），而不是含 native 副作用的对比。

#### 10.8.1 终值（1,058 类定向重灌后，2026-10-17 04:0x）

```
classes: ok=912 bad=2 err/skip=144     fields: ok=4880 bad=19 skip=0 runtime-drift=2
```

**19 条 bad 全部归因完毕（0 条未解释）**：
- **18 条 `java.util.Config.*`** = 真引擎**读了磁盘上的玩家配置**（`C:\Games\LASR` 下真实安装）并覆盖了
  代码默认值：`menuMusicVolume` 0.9→0、`metricSystem` 0→1、`Sound_Max_3D_channels` 16→32、
  `steer_controller_range` 240→720.3、`player_transmission` 1→4…；而 out_init⇄tufa_vm 的**默认值本身
  全绿**（0.9/2/0.1/240.0/16/0/“en”）。⇒ 环境差，不是语义差。
- **1 条 `Track.lastEngineVolume`** = clinit 调 `Sound.getVolume(CHANNEL_ENGINE)`，真引擎 1.0f、桩 0。
- `Config.language` 真侧解码成 `'en' + 一长串 'e'`（缓冲无 NUL 终止，我的定宽读取越界）——
  前缀与 vm 的 `str:en` 一致 ⇒ 解码边界，非语义差（下轮补：按 UTF-16 单位对齐扫 NUL，或读 String 长度字段）。

**又一条比对口径（本轮第 4 个假失配）**：池字面量带本地化前缀 `$N|text`，真 VM 的
String 对象里**只有文本**（`CommonStrings.OK` 真实 `OK` vs 池 `$30|OK`；`ResultsWindow`
真实 `You win!` vs `$1|You win!`）⇒ 差分器加 `norm_text()` 剥前缀，一次消掉 ~30 条假失配。

#### 10.9 车辆族过渡方案：注册时给 native 打桩（2026-10-17 凌晨）

**动机**：车辆零件类 clinit 经真 native（`Part.createTypeBegin/configureType/configureVisual/
createTypeEnd`、`ResourceRef.duplicateMesh`、`RenderType.changeResource`、`GameRef.setFlags`）
摸到停点处尚未初始化的引擎子系统 → 野跳（§10.7）。要拿到「Java 层 clinit 语义」的对比值，
最省事的口径是**让真 VM 也走桩**（与 tufa_vm 的桩口径对齐）。

**手法**（`UNI_STUB_NATIVES='*.configureType,…'` env，uni_dump 实现）：
- 桩代码页：`uc.mem_map(0x30000000, 0x1000)` + 写 `31 C0 C3`（`xor eax,eax; ret`）。
  返回 0 对所有 desc 都安全（int/ref=0、float=0.0f、void 无返回）。
- **约定依据**（实测，别再猜）：注册器调用点 `0x488b7f-0x488b99` = `mov ecx,[0x771154]` +
  4 个 push（fnptr/desc/name/class 右到左）+ `call 0x652910` ⇒ `__thiscall`，参数在栈上；
  而 native 自己**以裸 `ret` 结尾**（实测 `0x4840c5`，`int3` 填充到下一函数）⇒ 调用方清栈，
  桩只需 `ret`、不必 `ret n`。
- 安装：hook `0x652910` 入口，读 `[esp+4]`(class cstr)、`[esp+8]`(name)，命中就把
  `[esp+0x10]`(fnptr) 改写成桩地址 —— 注册表里存的就已经是桩，方法调用一次都不会进真实现。
- 实测命中的 11 条注册：`java.game.parts.{Part,Chassis}.{createTypeBegin,configureType,
  configureVisual,createTypeEnd}` + `java.util.resource.{GameRef.setFlags,RenderType.
  changeResource,ResourceRef.duplicateMesh}`。
- **单类验证**：`Hatch_S2_Rim_U_ST_stock` 从 `data-exec` 崩溃 → `ok=1 err=0`，
  静态值 `sizes=[100.0,200.0,800.0,2000.0]`（位模式 1088421888/1096810496/1114636288/
  1133248512）与 tufa_vm dump **逐位一致**。
- 口径提醒：打桩后的真 VM 值 = 「Java 层 clinit 语义」，与 tufa_vm 的桩口径可比；
  凡 tufa_vm **实现了**的 native（资源系统 17 个）仍可能有口径不对称的差，读差分时按此分类。

#### 10.10 打桩口径全量终值（2026-10-17 拂晓）

统一口径（全程 `UNI_STUB_NATIVES` 打桩，1,058 类一次灌齐）+ CRLF 门修正后重灌 Replay：

```
[dump] DONE ok=1058 err=0                     ← 真 VM 侧零错误零崩溃，全部灌出
classes: ok=1056 bad=2  err/skip=0            ← bad = Track / Config
fields:  ok=5384 bad=19 skip=0 runtime-drift=2
```

**19 条 bad 的完整归因（全部可解释，0 未解释）**：
- **18 条 `java.util.Config.*`**（含 `language`）：真引擎读磁盘玩家配置覆盖默认值 —— 环境差；
  out_init⇄tufa_vm 的默认值本身全绿。
- **1 条 `Track.lastEngineVolume`**：clinit 调 `Sound.getVolume(CHANNEL_ENGINE)`，真引擎 1.0f vs 桩 0。
- **1 条 `Replay.CRLF`**：真值 `"\r\n"`，我的可打印性门把 `\r \n` 当垃圾拒了（`read_string_checked`
  改：只拒 NUL 与「其他控制符占比 > 1/4」）。
- runtime-drift 2 条（`Input.activeControlFileId`、`Config.video_settings`）：停点前引导方法所改。

**关键结论**：真 VM（桩口径）× tufa_vm（桩口径）在**全部 1,058 个 clinit 类的静态面**上
**5,384 项一致 / 0 项未解释差异**；加上 `vm_diff_init`（tufa_vm × out_init 字面量）2,797 项全绿
与 oracle（真 VM × out_init 字面量）26/26，三条独立差分链在同一点闭合
——**「重编译产物功能一致」在静态初始化面上已无已知缺口**。

### 10.11 下一工作流：方法级对拍（探针类驱动，设计定稿）

**目标**：把验证面从「静态初始化」推到「任意方法行为」——重放 tufa_vm 的 11 条方法测试
+ 803 条构造器 sweep，在真 VM 里跑出同样结果对表。

**机制（不手工搭帧，复用已 proven 的 load→clinit→dump 管线）**：
1. **生成探针类** `java.test.Probe`：静态字段若干（I/F/L 各若干，作结果槽）+ `<clinit>` 里
   逐条「压字面量实参 → CALL 目标方法/构造器 → PUTFIELD 存结果」。生成器 `tools/tufa_gen.py`：
   用 `tools/tufa_src.py` 的 DSL 逐行发射（**池索引必须自己按发射顺序累加**：S/C1/C2/NT 都占槽），
   发完用 `tufa_src --compile` + `--roundtrip` 自校验。
   备选：以 `java.game.Bet.class.lasr.src` 为模板做变换（它已有 6 个静态 int + 干净 clinit）。
2. **把探针塞进真 VM 的类路径**：真 VM 只认游戏目录的 `classes.zip` ⇒
   - 先在 scratch 造一份「原 zip + java/test/Probe.class」；
   - `uni_harness._resolve()` 加一条路径重定向（env `UNI_FILE_REDIR=C:/Games/LASR/classes.zip=<scratch zip>`），
     只对命中的绝对路径生效；不改用户安装目录（**硬约束**：只读原版安装）。
3. **双跑对表**：`uni_dump --class java.test.Probe`（真 VM）vs
   `tufa_vm --cp <临时树> --index <临时索引> --clinit java.test.Probe --dump`，
   再用 `vm_diff_real.py` 的两侧 dump 口径比（探针文件名/格式与 vm_dump 对齐即可复用）。
4. **用例来源**：`vm/vm_method_tests.py`（11 条）+ `vm/vm_ctor_sweep.py`（803 条）的
   (类, 方法, desc, 实参) 四元组；实参只取**字面量**（与 out_init 同口径），引用型实参二期。

**护栏**：探针类不进 `extracted/`、不进 out_src_v2（生成物放 `probe/`）；对拍结果落
`out_method_diff.json`，与静态面的 `out_diff_final5.log` 分家。

#### 10.11.1 方法级对拍已跑通（2026-10-17）

**机制落地**：`tools/tufa_probe.py`（探针类生成）+ `tools/probe_diff.py`（一键双跑对表）。
真 VM 侧注入靠 `uni_harness._resolve` 的**路径后缀重定向**（`UNI_FILE_REDIR`，
把 `java/classes.zip` 指到 scratch 副本，**玩家安装目录只读**）+ `uni_dump` 的
`UNI_VMDUMP`/`UNI_OUTJSONL` 两个 env（避免污染主 jsonl 与主类清单）。

**实测（`probe/out_method_diff.json`，5/5 全绿）**：

| 目标方法 | 实参 | tufa_vm | 真 VM |
|---|---|---|---|
| PointPairSeries.TwoFloatOperands_Switch(IFF)F | 0\|3\|4 | 12.0 (0x41400000) | 12.0 (0x41400000) |
| PointPairSeries.TwoFloatOperands_Switch(IFF)F | 1\|3\|4 | 3.0 (0x40400000) | 3.0 (0x40400000) |
| PointPairSeries.Interpolate(FFFFF)F | 5,0,10,100,200 | 150.0 (0x43160000) | 150.0 (0x43160000) |
| parts.Rim.CalculatePhysRadius([F)String | [195,55,14,7] | "0.698" | "0.698" |
| parts.Rim.CalculatePhysMass([FFF)String | [195,55,14,7],2.3,1.7 | "526.026" | "526.026" |

覆盖了：int/float 字面量实参、**浮点数组字面量实参**（`元素压栈 → INT n → NEWARRAY 描述符 →
ARRAY_INIT n`）、float 与 **String 返回值**（String 结果字段声明成 `Ljava.lang.String;`，
两侧 dump 都带 `str:` 文本，直接按文本比）。

**生成探针类时必须守住的三条**（都是本轮踩出来的）：
1. `PUTFIELD_STATIC`/`FIELD_REF_STATIC` 的操作数是 **C2 fieldref（类引用 + NT）**，
   不是 NT 条目本身（用 NT 索引 ⇒ `resolve_member` 失败 ⇒ Runtime error）。
2. 指令必须插在 REC 的最后一条 **RETURN 之前**——插在之后 = 死代码（dump 全 0、steps 不变）。
3. 池索引 = 条目在 CONS 列表中的位置（S/C1/NT/C2 各占一槽；池尾可能夹一行空行）。

**下一步**：把 `vm_method_tests.py` 剩余 6 条（对象/引用实参、Object 返回值）与
`vm_ctor_sweep.py` 的 803 条构造器搬进探针口径（构造器用例需要「字段图对比」二期），
再把探针点扩大到 random 采样（用 out_init 的字面量当实参自动生成用例）。

#### 10.12 调用点语料自动抽取（tools/callsite_corpus.py）

**做法**：复用 `tufa_src.RecParser` 把 2,224 个 `.lasr.src` 的每条 SRCREC 解析成 AST，
遍历出所有 `CALL` 节点，筛「**静态 + 实参全是字面量**（null/bool/int/float）」+ **排除 native**
（native 表 = `out_vm_natives.csv`（列序是 registrar,cls,method,sig！）× 直接扫原生活料
`FUN_00652910("cls","meth","sig",…)` 取并集——只信 CSV 会漏），去重后落
`probe/spec_corpus.json`，直接喂给 `tools/probe_diff.py --spec-file … --merge-simple`。

**两点工程注意**：① `.src` 池索引 = 从 `CONS n` **之后**开始数的条目位置（把 `TUFA` 头行算进去
会整体错一位，`resolve()` 全失败——本轮踩过）；② 调用点里绝大多数实参是局部变量/字段/表达式
（`LIT 11 k` = LOCAL_LOAD），所以字面量静态调用天然稀少：全语料去重后**仅 5 条**，
其余价值要靠「方法 × 字面量扫描」（下一轮）。

#### 10.11.2 探针跑的「native 口径」教训（2026-10-17）

**症状**：8 例探针全绿后，把 Gamelogic 的 `getPrestigeValue/getPlayerRank` 加进同一个探针类，
RealVM 侧**后面所有用例集体失真**（`Count()=0`、`getInput(1)=0`，上一轮还是 3 / 2.0）。
根因：真 VM 在探针类里**首次加载 Gamelogic 会跑它的 `<clinit>`**，而该 clinit 调 4 个真 native
（`Texture.create`、`SfxRef.createFromFile`、`GameRef.setFlags`、`RenderRef.create`）——
停点处引擎子系统未初始化 ⇒ clinit 出错/写坏状态 ⇒ 后续用例读到垃圾。
⇒ **探针跑必须给真 VM 开与 tufa_vm 相同的桩口径**：`probe_diff` 现在自动注入
`UNI_STUB_NATIVES`（默认 `*.create,*.createFromFile,*.setFlags,*.configureType,*.configureVisual,
*.createTypeBegin,*.createTypeEnd,*.duplicateMesh,*.changeResource,*.add,*.remove,
*.createNativeInstance`，可用 `PROBE_STUBS` 覆盖）。

**教训**：① 一个探针类里放多个用例时，**用例之间会通过类加载互相污染**——要么保证目标类在
停点处能干净 clinit（= 打桩），要么**分块**（`probe_diff --chunk-size N`，每块一个
`java.test.ProbeK`，一块崩了不拖累其它块）；② 判「真差异 vs 口径差」的快捷手段：
把可疑量拆成更小的探针（如 `getPrestigeValue()` 两边都 0 ⇒ 不是存档差异；`getPlayerRank()`
一边 61 一边 0 ⇒ 是那次 clinit 污染），tufa_vm 侧还能用 `--call ... --args new:Class` 单侧复现。

#### 10.14 native 层第一波：java.io.File + Config 读盘闭环（2026-10-17 下午）

**为什么要它**：`tools/native_hist.py`（新）扫全语料 .tasm 常量池（C1/NT/C2 → CALL 操作数）统计
native 调用点：**注册 1156 个，Java 语料真正调用 238 个 / 2124 处** → `out_native_hist.csv`。
榜首 `java.io.File.readInt`(196)/`readFloat`(120)/`write(I)`(91)（7 个方法 ≈ 460 处）——配置/存档
全靠它，且它是「真 VM 读到玩家磁盘、tufa_vm 读到 0」那 18 条 Config 差分的根因。

**实装**（`vm/tufa_vm.c`，语义抄 `out_native_decomp`）：`open/close/readInt/readFloat/readResID/
readString/write(I)/write(F)/delete/exists`。安全口径：**读走 `--file-root`（安装目录，只读），
写走 `--file-write-root`（沙箱）**——重建产物永不写玩家真存档；另加 `--call` + `--dump` 组合
（跑完某方法后 dump 该类静态）与 `UNI_FILELOG=1` 读值/调用点日志。

**四条实测出来的 native 语义**（都对过真值，别再猜）：
1. **SDAT 头规则**（`FUN_004036f0`）：open 后读 8 字节，`byte[6]==3` ⇒ 结构化数据文件，数据从
   偏移 **12** 起（`save/game/options` 就是这形；`locale/.conf` = 2 字节 "en" + 零填充 ⇒ 偏移 0）。
   反解证据：真 VM `Config.SAVEFILEID = 0xFEDCBA98` = 文件偏移 12 的 dword、`SAVEFILEVERSION = 37`
   = 偏移 16 ⇒ 位置 12 ✓。
2. **readInt/readFloat = 当前位置起 4 字节小端，短读零填充**（decomp 里局部 `= 0` 初始化）。
3. **readString 没有长度前缀**：从当前位置逐字节填满 **255** 上限，越界后**重复首字节**；首字节读不到
   或为 NUL ⇒ 返回 **null**（不是空串）。真值：`Config.language = 'en' + 'e'*253`（`locale/.conf`
   只有 "en"）✓ 逐字符一致；`userID/lastRoomName` 真侧是 **null** ✓（我第一版把它们做成空串，错）。
4. **`File.name` 是实例字段**（File 唯一实例字段，超类 `java.lang.Native`），native 侧用
   `find_field(o->cls,"name",1,...)` 取路径；写模式 `mode & 1`、`mode>>1` = 0/1/2 = 读/写/别的。

**战绩**：`Config` 163 个静态字段从 19 条差分 → **161 一致 / 2 已知**（`player_display` 见下、
`CONFIGFILE` 是提升器缺陷）。18 条「真引擎读盘」差分全数消解（metricSystem=1、player_transmission=4、
Sound_Max_3D_channels=32、menuMusicVolume=0、head_move_steer=0x3EBF340D、sound_occlusion=1 …）。

**两条"残留"的最终定性（都不是缺陷，已实测闭环）**：
- `Config.player_display`：真 0 / 只跑 clinit+load 的我们 1。clinit 里它是**无条件**赋值 `LIT 5 1`，
  而新工具 **`tools/field_writers.py java.util.Config player_display`** 查出写者有**两个**：
  clinit 与 **`Config.init()V`**（`IFTHEN FIELD 20 222 { … PUTFIELD 32 238 - LIT 5 0 … }`）。
  真 VM 的引导跑过 `init()`，我们的探针只跑 clinit+load ⇒ 值停在 1；**实测让 tufa_vm 也跑
  `--call java.util.Config.init` ⇒ `player_display = 0`，与真值一致** ✓。
  ⇒ 差分器的 drift 规则只看"clinit 没写过"的字段，**漏掉"clinit 与后置方法都写"这一类**；
  以后遇到"clinit 无条件赋值却与真值不符"，先用 `field_writers.py` 查写者集合再定性。
- `Config.CONFIGFILE`：**两边逐字节相同**（17 字节，hex `736176655c67616d655c6f7074696f6e73`）。
  上轮"真 17 / 语料 19 ⇒ 提升器多解码一层"是**我的比较错误**：tufa_vm dump 会把 `\` 打成 `\\`，
  我没按文档的转义集（`\s \q \t \n \r \\ \uXXXX`）还原就与真值比。**提升器无辜**。
  ⇒ 比 dump 里的字符串字段前必须先 `unesc`（`vm_diff_real.py` 一直是这么做的）。

**Config 收尾口径**：163 字段 = 162 逐位一致 + 1（`player_display`，clinit 后被 `init()` 改写，
补跑 init 即一致）⇒ **零未解释残差**。

**方法教训**：判 native 是否生效，别拿 `--clinit` 跑（clinit 不调 `load()`）；要用 `--call` +
`UNI_FILELOG` 看真实读值序列，再与真 VM dump 逐字段对表。

#### 10.15 ★ 静态面满分：引导驱动重放（2026-10-17 傍晚）

**问题定性**：把 File 层做完后重跑全量差分，那 18 条 Config 差分**一条没动** —— 不是 native 没生效，
而是**跑批范围不同**：真 VM 的 dump 是「引擎引导已跑完 `Config.init()` + `Config.load()`」之后的状态，
而我们的批量只跑每个类的 `<clinit>`（`load()` 根本不在 clinit 里，它由引擎/主流程调）。
判据工具：**`tools/field_writers.py <类> <字段>`**（本轮回写）——`Config.video_settings` /
`Input.activeControlFileId` 的写者都是 `Config.load()`；`Config.player_display` = clinit + `init()`。

**做法**：`tufa_vm --batch` 支持 `UNI_DRIVERS=Class.method,…`，在逐类循环**之前**把这些引导方法
按序重放（`run_driver()`：静态/实例自动判、`inst_new` 造 this、可选 desc `()V/()I/()F/()Z`、
独立步数预算）。实测：
```
UNI_DRIVERS="java.util.Config.init,java.util.Config.load"  tufa_vm --batch --file-root C:/Games/LASR
[driver] java.util.Config.init steps=1968 errs=0
[driver] java.util.Config.load steps=714 errs=0
```
**结果（真 VM ⟷ tufa_vm 全量差分）**：
```
修 File 层前：classes ok=1056 bad=2 ； fields ok=5384 bad=19  runtime-drift=2
File 层 + 驱动：classes ok=1058 bad=0 ； fields ok=5403 bad=0   runtime-drift=2   ★★
```
最后一条 `Track.lastEngineVolume`（真 1.0f / 我 0）= `Sound.getVolume(CHANNEL_ENGINE)` 依赖 native
返回值 ⇒ 顺带实装 **`java.sound.Sound` 通道音量模型**：`setVolume`（`FUN_0040eb60` = 按通道号 switch，
把 float 存进 per-channel 全局 `DAT_0074230c/…`）写成一张 16 槽表（默认满量程 `0x3f800000`），
`getVolume` 读回 ⇒ 该项归零。

**残留两条 informational（都不进 bad 桶）**：
- `java.io.Input.activeControlFileId`：真 1 / 我 0。写者 = `Config.load()`（读文件的一个 int）+
  `controlOptions.ButtonApply_onAction`；真侧引导里引擎自己也会按输入设备写 ⇒ 属引擎侧 init 行为。
- `java.util.Config.video_settings`：真侧掩码里是裸引用指针、我侧解成数组 ⇒ dump 表现差（两边都非 null）。

**方法论**：差分出现「同族字段成片差、且都是默认值 vs 真值」时，**先怀疑跑批范围（驱动没跑）**，
用 `field_writers.py` 找写者 → 把驱动补进 `UNI_DRIVERS` 重跑，别去改 native。

#### 10.16 游戏 Java 入口能在 tufa_vm 里跑进主循环（2026-10-17 傍晚）

静态面满分之后，把驱动从 `Config.init/load` 往**游戏自己的 Java 入口**推：
`Config.startup()` 的实现 = `Config.root = Class.forName(startupClass).newInstance()`，
`startupClass = "java.game.Init"`（池里 `S "java.game.Init"`）⇒ 入口 = **`java.game.Init.<init>`**
（record 2，**RAWREC**，tufa_vm 直接执行）。

```
UNI_DRIVERS="java.util.Config.init,java.util.Config.load,java.game.Init.<init>"
[driver] Config.init  steps=1968      errs=0
[driver] Config.load  steps=714       errs=0
[driver] Init.<init>  steps=20000000  errs=13   ← 打满 20M 步预算
[batch done] errors=13 native_stubs=2,496,808 native_impl=216
**
⇒ tufa_vm 把游戏 Java 入口**跑进了它的主循环**（20M 步内 250 万次 native 调用），
   全量静态差分仍是 classes 1058 ok / fields 5403 ok / 0 bad（入口不污染静态面）。
```

**`Init.<init>` 的运行期调用序**（池解析，可直接当"下一步实现什么"的依据）：
`System.getCommandLineParameters()` → `Gamelogic.<init>([Ljava.lang.String;)` → `GfxEngine.init()`
→ `showSplashScreen(String)` → `Input.init()` → `Player.<init>` → `Input.setController/reset(ControlSetState)`
→ Hotkey 注册族（`Hotkey.<init>(IILHotkeyEventHandler;II)`、`activate()`）→ `Sound.init/eventFileOpen/
addMusicSet` → 主循环里的 `Gamelogic.status` 分支 + 音量热键。

**13 个错误的成因（都已定位，不是解释器缺陷）**：12 × `Instance::getArrayElement: null array ptr`
+ 1 × 步数预算耗尽。前者全是**桩返回 0 的引用级联**：`getCommandLineParameters` 返回 null ⇒
`Init.<init>` 立刻读它的 `length`/元素。已按"不臆造数据"的口径修掉第一条——**返回合法的空数组**
（`nat_sys_cmdline`，`arr_new("Ljava.lang.String;", 0)`）。余 12 条紧跟
`Controller.addMapping(IIIFFFF)I` 之后，来源是另一个数组返回型 native（`Input.activeAxis(I)[I`
一类的候选），需要逐个按 decomp 建模。

**运行期 native 排行（`vm/native_calls.tsv`，20M 步窗口）** —— 比静态直方图更贴功能路径：
`Part.createTypeBegin/createTypeEnd` 各 671、`Part.configureMesh` 619、`configureVisual` 592、
`configureTexture` 457、`RenderType.duplicate` 92、`Class.forName` 85、`Class.newInstance` 84、
`ResourceRef.duplicateMesh` 84、`RenderType.changeResource` 84（外加 `File.open/readFloat/close`）
⇒ **真正卡住功能路径的就是"Part 渲染建造机"家族**（与静态面已知的唯一硬骨头一致）。

**方法论**：每实现一个 native 都要问"有没有真值可比"——静态面用 out_realvm_dump；运行期没有真值时，
模型只能做到"不崩 + 不臆造"（如空数组），并在文档里标成模型而不是事实。

**⚠ 驱动范围的边界（本轮踩到，务必记住）**：驱动不是"越多越好"，**必须与参考 dump 的停点一致**。
真侧 dump 停在 `data-boot-complete 0x436130`，那一刻引擎已跑过 `Config.init()`+`Config.load()`，
但**还没跑游戏的 Java 入口**。实测把 `java.game.Init.<init>` 也加进驱动后（它进了主循环、真的开始建
Player/GfxEngine/Input/Sound），差分立刻**回退**：`classes 1056/2`、`fields 5386/2`、`runtime-drift 17`
——因为此时我们跑得比参考侧**多**（如 `Gamelogic.player` 真侧还是 null，我们已 `new Player()`）。
⇒ **两个口径分家**：
- **静态差分口径**（与真 VM 对表）：`UNI_DRIVERS="java.util.Config.init,java.util.Config.load"`
  ⇒ `classes 1058 ok / fields 5403 ok / 0 bad`。
- **运行期覆盖口径**（衡量"重建产物能跑多远"）：再加上 `java.game.Init.<init>`
  ⇒ 进主循环 20M 步 / 2.5M native 调用 / 13 已知错误（12 条桩返回 null 的数组级联 + 1 条预算耗尽）。

#### 10.17 ★ 运行期实验挖出一个真解释器 bug：DUP_X2 栈序（2026-10-17 晚）

**怎么挖到的**（可复用套路）：`Init.<init>` 报 12 次 `Instance::getArrayElement: null array ptr`。
① 给 `arr_new()` 加 `UNI_ALLOCLOG` ⇒ 看到 `new Hotkey[12]` **分配成功**（desc/len/cls 全对）；
② 给 **ARRAY_STORE** 加操作数日志 ⇒ 失败点 `arr=00000000 idx=0x03700…(指针) val=0x03700…(指针)`
⇒ 三个操作数全错位 ⇒ 嫌疑落到洗栈指令；
③ 该 idiom = `INT_LITERAL k / FIELD_REF_QUICK[globalKeys] / NEW…DUP…INVOKESPECIAL / DUP_X2 / ARRAY_STORE`
⇒ 读**原始 opcode 处理器的汇编**（`0x653a34` 起）拿权威序：
```
DUP_X1 (0x2b): pop v1 → pop v2 → push v1, v2, v1          ✓ 与我们一致
DUP_X2 (0x2c): pop v1 → pop v2 → pop v3 → push v1, v3, v2, v1   ✗ 我们写成 v1, v2, v3, v1
```
（辅助函数语义也由此定案：`FUN_00652f10`=POP、`FUN_00652f40`=PEEK、`FUN_00652fc0`=PUSH(val)；
opcode case 表在 `0x653880`，case 体在 `0x6539…0x653d…`。）

**修复与验证**：
```
Init.<init>：errs 13 → 1（剩下的 1 = 20M 步预算耗尽，非错误）
ARRAY_STORE：arr=00000000 → arr=02EDEBD8 idx=0/1/2… 全部有效
全量回归（正确驱动口径）：classes 1058 ok / 0 bad、fields 5403 ok / 0 bad  ← 未被打破
```
⇒ 这是**第一个由"运行期覆盖实验"直接挖出并修复的解释器缺陷**（静态面看不见它：该 idiom 只在
`Init.<init>` 这种洗栈构造里出现）。同一轮还修了 `nat_sys_cmdline` 的描述符（漏了 `[` 前缀，
`arr_new("Ljava.lang.String;")` 会建出非数组类）。

**教训**：栈洗牌类 opcode（DUP/DUP_X1/DUP_X2/DUP2/DUP_X2…）**必须逐个对原始 case 体的汇编**定序，
注释里写 `TODO verify` 的就等于埋雷；`arr_new`/`ARRAY_STORE` 级别的操作数日志是把"语义错"与
"桩返回 null"分开的最快手段。

**同族第二个（同类审计挖出）**：`DUP2 (0x2d)` —— 原始实现（`0x6539be`）是 **peek 顶两个、先 push v2 再 push v1**
（标准 DUP2：`[.., v2, v1] → [.., v2, v1, v2, v1]`），我们旧代码写成"连推两次 v1"（`[.., v2, v1, v1, v1]`）。
修后静态面回归仍旧满分、`Init.<init>` 错误数不变（都是预算耗尽）。⇒ **洗栈家族一次性审计完**：
`POP(0x29)=1 弹`、`DUP(0x2a)=peek+push`、`DUP_X1(0x2b)=弹2推3（v1,v2,v1）`、`DUP_X2(0x2c)=弹3推4（v1,v3,v2,v1）`、
`DUP2(0x2d)=peek2 推4（v2,v1,v2,v1）`，全部对过原始 case 汇编。定位手法：`tools/vm_opcode_cases.py`
（读 `0x654b70 + 4*(op-1)` 的跳转表拿 case 体地址）→ `tools/dis_va.py` 反汇编该 case。

**"能跑多深"的度量**：把驱动预算变为可调（`UNI_DRIVER_BUDGET`，默认 20M 步）。实测
```
UNI_DRIVER_BUDGET=200000000 → Init.<init> steps=200,000,000 errs=1（唯一错误=预算耗尽）
                              8.4 s 跑满，零崩溃、零其它错误
```
⇒ 在 native 全桩的环境下，游戏 Java 侧进入**无限主循环**（预期行为，游戏本来就不返回）；
`Gamelogic.<init>` 之后的每一步都受 engine 侧 native 制约 ⇒ **功能等价的长杆在 native/引擎层**，
而运行期 native 榜首（Part 渲染建造机家族）就是它。

#### 10.18 `System.currentTime` 模型让游戏 Java 启动**跑完**（2026-10-17 晚）

**症状**：主循环实验在 `Init.<init>+995` 卡死（`UNI_SAMPLE` 采样器每 100 万步都打同一帧）。
**定位**：采样器 + `[natwin]` 窗口日志（UNI_SAMPLE 下打第 1,000,000~1,000,200 步内的每次 native）
⇒ 该循环每 ~8 步调一次 `java.lang.System.currentTime()F`；看 `Init.<init>` 的 token：
```
INVOKESTATIC currentTime / LOCAL_LOAD t0 / FSUB / INT_LITERAL 3 / I2F / FLT / JMP_NE out / JMP back
```
= **开场 splash 的 3 秒等待**，而我们的 `currentTime` 是桩返回 0 ⇒ 差值恒 0 ⇒ 死循环。
**修**：实装 `nat_sys_currentTime`（CPU 秒，`clock()` 起算）；`[clock]` 日志实测 ~0.39 µs/调用
⇒ 3 秒 ≈ 60M 步。
**结果（实测）**：
```
step  64,500,000  → 采样帧变成 java.game.Gamelogic.<init>+1108（越过 splash 门）
driver Init.<init> steps=92,183,203 errs=22      ← 驱动跑完返回（12,017,188 次 native 实现调用）
```
⇒ **游戏 Java 侧从入口一路跑到"构建开始菜单窗口"**（错误帧集中在 `StartWindow.<init>+0x467`）。

**剩下 22 个错误的性质**（都已定性，不是解释器缺陷）：6× `null.fieldref java.game.frontend.
loadInProgress`（**空接收者**读实例字段）+ 16× `Thread::callmethod: null.methodcall`
（`java.gui.Rotating.setRColor`、`Background.setColor_*`）⇒ 全是**桩返回 0 的 GUI 对象**被下游直接使用
⇒ 下一批 native 的靶子 = **gui 构建族**（与运行期排行里的 Part 族并列）。

**自我纠错**（写进记录以免后人重犯）：上一小节我据"20M 步仍在同一帧"推断"桩环境下无限主循环"，
其实**采样只看了前 12 行**（`head -12`）；实际是 splash 等待要 ~60M 步。判"是否死循环"必须看**尾部**采样，
不能只看开头。调参：`UNI_DRIVER_BUDGET`（驱动步数预算）、`UNI_SAMPLE`（采样器 + natwin/clock 日志）。

#### 10.19 gui 构建族诊断：条件 trace + 窗口日志（2026-10-17 晚）

**工具**（都挂在 `UNI_SAMPLE` / `--trace` 下，不污染常规跑批）：
- **条件 trace** `UNI_WIN="<类>:<off0>:<off1>"`（配合 `--trace`）：只打印该帧在该偏移窗口内的指令
  —— 长跑（90M 步）也能精确定位某一处调用点；
- **`[guiwin]`**：当*调用方*帧是 `StartWindow.<init>` 时打印每次 native 的返回
  ⇒ 一眼看出哪个 native 是桩、返回了什么。

**实测定位**（`StartWindow.<init>+0x462`）：
```
044e op 15 FIELD_REF_QUICK 125   ; this.loadingAnim (java.gui.Embedded)
0453 op 10 INVOKE 176            ; Embedded.getObject()Ljava.gui.Component;
0458 op 01 CAST 180              ; (java.game.frontend.loadInProgress)
045d op 06 FLOAT_LITERAL 0.2f
0462 op 10 INVOKE 184            ; loadInProgress.setTransparency(F)V   ← 空接收者
```
= `((loadInProgress)this.loadingAnim.getObject()).setTransparency(0.2f)`。
`[guiwin]` 显示 `StartWindow.<init>` 里被调最多的是 **`java.gui.Component.construct(Window;[String;String)V`
（23 次，桩）** —— 引擎侧的控件工厂；第三参是**编码后的布局串**（池里是 `CPABAAAA…`/`OLAAAA…` 这类
长串，不是类名 ⇒ "第三参=嵌入类名"的猜测被否）。

**已落的模型**：`java.gui.Embedded.getObject()` → 返回一个合法的 `java.gui.Component` 实例
（结构有效、字段为 0；**布局驱动的内容不建模**，文档标注为简化）。
**实测效果**：错误数 22 → 22（未变），静态面未动（1058 ok / 5403 ok / 0 bad）。
⇒ 结论：余下 gui 错误不是单点 native，而是**引擎侧控件树为空**（`construct` 不建树 ⇒ 后续
`getChild`/字段读取全为 null）。要真解这一族，得做**控件树模型**：`construct(window, args, layout)`
把控件挂进父窗口的 Java 侧注册表，读侧 getter 从注册表取 ⇒ 这是下一步（u i 布局串解码工具
`tools/ui_layout.py` 已有，可复用）。**判据**：`StartWindow.<init>` 的错误数降到 0 才算这一族收口。

#### 10.20 java.gui 控件模型的真实形状 + 落地（2026-10-17 晚）

**结论**：`java.gui` 整层是**native 绑定**——`java.gui.Component` 一个类就有 ~120 个 `native`
访问器（`create/getLeft/setLeft/…/getBackground/getTemplate/setonCreate/…`），控件状态活在**引擎的
native widget 对象**里，Java 对象只是代理。三条已对过反汇编的硬事实：

| 事实 | 证据 |
|---|---|
| `Component.construct(Window;[String;String)V` = 引擎侧控件工厂 | `FUN_005a0450` → `FUN_00652ab0(thread,&a,&b,&c,&d)` 把 4 个实参铺进 `local_14/local_10/local_8/local_c`（**最后一个槽装被 pop 的 `this`**）→ 读 `layoutText.field[0]`（宽字符载荷，`FUN_00643bf0`=vm_get_int_field）→ `FUN_005d4b20` 解析布局块建 native widget |
| 控件子对象（Background/Frame/…）是**按需**造的 Java 包装 | `getBackground = FUN_005ac140`：读 `this.field[0]`（native widget 指针）→ `widget+0x218` 的包装，包装由 `FUN_005a0120` 懒建（vtbl+0xc 取类名 → `FUN_00649170` 载类 → `FUN_00656100` 实例化 → `FUN_005ccbd0` 关联） |
| `Embedded.m_object` 是真 Java 字段（`java.gui.Embedded` 实例字段 0），`Embedded.getObject()` 是**纯 Java**（`return this.m_object`） | 布局里的 `Class` 属性（语料 29 条，全是 `java.game.frontend.*`：`loadInProgress`×6、`infoComponent`×7…）就是引擎实例化并写进去的类 |

**tufa_vm 里的模型**（`docs/65` 本节 + `vm/tufa_vm.c` `java.gui bridge model` 段）：
- `nat_gui_construct`：把布局串按 A–P 十六进制解成字节块 → 解析 `u32 size / u16 ver / u16 0x10 /
  u8 kind / cstr name / cstr template / prop*`（类型 1/4=u32、2=float、6=16B、3/12/13/14=cstr+4B、
  其余=cstr，与 `tools/ui_layout.py` 同构）→ 属性表落到该控件的记录上；
- `<Parent>.<Prop>`（`Background.*` 1697 / `Frame.*` 1987 / `Image.*` 457 / …）**按 `get<Parent>()`
  自己的返回描述符**决定包装类并懒建，然后把属性写进**子对象自己的记录**（`Background.setColor_Active`
  系列因此自然对上，子类无需表）；
- `getXxx/setXxx` 用**规则回退**（`nat_gui_rule`）按属性名回答，不必给 120 个方法逐个建表；
  **布局没设过的属性返回 0**，与旧桩行为一致 ⇒ 模型只会**增**保真度，不会把已通过的路径改坏；
- `kind` → 代理类表（2=Window、6=Component、8=Label、13=TextButton、37=IconButton、56=Embedded、
  63=Rotating、… 24 项）来自语料分组（1058 布局、0 冲突），用于**交叉校验**而不是造接收者。

**判据达成**：`java.game.Init.<init>` 驱动（入口→`Gamelogic.<init>`→`StartWindow.<init>`）
**错误数 22 → 0**；静态面 `classes 1058 ok/0 bad`、`fields 5403 ok/0 bad` 未破。`UNI_GUILOG=1`
可打每个 `construct` 的 kind/name/props/子对象/嵌入类。

#### 10.21 两个真语义缺陷（顺带挖到的）+ `<i>` 的实证结论

**(a) 构造器描述符不带返回类型**：容器里 `<init>` 是 `"()"` / `"(I)"`，**不是 `"()V"`**；引擎自己的
`Class.newInstance = FUN_00662500` 就是
`FUN_00653820(cls, "<init>", "()", 1)`——`0x6f4d38` 处的字面量逐字节是 `"()"`（`0x6e7704` 同处是 `"<init>"`）。
我们原来 `find_method(c,"<init>","()V")` **永不匹配** ⇒ `newInstance` / 引擎侧实例化从来没跑过构造器。
修：统一 `find_ctor()`（先 `"()"` 再兜 `"()V"`）。**这一条才是把最后 4 个错误清零的**——
`loadInProgress.<init>` 终于跑起来，`loading1/loading2` 不再是 null。

**(b) `Class.forName` 对不存在的类必须返回 null**：`java.game.Map.<init>` 用
`while (i<20) { if (forName(getName()+"_track_"+n) != null) i=0; else i++; }` 探测赛道类；
而 `load_class` 的 **synth 兜底**（给任何名字造空类）让它**永远命中** ⇒ 死循环（实测卡死在
`Map.<init>`，日志不再增长、采样停在同一帧）。修：Java 可见的 `forName` 先查
`cls_find || index_lookup`（数组描述符放行），miss 返回 0；synth 兜底只留给字节码路径（库类）。

**(c) `<i>()` 实例初始化器——结案：它由构造器自己用字节码调用**（我先前推反了，保留全过程以免重犯）

- **真事实**：全语料有 **125 处 `INVOKESPECIAL` 指向 `<i>()`**，**全部在 `<init>` 里**、且总是紧跟超类构造器之后的第一条
  （例：`Bot.<init>(Ljava.lang.String;...)` +0x0f、`ChallengeOffer.<init>()` +0x0f），`desc` 一律 `"()"`。
  即 `<i>` = 字段初始化块，**由构造器自己调用**（这也是链接器要给没有 `<i>` 的类补占位的原因：ctor 里那条调用永远存在）。
- **我上一轮的「全语料 0 条字节码引用 `<i>`」是假阴性**：扫描器解 member-ref 时没有跟进**嵌套类引用**
  （C2 → tag4/5 类引用 → 名字/NT），于是 125 处一个都没解出来。真值是 **125/125 全在 `<init>`**。
  教训：**「0 命中」先怀疑解析器**（同一个坑在本项目已出现过两次：全段线性反汇编、fnmatch 过滤）。
- **真 VM 侧实测（两条独立实验，都指向同一结论）**：
  ① `tools/uni_inst_probe.py`（引导到 `0x436130` → 注册器 → `649170` 定义 → `655100` 链接 → **`656100` =
  `FUN_00656100`**，NEW 操作数 0x654816 与 `Class.newInstance`(0x662500) 都走它）读实例 `[+0]=Class* [+4]=字段数
  [+8]=字段表`：`loadInProgress` 的 14 个槽**全 0** ⇒ **分配不跑 `<i>`**（与上面「ctor 调用」一致）；
  ② **跨 VM 探针** `probe/spec_i_probe.json`：`new java.game.frontend.HUD_Interface().getPosition()`——
  `HUD_Interface.<i>` 写 `position=1` 且**没有任何其它方法**写它（已全方法扫过）⇒ 结果 1 就证明 `<i>` 跑过。
  实测 **两侧都是 1**（对照例 `PointPairSeries.Count()` 两侧都是 3）⇒ **真 VM ≡ tufa_vm**。
- **结论与落地**：模型**不需要**任何特殊处理——`<i>` 走构造器里那条普通 `INVOKESPECIAL`；`vm/tufa_vm.c` 里
  上一轮加的「分配时补跑 `<i>`」(`run_instance_init` + `UNI_RUN_I`/`UNI_NO_I`) **已删除**（它会让对象字段
  **初始化两次**：一次我补的、一次 ctor 里的）。真正修掉 `loadInProgress` 那 4 个空接收者错误的是
  **`find_ctor`**（让 `<init>` 真跑起来 ⇒ 它内部那条 `<i>` 调用把 `loadingAnimation/loading1/loading2` 建出来）。
- 复验（删代码后）不回归：静态面 `classes 1058 ok/0 bad`、`fields 5403 ok/0 bad`（drift 2）；
  `Init.<init>` 驱动 `errs=0`；tufa 侧探针重跑仍 `Probe0=3`、`Probe1=1`。

**复现命令（静态面绿灯，别漏 `--file-root`）**：
```
grep '^###' vm/vm_dump.txt | sed 's/^### //' | tr -d '\r' > vm/clinit_names.txt   # 1,058 条，顺序=真 VM 侧
MSYS_NO_PATHCONV=1 UNI_DRIVERS="java.util.Config.init,java.util.Config.load" \
  ./vm/tufa_vm.exe --cp extracted --index vm/class_index.txt --batch --dump \
  --file-root C:/Games/LASR < vm/clinit_names.txt > vm/vm_dump.txt
python tools/vm_diff_real.py     # classes 1058 ok/0 bad；fields 5403 ok/0 bad；runtime-drift 2(info)
```
漏 `--file-root` 的现象：`Config.init` 1968→932 步、`load` 714→62 步（`File.exists/open` 失败即早退）
⇒ 18 个 Config 字段退回 clinit 默认值、classes 1056/2——**看着像代码回归，其实是命令少了参数**。

#### 10.22 控件状态对拍：布局属性表 ⟷ 引擎 native widget（2026-10-17 晚）

§10.20 的 gui 模型有一个**从没验证过的假定**：布局里没写过的属性一律返回 0。引擎那边 getter 其实是
`FUN_00643bf0(this, DCB2C)` 取 native widget 指针，再读 `widget+K`（K 见下表）——widget 由工厂
`FUN_005d4b20` 先清零再套布局属性，所以**没被布局设过的属性 = 工厂默认值**，未必是 0。

**做法（新工具能力）**：给跨 VM 探针 `tools/tufa_probe.py` 加了两种实参
- `("S", 文本)` → `STRING_LITERAL`（能把 UI 布局串当字面量传）；
- `("C", [类, 构造器desc, 构造器实参, 布局串, 窗口spec?, 表spec?])` → 造对象**并驱动引擎侧控件工厂**
  `Component.construct(<win>, <tbl>, 布局串)`，发射
  `NEW;DUP;ctor;INVOKESPECIAL;DUP;<win>;<tbl>;STRING 布局;INVOKE construct;POP`（**void 调用要补 POP**：
  引擎非 `<init>` 调用会压返回槽，全语料 `void调用;POP` 33,524 次、无 POP 0 次）。

**必须给真窗口**：`construct(null, null, blob)` 在真 VM 里直接 `data-exec` 崩（两次、不同地址）——
工厂要用窗口/表。`("C", [..., "SELF"])` 传对象自身（根窗口写法 `local0.construct(local0,…)`），
再叠一个真 kind-2 窗口（`loadInProgress.StandardWindow` 的布局）就通了。
**真 VM 侧必须 `PROBE_STUBS=""`**：默认桩列表里的 `*.create` 会把 `java.gui.Object.create()` 换成返回 0 的桩，
控件工厂拿到半成品对象。

**实测（`probe/spec_gui_state_b.json` 8 例 + `probe/spec_gui_defaults.json` 16 例，最小 Label 布局
= Template/Left/Top/Width/Height/Align/Format）**：

| 属性 | 真 VM | tufa_vm（修前） | 结论 |
|---|---|---|---|
| Width（布局写 515.0） | 515.0 | 515.0 ✓ | 布局属性解码两侧一致 |
| Align（布局写 1） | 1 | 1 ✓ | 同上 |
| Priority（布局写 1） | 1 | 1 ✓ | 同上 |
| Visible | **1** | 0 ✗ | **工厂默认 1** |
| Enabled | **1** | 0 ✗ | **工厂默认 1** |
| CanHover | **1** | 0 ✗ | **工厂默认 1** |
| Zoom/LayoutType/Mutex/TabStop/Autosize/SelectedState/Clipping/TabOrder/UserData/FadeFactor | 0 | 0 ✓ | 默认 0 |

**修**：`gw_default()` 加 `Visible/Enabled/CanHover → 1`，且**只对「布局建出来的控件」生效**
（`GwRec.kind >= 0`）；裸对象（没跑过 construct ⇒ native widget = 0）仍返回 0——引擎的 getter 开头就是
`if (widget == 0) return 0`（`FUN_005aaff0` 等）。修后 16 例两侧全绿，静态面（1058/5403）与
`Init.<init>` 驱动（0 错误）未回归。

**引擎 getter → widget 偏移表（本轮 grep 反汇编得到，后续做真状态同步用）**：
`Enabled +0x1c4`、`Visible +0x1c8`、`Mutex +0x1cc`、`LayoutType +0x1d4`、`Zoom +0x1e4`、
`Priority +0x20c`、`Width +0x130`（`FUN_005aaf10/005aaff0/005ab0d0/005ab290/005ab610/005abf50/005a9f60`）。

**工具坑（本轮白等了一轮）**：`probe_diff` 的注入是 `shutil.copy(GAME_ZIP, ZIP)` + `z.write(探针类)` ——
**不要在它跑批时并发调用 `build_probe`/`tufa_dump`**，否则在跑的探针类的 zip 条目被冲掉，真 VM 侧表现为
`load-fail`（record error、0 字段）。另：`real_dump` 会覆写 `probe/out_method_diff.json`，跑自己的 spec 前
先备份、跑完拷回（本轮结果落在 `probe/out_gui_state_b.json` / `probe/out_gui_defaults.json`）。

#### 10.23 控件工厂默认值按 kind 扫描 + 「沙箱里造不出来的 kind」清单（2026-10-17 深夜）

§10.22 的 `gw_default()` 只在 **一个 kind（8=Label，16 个 getter）**上测过。本轮按 kind 扩样：

**做法**：`tools/spec_subobj.py` 之外的 `probe/kind_blobs.json`（每种 kind 取**语料自己**的布局串 +
接收者类 `java.gui.<Kind>`，**必须全限定**，短名会被 tufa_vm 当 synth 空类 ⇒ 全 0 的假结果）+ 4 个
getter（`getVisible/getEnabled/getCanHover/getWidth`，Width 是「布局值确实落进控件」的哨兵）。
`probe_diff.py --chunk-size 4`：**chunk 才是真 VM 的隔离单位**（一崩只毁本 chunk，24 例一次跑完 =
一崩全毁，这是上一轮 24/24 全 None 的原因）。

**结果**（`probe/out_kind_defaults.json` / `out_kind_defaults2.json`，两次独立跑一致，ok=8 bad=16）：

| kind | 真 VM | tufa_vm | 判 |
|---|---|---|---|
| 7 Button（布局写了 Visible=0） | Vis 0 / En 1 / Hov 1 / W 32.0 | 同 | ✓ 4/4 逐位一致 |
| 63 Rotating | Vis 1 / En 1 / Hov 1 / W 40.0 | 同 | ✓ 4/4 |
| 6 Component / 13 TextButton / 37 IconButton / 56 Embedded | `UC_ERR_READ_UNMAPPED`（record error，fields=[]） | — | ✗ 沙箱测不了 |

- **排掉一个假设**：`construct(win, null, blob)` 里 table 是 null 会不会就是崩因？run 2 传**真正空 `String[]`**
  （`("A",["S",[]])`）后同样崩 ⇒ **不是 table**。剩「这几个 kind 的引擎工厂要真引擎状态（字体/纹理/
  资源管理器/渲染设备）」这一解释，落**实机验证**桶（=用户手测那一侧），不再空耗 boot。
- 结论：`gw_default()` 的「kind 无关」有三条独立支撑（Label 16 getter + Button + Rotating），
  模型不动；4 个测不了的 kind 记成待实机对拍而不是缺陷。

**工具（本轮新增/改）**
- `tools/tufa_probe.py` 新增两种实参形式：`("M",[类,方法,描述符,实参])` = 对栈上已有值再发一次调用
  （链式：`construct(...)` → `getBackground()` → 属性 getter），`("Q")` = POP（链式后配平栈）。
- `tools/spec_subobj.py`：**子对象属性对拍** spec 生成器（`new Cls().construct(...).get<Parent>().get<Prop>()`），
  12 例覆盖 Background/Frame/Text/Header/Thumb/List/ContainerRoot/ListboxContainer/MaxPointer/MinPointer/
  ThumbPeakMax/ThumbPeakMin（`probe/spec_subobj.json`，**未跑**，下一轮跑）。二层组 `List.Scroller.Width`
  需要两条链，已跳过。
- `tools/native_callsite_rank.py`：语料 native **调用点**排行（静态分析 2,224 个 src 文件）。
  配套改 `callsite_corpus.load_natives()`：以 `out_native_methods.json` 为权威列表，并按 (方法,描述符)
  回退匹配 ⇒ 经**子类接收者**调用的基类 native（`vehicles.classes.X extends Part` 调 `Part.createTypeBegin`）
  也能算进来；否则排行里只剩 CarDifferential 那几条（假阴性）。
- **正则坑**：布局串的字母表只有 A–P，`[A-Za-z0-9...]` 这种宽松字符类会把普通字符串实参也吞进去
  （`"loadingAnimation"`）⇒ `ui_layout.hexblob` `KeyError`。取 blob 一律用 `"([A-P]{40,})"`。

**下一块（按语料调用点排行定的序）**：Part/Chassis 渲染建造机 ——
`Chassis.configureType(String)` **13,403** 点 / 691 类 → `Chassis.configureVisual(RenderType;FFFFFF)` 896 →
`Part.getVehicle()` 844 → `Part.applyDecal` 744 → `Part.createTypeBegin` 691 / `Chassis.createTypeEnd` 691 →
`Part.configureMesh` 619 → `Part.configureTexture` 457 → `WheelRef.setWidth` 72 / `CarDifferential.setTable` 72。
`createTypeBegin/End` **Ghidra 没建函数**（`out_native_decomp` 里没有 004a6910/0049d980），但
`tools/dis_va.py 0x4a6910 0x4a69d0` 能反汇编：SEH 序言 → `FUN_00652ab0` 取实参 → `[edi+0x10]` 取 Part
native → `FUN_0054f4d0` 取链表 → `operator new(0xc)` 挂链 → `[part+0x38] = 节点` → `operator new(0x1f50)`
造 8,016 字节的建造上下文（`mov [esp+0x18],eax` 存局部）。即 createTypeBegin 是**分配 + 挂链**，
指令流可当 impl 的直接依据。

#### 10.24 子对象属性对拍：模型是好的，四处坑全在探针/选择规则（2026-10-17 深夜）

**动机**：布局里 `<Parent>.<Prop>`（`Background.Color_Active` 1697 / `Frame.*` 1987 / `Image.*` 457…）
是**子控件**的属性；引擎存在子 native widget 里，Java 侧经 `get<Parent>()` 拿懒建包装对象读写。
§10.20 的模型（`gw_sub` + `<Parent>.<Prop>` 落子记录 + 名字规则回退）从没被验证过。

**工具**：`tools/tufa_probe.py` 新增 `("M",[类,方法,描述符,[实参]])`（对栈上已有值再发一次调用 =
`construct(...)` → `get<Parent>()` → 属性 getter）、`("Q")` = POP 配平；生成器 `tools/spec_subobj.py`。

**★ 最终结果（`probe/spec_subobj.json` 7 例，tufa 单侧 oracle = 布局串自己解出来的值）**
Informative **6/6 全 OK**：`Frame.getBorderRight()=5.0`、`FormattedText.getEnableLineBreak()=1`、
`StringListbox.getItemHeight()=18.0`、`Component.getLayoutType()=1`、`Component.getHeight()=2.0`×2；
另 1 例布局值本来就是 0（无信息）。日志直接证据：`[gui] prop java.gui.Combobox.List.ItemHeight sub=ok`
+ `[gui] construct java.gui.Combobox … subs=3` ⇒ **子记录确实建了、值确实写进去了、读得回来**。

**四处坑（我先前把前三条误判成「模型缺口」，逐条更正）**

| 症状 | 真因 |
|---|---|
| 读回 0，stderr 有 `Referenced field 'getItemHeight' no longer exists…` | 属性 getter 的 `desc` 写成**裸类型** `"F"`：`resolve_member` 靠 `desc[0]=='('` 判方法（`vm/tufa_vm.c:502`），裸类型走**字段**解析后**静默压 0**。必须写方法描述符 `"()F"` |
| 某属性永远 0 | 该 getter **在注册表里 0 条**（如 `Frame.BorderLeft`）⇒ 生成器改为「查不到就跳过」 |
| 声明在父类（`Component.getHeight`）却写成接收者类 | 声明类要取**注册表里的 owner 类** |
| **注册表里有名字 ≠ 容器的 MTHD 里声明了**（本轮第 4 条） | `java.gui.Gauge.getMaxPointer/getMinPointer/getNormalPointer`、`java.gui.TabListContainer.getRowHeight` = **注册了但类里没有**：探针 INVOKE 那个名字解析不到、读回 0；**语料 Java 代码从不调用它们**（`grep -rn getMaxPointer out_pseudo/` = 0 处；布局里的 `MaxPointer.Angle` 由布局引擎自用）⇒ 不是模型缺口。生成器改为只挑**容器真的声明了**的 getter（读 `out_tasm/java/classes/**` 文本里的 `M … ; name`） |

**顺带修的模型侧小缺陷**：`gw_sub_class` 原来在「名字撞上但描述符不是 `()L…;` 的重载」时直接
`return NULL`，会让**整组子对象**创建失败（`Gauge` 的 NormalPointer/MinPointer/MaxPointer 正是这样）；
改成 `continue` 继续扫后面的重载，并加 `UNI_GUILOG` 定向日志
（`[gui] sub_class <类>.<getX> MISS (matched=N)` / `desc=… not ()L...;`）。

**重建后两闸门**：`classes 1058 ok / fields 5403 ok / 0 bad`；`Init.<init>` **87,727,821 步 / errs=0**。

**真 VM 侧仍不可测**：这些例子的 kind 落在 §10.23「沙箱造不出来」集合（`UC_ERR_READ_UNMAPPED`）
⇒ 子对象属性的**真值对拍要等实机会话**。

**复现**（tufa 单侧，秒级；探针索引是 `vm/probe_index.txt`，不是 `class_index.txt`，用错 ⇒ `steps=0` 假阴性）：
```
./.capenv/Scripts/python.exe tools/spec_subobj.py
UNI_GUILOG=1 ./vm/tufa_vm.exe --cp extracted --index vm/probe_index.txt   --clinit java.test.ProbeS --dump          # 日志在 stderr
```

#### 10.25 Part/Chassis 渲染建造机：模型第一刀（2026-10-17 深夜）

**为什么是它**：语料 native 调用点排行（`tools/native_callsite_rank.py`）头 8 名里 7 个属于这一族——
`Chassis.configureType(String)` **13,403 点/691 类** → `configureVisual` 896 → `Part.getVehicle` 844 →
`applyDecal` 744 → `createTypeBegin/End` 691 → `configureMesh` 619 → `configureTexture` 457。

**反汇编事实（本轮读的三支）**
| 函数 | 内容 |
|---|---|
| `Part.configureType` = `FUN_004997c0` | 取 Java 串 → **拷进 C 缓冲** → `this+0x10 → +0x38 → [node+8]` 拿到**建造上下文**（正是 `createTypeBegin` 写进 `part+0x38` 的那个 12 B 节点的 +8） → 查 `FUN_0055a230(0xa0000000,0) >= 0` → 按**每 kind 的处理器表** `DAT_007439b0[kind]` 分派 |
| `Chassis.configureType` = `FUN_004558f0` | 同形，只是 `this` 走 `FUN_00652c20()`（引擎取当前实例） |
| 分派器 `FUN_004afb30` | 把整行拷进缓冲后**在 `;`(0x3b) / `#`(0x23) / `/`(0x2f) 处截断**（语料里 `slot … ; lightbar` 的注释就是靠它）→ 分词 → 首 token 与一张 `(名字, 处理函数)` 表**大小写不敏感**比对 → 命中就调 `handler(行, ctx)`，表以 `"eof"` 结束 |

⇒ 建造机的形状 = **逐行指令 + 每 kind 一张处理器表**；`createTypeBegin` 分配 0x1f50 B 上下文并把 12 B 节点挂进
`part+0x38`（`tools/dis_va.py 0x4a6910` 读出的指令流：`FUN_00652ab0` 取实参 → `[edi+0x10]` → 链表 →
`operator new(0xc)` → `[part+0x38]=节点` → `operator new(0x1f50)`）。

**语料事实**：`configureType` 调用点 **13,217** 个、首 token **484 种**；真实键与频次（前 12）：
`noclick` 1810、`slot` 1153+443、`body` 742+408、`bone` 680、`nocollision` 637、`lods` 634、`lod` 621+290、
`flags` 467+…、`use_mesh` 467、`damage` 386、`flexible` 378、`wing` 291，另有
`dirt_texture / camera / type / spring / wheelbones / flap / slotdeform / controller`（与 `docs/61` 的
「字符串参数页」字段表一致：`body` 第 7 个 float = 质量、`slot` 第 7 个数字 = 物理槽 id 等）。

**tufa_vm 落地的模型**（`java.game.parts type-builder model` 段；**状态模型，不造几何**）
- `PtRec` 以**所有者类名**为键 —— 因为语料里这些调用是 **`INVOKESTATIC`**（`vehicles.classes.X.createTypeBegin()`），
  类型属于**类**而不是实例；
- `createTypeBegin/End` 开/闭一次建造；`configureType` 逐行记录（键 + 最多 8 个 float + 一个文本 token，
  按反汇编在 `;`/`#`/`/` 截断）；`configureVisual/configureMesh/configureTexture/applyDecal` 计数；
- `UNI_PARTLOG=1` 打摘要 `[part] begin/end <类> type=N vis=N mesh=N tex=N decal=N key:count…`。

**验证（1/1 逐条一致）**：`--clinit vehicles.classes.CTF_Lightbar`（语料里它声明 1×begin、3×visual、7×type、1×end）
⇒ 实测 `type=7 vis=3 mesh=0 tex=0 decal=0 linked:1 linked:1 body:8 noclick:0 slot:7 slot:7 slot:7` ✓
键与顺序全对（`body` 行有 10 个数字，模型按 8 个上限截断）。`UNI_DRIVERS="vehicles.classes.CTF_Lightbar.staticinit"`
驱动同样 133 步 / 0 错误。抽样 24 个车辆类 `--clinit` 全部能加载（2 个 `steps=0` 的类**本来就没有 `<clinit>`、没有代码记录、也没有任何字段**：
`Coupe_SD_T5_IDiffs_rally` / `Hatch_CycloneR_Rim_ST` 只声明了 `onInstall_*` + `<init>` ⇒ 正常。
注意：判「类没加载」不能只看 `steps=0` 或 `--dump` 字段行——空类两者都是 0）。

**⚠ 段错误坑（本轮踩到并修）**：native 的**接收者可以是 0**（`INVOKESTATIC`！），第一版在日志里
`((Inst*)t)->cls` ⇒ **宿主段错误**（`UNI_PARTLOG=1` 才触发，害我误判成 dump 路径的锅）。
现在统一走 `pt_owner()`：`inst_valid(o) && o->cls` 才取类名，否则退回 `g_nat_cls`。

**闸门**：`classes 1058 ok / fields 5403 ok / 0 bad`；`Init.<init>` **88,683,478 步 / errs=0**
（另一次带 PARTLOG 的 89.2M 步，`native_impl` 11.6M）。**未做**：几何/网格/纹理解析成真值、`getVehicle` 关联、
每个指令的语义（现在只记不解释）——都是后续轮次。

#### 10.26 native ABI 与「返回槽」规则：两条等效性硬事实（2026-10-17 深夜）

重编译产物要「功能一致」，边界上有两件事必须逐字对上：**引擎 native 怎么被调用**、**返回值怎么回到
Java 栈**。本轮各拿到一份反汇编/代码级证据。

**(a) native ABI（读 `FUN_00498800` = `Part.getVehicle` + `FUN_004997c0` = `Part.configureType`）**
- 入口只有**一个 C 实参**：`ecx` = **thread/env 指针**（两处都写成 `param_1` 后被直接喂给取值助手）；
- Java 实参**不在 C 栈上**，要 native 自己从**VM 操作栈**取：
  `FUN_00652ab0(thread, idx, &out)`（带下标，configureType 用它取第 0 个实参 `String`）；
  **receivers 用另一个形态** `FUN_00652ab0(thread, &out)`（getVehicle 用它取 `this`）；
- 返回值走 **eax**（`FUN_00498800` 结尾 `return *(undefined4 *)(…+0x68);`），native 自身以裸 `ret` 收尾
  （早前注册点实测的结论一致）；
- ⇒ **产品级重建 VM 若想直接复用原版 native，必须照这个 ABI 调**（ecx=thread、实参留在 VM 栈上、
  返回值从 eax 取回再按签名压槽）。`vm/tufa_vm.c` 现在用的是**模型 ABI**（先把实参右到左弹进 `argv[]`、
  `rfn(thisv, argv)` 直接返回 C 值）——**行为等价但不是字节兼容**，本轮把这条写清楚，免得把模型
  当成产品边界。

**(b) 返回槽规则（我们 VM 里已实现，本轮交叉验证）**
`prep_method()`：`m->retc = (p && p[1]) ? p[1] : 0;`（`p` = `')'` 的位置）⇒
- **签名以 `)` 结尾（构造器 `"()"`/`"(I)"`）⇒ retc = 0 ⇒ 完全不压槽**；
- **`)V` ⇒ retc = 'V'（非 0）⇒ 压一个哑槽**；`)I/)F/)L…` ⇒ 压真值。
  这与提升器的语料事实吻合：**全库 33,524 处 `void 调用; POP`、0 处无 POP**（§M3 第 ④ 条），
  也解释了当初「构造器描述符不带返回类型」为什么是硬性要求（`"()V"` 会多压一个槽）。
  对应实现：`call_method` native/普通两路都 `if (m->retc) push(...)`，RETURN 路径
  `if (retc) { if (retc != 'V') rv = pop(); }`（void 只留槽不取值）。

**(c) `Part.getVehicle` 的引擎链（本轮读出来的，模型暂不实现）**
`Java 零件对象 → FUN_00643bf0(obj, 0x8dcb2c) 拿 native 对象 → +8 → 按 kind 表 `DAT_007439b0[+0x24]`
→ +0x64 → +0x98 → +0x68 = Vehicle`。三层都带 `if (!x) return 0` 与 `FUN_0055a230(0xa0000000,1) >= 0`
守卫。**tufa_vm 保持返回 0**：这条链的第一跳（Java→native 对象槽 `0x8dcb2c`）由引擎在零件实例化时建立，
Java 侧没有任何 setter，我们**不臆造**（844 个调用点都在实机运行时路径上，不在 Init 窗口里）。

#### 10.27 容器才是 native 的权威清单：注册表抓漏 1,508 条（2026-10-17 深夜）

**起因**：准备做 `java.util.resource` 家族时发现语料调用 `ResourceRef.unload(I)V` **745 次**，而
`out_native_methods.json`（我从注册点扫出来的表）里**根本没有这一行**。顺着查下去，是"native 全量"
这件事本身需要更正。

**判据（三种已知样本互证）**：`out_tasm/<类>.class.tasm` 的方法行
`M <flags> <nameIdx> <descIdx> <code> <rec> ; <name> <desc>` 里，**`code == 4294967295` ⇔ 没有代码 ⇔ native**：

| 样本 | 行的 code 列 | 已知性质 | 判 |
|---|---|---|---|
| `Part.getInfo (I)I` | `M 1 56 55 5 2` → 5 | 纯 Java（委托 `getInfo(I,0)`） | Java ✓ |
| `Part.flap (I)I` / `getVehicle ()Ljava.game.Vehicle;` | `M 258 … 4294967295 0` | native（无 Java 体） | native ✓ |
| `Component.addChild (L…)V` | `M 1 30 24 2 2` → 2 | Java（`local0.addChildN(local1);`） | Java ✓ |
| `Component.create ()V` / `getBackground` / `getFrame` | `4294967295` | native | native ✓ |
| `Embedded.getObject ()Ljava.gui.Component;` | `M 1 13 12 1 1` → 1 | 纯 Java（`return this.m_object;`） | Java ✓ |

**规模修正（重要）**：容器声明 native **2,660** 条；我的 `out_native_methods.json` 只有 **1,523** 行，
且**整模块缺失**：`java.io.File` 16、`java.sound.Sound` 33、`java.util.resource.ResourceRef` 28、
`java.net.MetaServer` 60、`java.gfx.GfxEngine` 45、`java.lang.System` 32、`java.gui.Text` 30、
`java.game.Racer` 16、`java.lang.String` 15、`java.lang.Thread` 13 …（精确差集 **1,508** 条）。
⇒ 以前文档里"注册表 1,156/1,523 条"要读成**「我抓到的注册点」**，不是 native 全量；后续实现顺序也
**不能按注册表顺序**，要按语料真实调用频次。

**新工具**（本轮）
- `tools/native_audit.py`：从 `out_tasm` 扫出**容器声明的 native 清单**（2,660）并与注册表求差，`--gaps` 全量列出。
- `tools/native_backlog.py`：`declared ∩ called − implemented` = **真 backlog**，按调用点频次排序
  （called 来自 `tools/native_callsite_rank.py`，implemented 从 `vm/tufa_vm.c` 的分派表正则取）。

**§10.27 附：真 backlog（`tools/native_backlog.py`，2026-10-17 深夜实测）**

```
declared natives (containers):       2660
called by the corpus:                27407 sites / 551 natives
implemented in vm/tufa_vm.c:         50
covered by the gui rule / by design: 208      (java.gui.* 的 get/set 走 nat_gui_rule；
                                               Part.getVehicle 按 §10.26 设计性返回 0)
CALLED but NOT implemented:          311 natives
declared but never called:           2109      (不在关键路径上)
```

按调用点排的队首（就是实现顺序）：

| 调用点 | native | 备注 |
|---|---|---|
| 745 | `ResourceRef.unload(I)V` | 语义已读（`FUN_0042b450` = 取 `[obj+8]` 节点 → 虚表 +0xc 释放；`p[9]==1` 走 `FUN_00435bb0`）⇒ 模型只需标记释放 |
| 88 | `GameRef.command(String)V` | 引擎命令串分派 |
| 88 / 40 / 39 / 35 | `gfx.Viewport.getWidth/getTop/deactivate/getLeft` | 视口矩形：能设就设、设不了不造 |
| 72 / 45 / 22 | `parts.CarDifferential.setTable(I[F[F)V` / `set(IFF)V` / `set(IFFF)V` | 差速器参数，确定性存储 |
| 60 | `Chassis.getSfxTable(I)LSfxTable;` | 需要能给回一个 SfxTable 实例 |
| 29 / 16 | `net.MetaServer.getRoomParameterI/setUserParameterI` | 房间/用户参数表 |
| 25 | `java.lang.Math.random()F` | 模型必须给**确定性 PRNG**（真随机无法对拍），并标注为模型 |
| 24 / 23 / 21 / 21 / 21 | `gui.Component.postCustomEvent(I)` / `ResourceRef.cache()` / `gui.Object.initBegin+initEnd` / `String.intValue()` | 大多是一行语义 |
| 20 / 19 | `gfx.Animation.finalize()` / `java.lang.Vector3.rotate(Ypr)` | **`Vector3.rotate` 是纯数学 ⇒ 可以用探针逐位对拍**（像 §10.11 那 5/5） |
| 17×6 | `gfx.GfxEngine.setShadowmap/setShadowColor/setGlowThreshold/setEnvmapIntensity/…` | 渲染配置：存值即可 |
| 17 | `sound.Sound.changeMusicSet(I)` | 与已有音量模型并列 |

⇒ 下一批做「便宜且可验」的一组：`Vector3.rotate`（探针逐位对拍）+ `String.intValue/floatValue` +
`ResourceRef.unload/cache` + `gui.Object.initBegin/initEnd` + `Animation.finalize` + `Viewport.*` +
`CarDifferential.set*` + `SfxTable` 拿取 + `Sound.changeMusicSet`。

#### 10.28 三条「精确」native + 一个会假装成真 VM 故障的探针坑（2026-10-17 深夜）

**pick 原则**：backlog 里 311 条未实现 native 大多是**引擎状态读写**（返回值由引擎对象决定，模型不能造）；
先做**纯计算**的那几条——它们的语义可以从二进制里读准，而且**能用跨 VM 探针逐位对拍**。

**(a) `java.lang.String.intValue()I`**（语料 21 点）= `FUN_006615a0`：
取 native 串槽（= String 的宽字符缓冲）→ **`_swscanf(text, L"%d", &out)`**（格式串在 `0x6e7c54`；
扫描器包装 `FUN_00567cf0` 就是 `_swscanf`）→ 返回 `out`。
**(b) `java.lang.String.floatValue()F`**（16 点）= `FUN_006615e0`：同上，格式串 `0x703b1c` = `L"%f"`。
**(c) `java.lang.Math.random()F`**（25 点）= `FUN_00405390`：`FUN_006674f8()`（CRT `rand`）`& 0x7fff`、
`== 0x7fff` 时改 `0x7ffe`，再 `fild`（int→double）`fmul dword[0x6e7cf4]`（**float32** = `1/32767`，位模式
**`0x38000100`**）→ 收尾由包装窄化成 f32。⇒ 就是 **MSVC CRT 的 LCG**（`s = s*214013 + 2531011;
r = (s>>16) & 0x7fff`）。实现里按**位模式**取那个常量（写 double 字面量会在个别 r 上翻最后一位）。

**跨 VM 对拍（`probe/spec_strnum.json` 6 例，`--chunk-size 3`）**
| 用例 | tufa | 真 VM | 判 |
|---|---|---|---|
| `"12.75".floatValue()` | `0x414C0000` | `0x414C0000` | ✓ 逐位一致 |
| `"-3.5".floatValue()` | `0xC0600000` | `0xC0600000` | ✓ 逐位一致 |
| `"xyz".floatValue()` | `0` | `0x203C0D70`（非规格化极小值） | **原版 UB**：`swscanf` 没匹配时 `out` 是**未初始化栈垃圾** ⇒ 我们置 0 并在代码注释里写明不可复现 |
| `"123"/"-45"/"abc".intValue()` | 123 / -45 / 0 | `None` | 见下（**不是**语义差） |

**★ 探针坑：`poison-skip` 会伪装成「真 VM 崩了」**。`tools/uni_dump.py` 有个**按类名计数、跨运行保留**的
黑名单 `out_dump_poison.txt`（≥2 次即跳过该类的 dump，写 `error: "poison-skip"`）。20:00:56 那轮
`gui_kinds6`（6 个 chunk）把 `java.test.Probe0…Probe4` 写进了黑名单 ⇒ **之后任何探针跑的第 1 块都被静默跳过**，
表现就是 `real=None`（和真 VM 崩了长得一模一样，我上一轮已经因此误判过一次）。
修：① `probe_diff` 现在给每次运行生成**唯一类名** `java.test.P<pid低4位>_<k>`；
② 这份陈旧黑名单已移走（`out_dump_poison.stale.bak`）。判据：`vm/*.log` 里出现 `[dump] poison-skip` 就是它，
不是语义差。

**闸门**：静态面 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 75,402,869 步 / errs=0（步数在 75–89M
区间浮动=开场等待循环读 clock，`errors=0` 才是判据）。

**收口（重跑后）**：`probe/out_strint.json` = `ok=2 bad=1` ⇒ `"123"→123`、`"-45"→0xFFFFFFD3` **两侧逐位一致**；
唯一 BAD 是 `"abc"`（tufa 0 vs 真 VM `0x203C0D60`）。把它和 `floatValue("xyz")` 那例的 `0x203C0D70` 放一起看：
**两个用例的"垃圾值"互不相同**⇒ 正是未初始化栈的表现，不是某个确定语义。这同时证明
`chunk 1` 之前被 `poison-skip` 跳掉是**工具坑**而非语义差（换唯一类名后立刻测到真值）。
⇒ 结论：`String.intValue/floatValue` 的语义**已按二进制读准并对拍通过**（可比值 4/4 全绿），
UB 那两例按"置 0 + 注明不可复现"入册。

#### 10.29 native 实现第三批：整块「引擎调用型」no-op（9 条 / 902 调用点）+ 一个重要的引擎机制发现

**做法**：不再按 backlog 频次盲打，而是**先把候选函数反汇编到 `ret`**，再决定口径。
这一族（`gui.*` / `gfx.*` / `ResourceRef.unload`）形状完全一致：

```
push ecx                                  ; 给"拆箱输出槽"留位
... push &slot ; push this ; call 0x652ab0 ; ← FUN_00652ab0 = 一次性拆箱全部 Java 实参到局部槽
mov eax,[0x8dcb2c] ; push eax ; push this ; call 0x643bf0
                                          ; ← FUN_00643bf0(this, 0x8dcb2c) = 取 receiver 的
                                          ;   NATIVE 对象（0x8dcb2c 是"native 指针槽"的槽号）
test eax,eax ; je <end>
   ... 一次引擎调用（虚表槽 / 单例）...
ret                                       ; 全程不读写任何 Java 字段
```

| Java native | 地址 | 体内唯一的引擎动作 | 口径 |
|---|---|---|---|
| `gui.Object.initBegin()V` | `0x5a0360` | `call [native+0x24]` | no-op |
| `gui.Object.initEnd()V` | `0x5a0390` | `call [native+0x28]` | no-op |
| `gui.TabList.refresh()V` | `0x5a0e40` | `call [native+0x164]` | no-op |
| `gui.Component.postCustomEvent(I)V` | `0x5a0410` | `FUN_005dce00(native, id)` | no-op |
| `gfx.Viewport.deactivate()V` | `0x40ae30` | `FUN_004c3140(gfx 单例[0x7432c8], native)` | no-op |
| `gfx.Animation.finalize()V` | `0x40aec0` | `call [native]` + 按名字查找 + 清一个 native 槽 | no-op |
| `ResourceRef.unload(I)V` (745) | `0x4107d0` | `FUN_0042b450(native, handle)` | no-op |
| `sound.Sound.changeMusicSet(I)V` | `0x40e9a0` | 写全局"当前音乐集"指针 `[0x75df08]` | **存索引**（全局 `g_music_set`） |
| `Chassis.getSfxTable(I)SfxTable;` (60) | `0x4884e0` | 返回引擎 chassis 结构里的子对象指针 | **每 (receiver,index) 一个缓存实例** |

`ResourceRef.unload` 值得单独说：`FUN_0042b450` 释放的是**传进去的 handle**，receiver 自己（Java 字段 /
native 槽）**一个字节都没动** ⇒ Java 层可见行为就是"什么都不发生"，745 个调用点一起收口。
`changeMusicSet` 的 `[0x75df04]` 是音乐集表长度、`[0x75db00]` 是表；越界时引擎存 NULL ⇒ 模型存 `-1`；
表长度我们不建模（不臆造），只记索引。
`getSfxTable`：`i<0 → NULL`；`native=FUN_00643bf0(this)` → `[native+8]` → `node[+0x24]`=kind → 查
**每 kind 偏移表 `[0x7439b0]`** → `node + offset`。模型没有 chassis 结构，就返回**每 (receiver,index)
一个 `gw_new_obj("java.game.parts.SfxTable")` 实例**（跑过 `<init>`），调用方可以存下来并继续调方法。
本侧冒烟实测（`probe/java/test/ChkSfx02`）：`new Chassis()` → `getSfxTable(0)` 返回非 0 实例、无崩溃。

**★ 引擎机制发现（对后续所有 `set*` 家族都关键）**：`CarDifferential.set(IFF)V` 与 `set(IFFF)V`
**共用同一个 C 函数 `0x483db0`** —— 因为 `FUN_00652ab0` 是**按 Java 调用实际传了几个实参**来拆箱的，
而函数**先把某个槽预写成默认值**（`movss xmm0,[0x6ea6d4]`；**该常量 = `0x3f000000` = 0.5f**），
Java 调用传满了就覆盖它。⇒ 「一个 C 体 + 多个 Java 重载签名」是这套引擎的**常规**写法，
读伪码时看到 `(IFF)V`/`(IFFF)V` 指向同一地址**不是**登记错误。
该函数落的四个字段：`[native+0]=int`、`[native+4]=abs(f2)`、`[native+8]=-abs(f1)`、`[native+0xc]=f3|0.5f`；
`setTable(I[F[F)V`（`0x483e30`）按 int 选槽：`0→+0x1c`、`1→+0x5c`、`2→+0x9c`（步长 `0x40`）再拷数组
⇒ 这一族写的是**引擎 native 结构、Java 侧无 getter**，属于"可存但不可验"的一类（见下轮计划）。

`GameRef.command(String)V`（88 点，`0x4209f0`）读到：拆箱 → 取 native → `FUN_00567d90`/`FUN_0066659d`
（取窄字符串/长度）→ 长函数体，是**命令/脚本解析器**，不是 no-op ⇒ **不臆造**，留作专门一轮。

**闸门**：静态面 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 84,686,267 步 / errs=0；
backlog `308 → 299`（implemented 53 → 62）。新增同类与地址一并登记在 `vm/tufa_vm.c`
（每条都带"引擎调用停在此处"的注释，未来做 native 侧模型可直接接上）。

#### 10.30 native 实现第四批：把 299 条 backlog **按形状机械分类**，再按类落实现（26 条 no-op + 5 条精确状态）

**为什么要做工具**：backlog 还有 ~300 条，一条条读会退化成"读一个猜一个"，而猜出来的模型和真引擎长得一样、
测不出来。`tools/native_shape_scan.py` 把每条 backlog native 的 C 体反汇编到 `ret`，按**形状**归类：

| shape | 判据（机械） | 口径 |
|---|---|---|
| `pure-compute` | 无 `0x8dcb2c` 访问 + 无调用 + 浮点运算 | 可**精确实现**并跨 VM 对拍 |
| `global-store(-ret)` | 只写全局 `[0x......]` | 可精确实现（存 VM 全局） |
| `engine-call` | `unwrap → 0x643bf0(this,0x8dcb2c)` + **≤1 次**引擎动作，且**每个 store 的基址寄存器可证持有 native 指针** | Java 可见 no-op |
| `engine-call-store?` | 同上但 store 基址**证不出**是 native | **不许当 no-op**，留 backlog |
| `engine-read` | 走 native 对象取字段/虚表 | **引擎状态，模型不可造** |
| `complex` / `no-address` / `named-native` | 解析器/分配器/数组、注册表无地址、宿主函数名 | 各自专门处理 |

**判据是校准出来的**（这一点最重要）：先读了 4 个函数把规则钉死 —— `0x40aa00 setGlowThreshold`
（无 native 访问、只写一个全局浮点）、`0x40a170 setDirtLevel`（比较+存+**返回**该全局）、
`0x40bfd0 Viewport.getWidth`（走 native 对象取 `[+0x18]` ⇒ 引擎状态，**不能造**）、以及上一轮那族 `engine-call`。
第一版规则把 `Viewport.getWidth` 误判成 `engine-call`（会把它当 no-op 返回 0！）—— 修正：**返回值不是 `V` 的永不判 no-op**，
并把 store 的基址寄存器做前向跟踪（`FUN_00643bf0` 返回在 eax，`mov X,eax` 传播；x86 ABI 下再调别人 `call` 则 eax/ecx/edx 失效）。

**分类结果（299 行 / 1,414 调用点）**：`engine-read 137 行/625 点`、`complex 43/158`、`no-address 54/246`、
`engine-call 26/154`、`engine-call-store? 15/97`、`field-getter 10/24`、`named-native 3/15`、`global-store 5/63`、
`global-store-ret 2/24`、`trivial 3/3`、`pure-compute 1/5`。全部落盘 `out_native_shapes.json`，
**边界清单**（模型明确拒绝伪造的部分）`out_native_boundary.json`。

**本轮落地**：
- **26 条 `engine-call` no-op**（154 点）成批登记，逐行注释带 `fn` + 引擎动作 + 站点数（`out_noop_candidates.json` 是同一份证据）。
  例子：`gui.TabList.start`(12,`call 0x5e5e30`)、`GameRef.setEventMask/clearEventMask`(12,`call 0x6478f0`)、
  `sound.SfxRef.stop`(10,`call 0x42f420`)、`gui.WStringList.clear`(8)、`Thread.stop`(4)、`Component.addChildN/remChildN/nshow/nhide`、
  `CarDifferential.setTable(I[F[F)`(72 —— 把两个 float[] 拷进引擎的差速器结构 `+0x1c/+0x5c/+0x9c`，Java 侧无 getter)。
- **5 条精确状态**（全部先反编译再写）：
  `Math.sqrt(F)F` = `0x4053c0` → `sqrtf`（fsqrt/sqrtss 对 f32 都是正确舍入 ⇒ 逐位相同，已跨 VM 对拍）；
  `GfxEngine.setGlowThreshold(F)V` `0x40aa00` → `g[0x743568] = f`；
  `setDirtType(I)I` `0x40a130` → `if (i>=0) g[0x771ab8]=i; return i>=0?i:g`；
  `setDirtLevel(F)F` `0x40a170` → `if (f>=0.0f) g[0x742608]=f; return g`；
  `System.syncGameTime(F)V` `0x405e30` → `d=(int)f%86400; g[0x763d9c]=d; g[0x7435a4]=phase?0:1`（phase 判据照抄）。
  **模型全局的初值从 exe 镜像里读**（`0x743568`=0x6666263f=0.65f、`0x742608`=1.0f、`0x771ab8`=0x34861481、
  `0x763d9c`=0x395b3926、`0x7435a4`=1），这样"第一次调用之前"的状态也一致。

**故意没落的三条**：`setShadowColor(II)` `0x40a8f0`（存 `~arg` 到 `0x7435d4`）、`setEnvmapIntensity(FF)` `0x40aa30`
（`0x7434ac/0x7434b0`）、`setReflectorColor(FFF)` `0x40aa70`（`0x743370/374/378`）。槽→全局的映射已经从反编译拿到了，
但**哪个 Java 实参进哪个槽还没判死**，而这三条既无 getter 又不做范围检查 ⇒ **猜错也测不出来**。
宁可在 backlog 里留着（下一步用 harness 读目标全局来定序），也不把"可能是反的"当成交付。

**★ 这一批差点白干（教训）**：第一版生成 26 条登记时按 key 的**第一个点**切类名，写出 `{"java", "game.parts.…"}` ——
`cl` 编译不报错、闸门照旧全绿，只有 `native_backlog.py` 的 implemented 计数暴露（62→67 而不是 62→93）。
改成从右切（`(.*)\.([^.]+)(\(.*)`）后 67→**93**、backlog **294→269**。⇒ **登记完必须看 backlog 数下降**，
"编译通过 + 闸门绿"证明不了登记真的生效。
**闸门**：静态面 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 85,441,659 步 / errs=0，
且 `native_stubs 227 → 196`（少掉的 31 类桩就是新实现被真正命中的直接证据）。

#### 10.31 把实参顺序**证明**出来（不是猜）：引擎拆箱器的合同 + 三条多实参 gfx setter 落地

上一轮把 `setShadowColor(II)` / `setEnvmapIntensity(FF)` / `setReflectorColor(FFF)` 留在 backlog，因为"哪个 Java 实参进哪个全局"
判不死、也没有 getter 能对拍。这轮把它**从引擎自己的 helper body 里证出来**：

**① `FUN_00652f10` = VM 操作数栈的 POP**（`[stk+0x50][--stk[0x54]]`，读后清零）
⇒ **第一次 pop 拿到的是最后一个 Java 实参**。

**② `FUN_00652ab0(this, extra_out, p1, p2, ...)` = 拆箱器**，body 是：
```
argc = this->vm[0x1c];
for (i = 0; i < argc; i++) local[i] = POP();          // local[0] = 最后一个实参
for (k = 0; k < argc; k++) *(p_{k+1}) = local[argc-1-k];   // 逆序回填
if (this[0] == 0) extra = POP(); else extra = 0;
if (extra_out) *extra_out = extra;
```
⇒ **`p1` 收 arg1、`p2` 收 arg2、…**（指针列表就是 Java 实参顺序），`extra_out` 只在 `this[0]==0` 时才被填
（这就是每条 native 里那个 `push 0` 的来源 —— 它是"可选的额外输出指针"，传 NULL）。
1 实参的那族（`setDirtLevel/setGlowThreshold`）只有一个指针、而且是**最后压入**的那个（反编译里叫 `local_4`），
按上面的规则它就是 arg1 —— 与 `setDirtLevel` 实测（它把 arg 存进全局又返回该全局）一致。
⇒ 两个 helper 是**自证**的，顺序问题不存在了。

**③ store 偏移的标定**：在 `0x40aa70 setReflectorColor` 上，asm 的 store 源地址（`[esp+0x14]/+0x18/+0x1c`）与反编译的符号序
（`local_c/local_8/local_4` = arg1/2/3）**同时已知** ⇒ 偏移递增 = 实参序号递增。同帧（`sub esp,8`）的
`setEnvmapIntensity/setShadowColor` 因此读 `[esp+0x10]`=arg1、`[esp+0x14]`=arg2。

**落地**（模型全局初值同样从 exe 镜像读）：
| native | 地址 | 语义 | 初值 |
|---|---|---|---|
| `GfxEngine.setShadowColor(II)V` | `0x40a8f0` | `g[0x7435d4] = ~arg1`（arg2 未用） | `0xffffffff`（≡ `~0` ✓ 自洽） |
| `GfxEngine.setEnvmapIntensity(FF)V` | `0x40aa30` | `0x7434ac=arg1`、`0x7434b0=arg2` | `2.0f` / `0.5f` |
| `GfxEngine.setReflectorColor(FFF)V` | `0x40aa70` | `0x743370/374/378 = arg1/arg2/arg3` | `1.0f` ×3（白色反射 ✓ 合理） |

**★ 同时修正一处旧结论**：`0x8dcb2c` **不是**"native 指针槽号常量"，它是**一个全局变量的地址**，
其中存的是传给 `FUN_00643bf0(obj, idx)` 的**字段索引**；而 `FUN_00643bf0` 的 body 是
```
if (obj && 0 <= idx < obj[1]) return obj[8][idx];      // obj[4]=字段数, obj[8]=字段数组
else log("vm_get_int_field: unknown field: %d")
```
⇒ 即 `vm_get_int_field`。natives 里那句 `mov eax,[0x8dcb2c]; push eax; push this; call 0x643bf0`
是"取 this 的**第 N 个字段**"（N 由 VM 在运行时设定），不是我上一轮写的"取 native 对象槽"。
配套的 setter 是 `FUN_00643b30(obj, idx, value)`（见 `Animation.finalize`）。
口径不变（这些字段就是引擎侧数据、Java 不可见），但文档/skill 里的措辞已按事实改正。

**闸门**：静态面 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 80,076,376 步 / errs=0；
backlog `269 → 265`（implemented 93 → 96，**登记后按计数复核**）。
**在跑的对拍**：`probe/spec_gfxstate.json`（6 例顺序用例：`setDirtLevel(-1)` 读镜像初值 → 存 0.25 → 再读 0.25；
`setDirtType(-1)` 读初值 → 存 7 → 再读 7 —— 这两条**有返回值所以可观测**，是这一族的可验证锚点）。

**对拍收口（`probe/spec_math.json`，6 例）**：`Math.sqrt` **6/6 逐位一致** —— 2.0→`0x40000000`、
1.4142135381698608→`0x3FB504F3`、0.7071067690849304→`0x3F3504F3`、`sqrt(1e-40)`→`0x1E3E1AE7`、
`sqrt(1e20)`→`1e10`→`0x501502F9`、`sqrt(-1.0)`→**两侧同为 `0xFFC00000`**（x87 的 indefinite NaN）。
⇒ 与 `String.intValue/floatValue` 一样，这条是**可比值全绿**。
**一个要记在案的前提**：模型里的 `sqrtf` 走的是**宿主 libm**（MinGW/glibc），引擎走 x87/SSE 指令；
这 6 例逐位相同说明两者在 f32 域一致（连 NaN 载荷都一致），但这属于"**实测一致**"而非"数学上必然"——
换宿主工具链理论上可能改 NaN 载荷，届时这几例要重跑。已写进这条记录，别把它当恒真。

**探针坑（今晚踩两次）**：`probe_diff.py` 多实例**不能并发** —— 它们共用 `probe/classes_probe.zip`、
`probe/realvm_probe.jsonl`、`probe_index.txt`，并发时真 VM 侧直接拿不到 dump（`rv=None`，
现象与"真 VM 崩了"一模一样）。已给 `probe_diff` 补上"真 VM 无记录 ⇒ 打它的日志尾再继续"的路径。

**gfxstate 重跑 ⇒ 6/6 两侧一致（`probe/spec_gfxstate.json`，顺序用例）**：
```
r0 setDirtLevel(-1.0)  tufa 1.0        real 1.0        <- 镜像初值 0x3F800000 ✓（修正前我把字节 00 00 80 3F 当数值写，得 4.6e-41）
r1 setDirtLevel(0.25)  tufa 0.25       real 0.25       <- 存
r2 setDirtLevel(-2.0)  tufa 0.25       real 0.25       <- 不存（f<0 保持）
r3 setDirtType(-1)     tufa 0        real 0        <- 0x771ab8 是 bss，运行时初值 0（镜像字节不可用）
r4 setDirtType(7)      tufa 7        real 7
r5 setDirtType(-1)     tufa 7        real 7          <- 不存（i<0 保持）
```
⇒ 这一族（写全局 + 返回该全局）**两侧行为一致**，且顺带把"镜像初值可读/不可读"的边界钉死：
**初始化数据段可读、bss 必须实测**。

**一个未解释的残留（记在案，不藏）**：修正前那次跑，`r3` 的 tufa 侧值是 `881202305`，
既不是我写的常量 `881210497`(0x34861481)，也不是真 VM 的 0 —— 目前怀疑是那次"并发跑导致真 VM 侧无 dump"的同一批次里
tufa 侧 dump/字段读取的干扰（修正初值后重跑已无此现象）。若以后再出现"模型回值 ≠ 自己的常量"，按可疑顺序查：
① 并发跑的共享 scratch；② 探针字段类型/序号；③ native 返回路径。
**教训（通用）**：把"读到的字节"直接贴成 C 常量很容易贴成**小端字节序**；凡是从镜像抄的常量，一律写成
**数值 + 注释里附字节**，并且只要该全局可观测，就用 probe 对拍一次。

#### 10.32 探针产物台账（自动生成快照，防止把旧文件当新结果读）

| 文件 | 结果 | 首个用例 |
|---|---|---|
| `probe/out_gfxstate.json` | ok=4 bad=2 | `java.gfx.GfxEngine.setDirtLevel(F)F` |
| `probe/out_gfxstate2.json` | ok=6 bad=0 | `java.gfx.GfxEngine.setDirtLevel(F)F` |
| `probe/out_gui_defaults.json` | ok=16 bad=0 | `java.gui.Label.getWidth()F` |
| `probe/out_gui_state_b.json` | ok=6 bad=2 | `java.gui.Label.getWidth()F` |
| `probe/out_i_probe_diff.json` | ok=2 bad=0 | `java.util.PointPairSeries.Count()I` |
| `probe/out_kind_defaults.json` | ok=8 bad=16 | `java.gui.Component.getVisible()I` |
| `probe/out_kind_defaults2.json` | ok=8 bad=16 | `java.gui.Component.getVisible()I` |
| `probe/out_math.json` | ok=6 bad=0 | `java.lang.Math.sqrt(F)F` |
| `probe/out_math1.json` | ok=1 bad=0 | `java.lang.Math.sqrt(F)F` |
| `probe/out_method_diff.bak.json` | ok=4 bad=1 | `java.util.PointPairSeries.Operation_Switch(ILjava.util.PointPairSeries;Ljava.util.PointPairSeries;)Ljava.util.PointPairSeries;` |
| `probe/out_method_diff.json` | ok=6 bad=0 | `java.gfx.GfxEngine.setDirtLevel(F)F` |
| `probe/out_method_diff_chunk1.json` | ok=0 bad=24 | `java.gui.Component.getVisible()I` |
| `probe/out_strint.json` | ok=2 bad=1 | `java.lang.String.intValue()I` |
| `probe/out_strnum.json` | ok=2 bad=4 | `java.lang.String.intValue()I` |
| `probe/out_subobj.json` | ok=0 bad=12 | `java.gui.Checkbox.getBlendModeI` |

规则：结果 JSON 现在带 `spec`/`written` 溯源戳（新跑的都有）；**跑之前看文件里首用例是不是你要的那个 spec**，
跑完只信本次 stdout 的行，不要拿 `cp` 出来的文件当结论（今晚这个坑踩了三次）。

#### 10.33 `engine-call-store?` 15 条全部定性 ⇒ **按形状可分类的 backlog 清空**

上一轮把 15 条（97 点）留在"store 基址证不出是不是 native"的桶里。这轮逐条**读到 ret**（有反编译用反编译，没有的读 asm），
结论：**15 条全部写引擎内存，没有一条写 Java 字段** ⇒ Java 可见行为 = no-op，已登记（backlog 265 → 251）。

**读出来的引擎对象图（可复用）**：这些 setter 的目标不是直接来自 `FUN_00643bf0`，而是走一条链 ——
```
this --vm_get_int_field--> [native+8]            (= 引擎侧的"节点")
     --FUN_0055a230(node, 0xa0000000, 1) 校验--> 必须在
     node + per_kind_offset_table[0x7439b0][node[0x24]]     (= 每 kind 的子对象偏移表，与 getSfxTable 同一机制)
     --[+0x64]--> 车辆对象 --[+0x24d4]--> 车轴/悬挂结构  ⇒ 目标字段
```
逐条确认（写进 `vm/tufa_vm.c` 的注释里）：
| native | 地址 | 实际落点 |
|---|---|---|
| `Chassis.setSteerWheel(FF)` | `0x486b50` | `*(*(native+0x24d4)+0xbdc/+0xbe0)` |
| `Chassis.setNitro(FF)` | `0x4840d0` | `*(*(native+0x24d4)+0xcd0/+0xccc)`（第二个槽预置 `0x3f800000`=1.0f 作缺省） |
| `Chassis.setExhaustParams(FF)` | `0x4861c0` | `*(*(native+0x24d4)+0xc18/+0xc1c)` |
| `WheelRef.setCPatch(FFFF)` | `0x4877f0` | `*(native+0x24)+0x6c/0x70/0x74`、`native+0x54`（值经 `ABS` 与 `fsin`） |
| `WheelRef.setBearing(F)` | `0x4832c0` | `native+0xe4`（<0 时还会用 `FUN_005668d0(字符串,double)` 记日志） |
| `WheelRef.setRadius(F)` | `0x487280` | `native+0x50`、`native+0x58` |
| `Component.setanchor_bottom(I)` | `0x5aba20` | `native+0x1f4`（gui 控件的 native 对象） |
| `GroundRef.setFullyLoaded(I)` | `0x421e40` | `FUN_0042d670()` 单例 `+0x9c` |
| `LightType.setFlare(...)` | `0x425e90` | 栈上建节点再挂进引擎 light 对象 |
| `LightType.setLight(IFII)/(IIII)` | `0x422150/0x4220d0` | 只有栈局部 + 一次引擎调用 |
| `CarDifferential.set(IFF)/(IFFF)` | `0x483db0` | `native+0/4/8/0xc`（`abs`/`-abs`） |
| `GameRef.addTimer(FI)` / `ViewVideo.play(I)` | `0x421030/0x5a0bf0` | 仅一次引擎调用 |

**工具修正**：`native_shape_scan.py` 的"store 基址是不是 native"跟踪只认 `mov X,eax`，而这 15 条是**间接链**
（`vm_get_int_field` → `[+8]` → kind 表 → `[+0x24d4]`）⇒ 已把传播扩展到 `mov X,reg`/`mov X,[reg+off]`/`lea X,[reg+off]`
（只从 `0x643bf0` 的返回值出发），避免下次再把这类误park在可疑桶。

**里程碑：按形状可分类的部分清空了。** 今晚 backlog **299 → 250**（implemented 53 → 111，约 1,150 个调用点落地）。
剩下六桶：
| shape | n | sites | 性质 |
|---|---|---|---|
| `engine-read` | 137 | 625 | **引擎状态，模型不可造**（边界，见 `out_native_boundary.json`） |
| `no-address` | 54 | 246 | 注册表里没有 C 地址 ⇒ 要换注册器扫描法 |
| `complex` | 43 | 158 | 解析器/分配器/数组（`GameRef.command`、`Smiley.add`、`WStringList.toString`…） |
| `field-getter` | 10 | 24 | 全程无调用、只动 `[reg+off]` ⇒ **可能是真 Java 可见字段读写，下一步先读这 10 条** |
| `named-native` | 3 | 15 | 宿主提供的函数（如 `vm_system_timewarp`） |
| `trivial` | 3 | 3 | 极简（`Network.Leave()I` 等） |

**闸门**：静态面 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 83,595,025 步 / errs=0；`native_stubs 196`。

#### 10.34 `field-getter` 桶拆开：13 条其实是三类（1 条精确 + 4 条引擎内存 + 5 条边界读）

那一桶当初是按"无调用 + 只动 `[reg+off]`"归类的，读完全部 13 行才发现混着三种东西：

**① 可精确实现（不是 no-op，是常量）**
- `java.net.Network.Leave()I` = `0x41a750`：`mov eax,1; ret` ⇒ **恒返回 1**（"true"）。已按常量登记。

**② 写引擎内存 / 引擎全局表（Java 不可见 ⇒ no-op，已登记）**
- `java.gfx.GfxEngine.setFadeParams(II)V` = `0x40b130`：写 **gfx 单例** `[0x7432c8]+0x1b0/0x1b4`（引擎自己连 null 检查都没有）。
- `java.sound.Sound.nextTrack()V` / `prevTrack()V` = `0x40ea10`/`0x40ea20`：读**全局当前音乐集指针 `[0x75df08]`**（正是 `changeMusicSet` 写的那个），非空则尾跳进引擎的下一首/上一首例程。
- `java.io.Input.createHotkey(Ljava.io.Hotkey;)V` = `0x404970`：扫描引擎的**全局热键表**（`0x75d400` … `0x75db00`，步长 `0x1c`）找持有该 hotkey 的条目 → 清一个标志（`0x75d3f8 + i*0x1c = 0`）→ 再找一个空槽。
  ⇒ 顺带发现：`0x75d3f8/0x75d400…0x75db00` 是**热键表**，紧邻的 `0x75db00`（表基址）/`0x75df04`（长度）是**音乐集表** —— 同一块引擎全局数据区里的相邻表。

**③ 引擎状态读取（= 模型边界，保持不建模，但把话说清楚）**
| native | 地址 | 读什么 |
|---|---|---|
| `Replay.length()F` | `0x4280e0` | 全局回放对象 `[0x770fc0]+0x1c`（**有 null 保护**，空则返回 0） |
| `Replay.pos()F` | `0x428110` | `[0x770fc0]+0x18 / +0x1c`（**有 null 保护**） |
| `Replay.native_mode()I` | `0x4280b0` | `[0x770fc0]+0x48 & 0xff`（**有 null 保护**） |
| `Network.getIndex(I)I` | `0x4098b0` | 全局网络对象 `[0x74233c]`（**有 null 保护**，空则返回 0） |
| `System.simTime()F` | `0x4c0f70` | 帧时钟单例 `[0x743230][0]`（**无 null 保护** ⇒ 引擎里这是活时钟） |

**★ 这里有一个重要的措辞升级**：前四条**有 null 保护**，所以模型返回 0 **不是"我们编的 0"，而是引擎自己在"对象不存在"时的返回路径**
（真引擎在无回放/无网络连接时也是 0）。这是在没有实机的情况下能给出的最强说法。
`System.simTime` 没有保护：引擎里它是活时钟，我们的 0 是**模型边界**，已如实标注（未来要用它做动画/计时，必须定义确定性时钟，不能随手拿宿主时间）。

**工具**：`native_shape_scan.py` 新增 `engine-singleton-store` 形状（store 基址来自**绝对全局**的加载，如 `mov eax,[0x7432c8]`），
把"写引擎单例/全局表"从 `field-getter` 里分出来，避免这类 no-op 再混进边界桶。

**闸门**：静态面 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 84,951,657 步 / errs=0；`native_stubs 196`；
backlog `250 → 245`（implemented 111 → 116）。

#### 10.35 `Float.toString(F,String)` 精确落地 + `WStringList` 整族已解锁（分隔符 = `"
"`）

**① `java.lang.Float.toString(FLjava.lang.String;)Ljava.lang.String;`（9 点）= `0x404e60`**
body 读出来是纯计算、无引擎状态：
```
vm_get_int_field(fmt)                 -> 格式串 String 的宽字符缓冲
fld arg1 ; fstp qword                 -> f32 加宽成 double
FUN_00567cc0(dst,n)                   -> 它自己的 body 就是 _vswprintf(dst,n,fmt,va)
缓冲 -> Java String（FUN_00644cf0）
```
⇒ Java 可见语义 = **`swprintf(fmt, (double)value)`**（宽字符说明符 + **double** 实参），模型直接用宿主 C library 做同一件事。
已登记，并用 `probe/spec_floatstr.json` 做跨 VM 对拍（7 例：`%.3f/%.1f/%g/%+.2f/%.0f/%.3e/%.2f`）。
**这个对拍还兼作一件事**：检验**宿主 libm 的浮点格式化与引擎的 MSVC CRT 是否一致**（含 `%.0f` 的 1234.5 这类**tie 舍入**）——
如果两者在 tie 上不同，会立刻暴露成字符串不一致，而不是悄悄通过。

**② `java.gui.WStringList` 整族（约 40 点）已解锁**
声明面把这一族列全了：`create/destroy/add(String)/insert(String,I)/remove(I)/set(String)/set(WStringList)/clear/getCount/toString`
—— **全是 native**，也就是说这份列表的数据在引擎侧。但语义已经够精确：
- `WStringList.toString()` = `0x5bb380` → `vm_get_int_field(this)` → `FUN_005bad20(&out)`（引擎的"列表→字符串"）→ `FUN_00644cf0` 转成 Java String。
- `FUN_005bad20` 的 body 是 `for (i=0; i<count; i++) { if (i) append(分隔符); append(item_i); }`，
  其中**分隔符常量在 `0x6ff81c`**，读出来是 **宽字符 `L"
"`** ✓（紧接着的常量是 `"getY"`）。
⇒ 模型可以**完全复现**这一族：per-instance 保存 Java 侧传进来的字符串（add/insert/set/remove/clear 都是 Java 可见的实参！），
`getCount()` = 项数，`toString()` = **用 `"
"` 连接**。而且这一族**可对拍**（add×3 → getCount + toString 与真 VM 比）⇒ 下一刀。

**闸门**：静态面 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 86,518,095 步 / errs=0；`native_stubs 182`；
backlog `245`（implemented 117）。

#### 10.36 `Float.toString` 做到逐字符一致：**MSVC 兼容格式化器**（探针测出两处 CRT 差异）

这一条的价值不在"实现了一个 native"，而在**对拍测出了宿主 libm 与引擎 MSVC CRT 的真实差异** —— 这种差异如果不测，
就会变成"看起来一样、某些数值上悄悄不同"的假一致。

**测出来的两处（`probe/spec_floatstr.json`，都是字符串对拍抓到的）**
| 用例 | 引擎(MSVC) | 宿主(glibc/MinGW) | 规则 |
|---|---|---|---|
| `%.0f` of `1234.5` | `1235` | `1234` | **MSVC 平局向外舍**（away from zero），glibc 平局取偶 |
| `%.3e` of `1e20` | `1.000e+020` | `1.000e+20` | **MSVC 指数至少 3 位** |

**实现**（`vm/tufa_vm.c`）：
1. `%f` 路径：用宿主在 **N+60 位**展开（对 double 而言，凡"差的位数会影响到结果"的情形这个展开都是精确的），
   再把十进制串按 **"最近、平局向外"** 手动舍入（`round_digits_away`）⇒ 平局与非平局各自正确。
2. `%e` 路径：同样对尾数做上述舍入（含进位进指数），指数按 **`e%c%03ld`** 输出 ⇒ 至少 3 位。
3. `%g`：**自己选形式**（E<-4 或 E≥P 用指数形式，否则定点），精度换算后复用同一套舍入，再去掉小数尾部多余 0。
4. 宽度/标志：`+`、`-`（左对齐）、`0`（零填充）、宽度；**宽度解析在 `.` 处停止**
   （第一版把精度数字当宽度解析，`%5.2f` 的宽度被 `2` 覆盖 —— 被探针抓到）。

**对拍进展（同一份 16 例 spec）**
- 第 1 轮 **5/7**：抓到上面两处差异（当时 tufa 用宿主行为）。
- 第 2 轮 **11/16**：两处差异修好；新暴露 5 条 BAD，全是**我自己格式化器的 bug**（`%g` 不舍入、宽度被精度覆盖）。
- 第 3 轮 **15/16**：`%g`（`%.1g` 2.5→`3`、`%.2g` 0.125→`0.13`）与宽度三例（` 3.14` / `3.14    ` / `00003.14`）全对；
  只剩 `%.3e` 的指数位数（`%+03ld` 只保证"含符号共 3 字符"）。
- 第 4 轮：指数改为 `e%c%03ld` 后，本侧逐例核过 = 期望值（`1.000e+020` ✓），双跑确认中。
  非平局不应进位的反例（`%.2f` of `1.005` → `1.00`）也对 ✓。

**第 4 轮双跑 = `ok=16 bad=0`** ⇒ `java.lang.Float.toString(F,String)` **16/16 逐字符一致**
（含两处 CRT 差异修正后的 `1235` 与 `1.000e+020`、四条平局 `2.5→3`/`-2.5→-3`/`0.5→1`/`%.1g 2.5→3`、
两条 `%g`（`0.13`）、三条宽度/标志、以及非平局反例 `1.005→1.00`）。
⇒ 这条 native 从"没实现"变成"**读准语义 + 复现 MSVC CRT 行为 + 逐字符对拍通过**"。

#### 10.37 `java.gui.WStringList` 整族落地 + 探针拿到 **DUP**（能测有状态的序列了）

**这一族全是 native**（`create/destroy/add/insert/remove/set/clear/getCount/toString`），列表本体在引擎里；
但**每次改动都以 Java 可见的实参到达**，所以 Java 层行为可复现、也可测。逐条读出的语义：

| native | 地址 | body 读出的语义 |
|---|---|---|
| `getCount()I` | `0x5a07b0` | `return native[0x10]`（引擎列表的计数） |
| `add(String)V` | `0x5a0850` | 取 this 的 native + 该 String 的 native → 追加（计数 +1） |
| `insert(String,I)V` | `0x5a0920` | 三参：this / String / index |
| `remove(I)V` | `0x5bb410` | `FUN_005bae50(list, index)` |
| `clear()V` | `0x5bb350` | `FUN_005baa80(list)` |
| `create()V` | `0x5a07e0` | 分配 `0x18` 并构造引擎列表 ⇒ 空 |
| `destroy()V` | `0x5bb2f0` | 释放（带引用计数判断）⇒ 空 |
| `set(WStringList)V` | `0x5bb450` | `FUN_005baaf0(dst, src)` = 整体复制 |
| `toString()String` | `0x5bb380` | `FUN_005bad20` = `for i { if (i) 追加 SEP; 追加 item_i }`，**SEP = `0x6ff81c` = 宽字符 `L"
"`** |

**模型**：每实例一份 Java 字符串引用列表（`g_wsl`，按 `Inst*` 键）；上面 9 条已登记（`set(String)V` 留 stub，见下）。

**★ 探针工具补了一个 op：`("D", None)` = JVM `DUP`。** 原因：探针里每次链式调用都会**消耗栈顶的接收者**
（`M` op = "invoke on the value already on the stack, leave the result"），而 `add/insert/remove/clear` 都是 **void**
⇒ 一个 case 里**根本没法对同一对象调用两次**。加上 DUP 后序列就能写成
```
[recv]  D M(mut) Q   D M(mut) Q   …   M(observe)
```
`add` 在语料里是 **0 个调用点**（只有 `toString` 14 / `clear` 8 / `create` 2 / `destroy` 1 / `set(String)` 1 被调用），
也就是说**只有探针能测它** —— 没有这个 op，这一族等于不可验证。分隔符 `"
"` 更是**必须 ≥2 项才可见**。

**本侧结果（6/6 符合预期）**：`getCount`(add a,b,c)=**3**；`toString`=**`a
b
c`**；空表 `""`；
`add a + insert z@0` = **`z
a`**；`add a,b,c + remove(1)` = **`a
c`**；`add a,b + clear` = **0**。双跑确认中。

**刻意没做**：`set(String)V`（`0x5bb4a0`，1 个调用点）—— 它的 body 取了 String 的 native 对象再调一个
contract 尚未判死的 helper；**猜错没有任何测试能抓到**，所以留成有记录的 stub。

**闸门**：静态面 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 79,482,749 步 / errs=0；`native_stubs 182`；
implemented `117 → 125`；backlog `245 → 241`。

**`WStringList` 双跑收口 = `ok=6 bad=0`** ⇒ 整族**逐字符一致**：
`getCount`(add a,b,c)=**3**；`toString`=**`a
b
c`**（分隔符 `"
"` ✓）；**空表 → null**（与真 VM 同一个 0）；
`add a + insert z@0`=**`z
a`**；`add a,b,c + remove(1)`=**`a
c`**；`add a,b + clear` 后 `getCount`=**0**。

**这个族顺带修掉的三处（全部来自对拍，不来自推测）**
1. 我的分隔符曾被编译成**两个字符 `\`+`n`**（补丁脚本用 raw string 写 `L'
'` 写成了 `L'\n'`）⇒ 改成**显式码点 `(wchar_t)10`**，杜绝任何转义层。
2. **空表 `toString()` 必须返回 null**（真 VM 给 `None`），不是 `""` —— 引擎的列表→串构造器对空表不产出。
3. **一类"假 BAD"陷阱**：tufa 的 dump 会**转义**控制字符（行式输出），真 VM 的 dump 原样输出 ⇒ 值相同却判 BAD。
   已在 `probe_diff` 的解析里补全反转义（`
 	 
 \`，原来只有 `\s \q`）。以后任何含控制字符的字符串对拍都不会再中招。

#### 10.38 `GameRef.command(String)`（88 点，本轮最大单块）= 引擎通知分发，Java 侧 no-op

**body（反编译）**：
```
vm_get_int_field(this)                  -> GameRef 的引擎对象
串非空 -> strlen + malloc + 拷贝到 C 缓冲（宽字符）
native == 0  ->  记日志 "Reference conflict (%s@%d)"
否则         ->  FUN_00435650(native, 0, 0x10, text, 0)
```
`FUN_00435650` 是引擎的**通用通知分发**：按掩码查对象节点（`node[0x88]` 与 mask），调节点虚表处理函数
（`(*(node+0xc)+0x3c)(node[100], 0, 0x10, text)`），再遍历 `node[0x74]` 的处理函数链表、对掩码匹配的逐个调用，
并把文本载荷转给 `FUN_004351d0`。**这条路径不写任何 Java 字段** ⇒ Java 可见行为 = no-op，已登记。

**★ 已知限制（写明，不藏）**：引擎的处理函数**可以是 Java 回调**（`GameRef.registerCallback(I)` 注册的那种；
那条 native 在本模型里也是 no-op）⇒ 如果某个命令的**唯一可见效果就是触发一个 Java 回调**，我们的模型不会复现。
其余它能做的事都属于引擎侧状态。这条限制写进了 `vm/tufa_vm.c` 的注释与该 native 的登记行。

**闸门**：静态面 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 89,420,392 步 / errs=0；
**`native_stubs 182 → 179`**（新登记真被 Init 链命中）；implemented `125 → 126`，backlog 顶部已换成
`Viewport.getWidth`(88) / `getTop`(40) / `getLeft`(35) / `MetaServer.getRoomParameterI`(29)。

#### 10.39 补上注册表缺口：新扫描器 v3 → 多找回 **1,851 条 native 登记**，"无地址"桶 54 → 21

**为什么会有"无地址"桶**：老的注册表扫描器只认"`FUN_00652910` 的四个参数**全是字符串**"的调用点。
但这套引擎的常见登记形式是 **把实现函数指针也作为参数压栈**：
```
push <impl fn>      ; .text 里的函数地址
push <sig>          ; 例如 "(II)I"
push <name>         ; 例如 "getRoomParameterI"
push <class>        ; 例如 "java.net.MetaServer"
mov  ecx, <表>
call 0x652910       ; ← 注册器
```
⇒ 函数指针不是字符串，老扫描器整条跳过 ⇒ 54 条 native 一直没有 C 地址（246 个调用点无从分类）。

**新工具 `tools/native_registry_v3.py`**：
1. 扫 `.text` 里**所有** `call 0x652910`（注意操作数是 **rel32 相对偏移**，不是绝对 VA —— 这点我第一版写错了，两处：先按绝对立即数找、再按段特征位过滤，而 `dis_va.sections()` 返回的元组**没有特征位字段**，导致 `s[5]` 取不到值把全部段跳过，两次都返回 0 条）；
2. 从调用点**向前只收"连续的一段立即数实参"**（遇到非压栈/非写实参的指令就停），避免把**相邻那条登记**的压栈也吞进来
   （这一版曾把 `setLightlet` 映射到 `setShadowColor` 的地址上）；
3. 三个字符串里认 `"java."` 开头的是类名、`"("` 开头的是签名、其余是方法名；代码段里那个立即数就是实现地址
   （`is_code` 现按**段名 `.text`** 判断，之前"不是 C 字符串就算代码"的把一个数据地址 0x724dc0 当成了函数）。

**结果**：`registrar calls: 1947 → entries recovered: 1851`（老扫描器是 1776 条 union，且缺这 54 条）。
**自校验**：新扫描器给 `GfxEngine.setShadowColor(II)` 的地址是 **`0x40a8f0`**，与我上一轮**手工反汇编确认过**的地址一致 ✓
（`setShadowmap 0x40a600` / `setLightlet(String) 0x40a6b0` / `setLightlet(F×27) 0x40a760` 也各自分开、顺序合理）。

**对 backlog 的影响**：`no-address 54 行/246 点 → 21 行/96 点`；**219 / 240 行现在都有 C 地址**。
形状分布随之重算（`out_native_shapes.json`）：
`engine-read 158 行/…（边界，但**现在它们的地址与形态已知**）`、`complex 47`、`no-address 21`、
`field-getter 7`、`engine-call 3`、`named-native 3`、`trivial 2`。
新找回的条目里有一批**可精确实现**的（`Vector3.rotate` 0x4065a0 / `Vector3.distance` 0x4066f0 / `Vector3.toYpr` 0x4065a0 族、
`Integer.toString(I)` 0x404f30、`String.append(I)` 0x661620、`String.token(I,String)` 0x661490）—— 下一轮按形状实现并对拍。

**闸门**：本轮只改工具与文档，`vm/tufa_vm.c` 未动；上一次闸门（`GameRef.command` 登记后）为
静态面 `1058 / 5403 / 0 bad`、`Init.<init>` 89,420,392 步 / errs=0、`native_stubs 179`、implemented 126。

#### 10.40 用 v3 找回来的地址实现两条「可对拍」的 native + `Vector3` 族的性质判明

**① `java.lang.Integer.toString(I)Ljava.lang.String;` = `0x404f30`**（原本在"无地址"桶里，v3 找回）
```
body: unwrap -> 取 int 实参 ; FUN_00567cc0(dst, fmt@0x6e7c5c, value)
      → 该 helper 的 body 就是 _vswprintf → 十进制文本 → FUN_00644cf0 转 Java String
```
⇒ 语义 = 整数的十进制文本（整数格式化**没有**浮点那套平局/指数位数问题，直接 `%d` 即可）。

**② `java.lang.String.append(I)Ljava.lang.String;` = `0x661620`**
```
body: 取 receiver 的宽字符缓冲（vm_get_int_field(this)）+ FUN_006675de（宽字符 strlen：
      每字符 +2 前进、最后 (byte_len/2)-1）→ 拼上 int 实参的十进制文本 → 新 Java String
```
⇒ 语义 = `receiver_text + 十进制(arg)`。

**本侧逐例核对（`probe/spec_intstr.json`，11 例）**：`0` / `12345` / `-7` / `2147483647` / `-2147483647` / `1000000`；
`"abc".append(42)` = `abc42`、`"".append(-5)` = `-5`、`"x".append(0)` = `x0`、`"score: ".append(123456789)` = `score: 123456789`、
`"n".append(-2147483647)` = `n-2147483647`。跨 VM 双跑确认中。

**③ 顺带判明 `Vector3` 族的性质（下一刀的依据）**：`Vector3.rotate(Ypr)`（`0x4065a0`）读的是
**Java 对象按字段名取的浮点**：连续三次 `push <字段名字符串>; push <obj>; call 0x6435f0` ⇒ `fstp` 到局部，
其中 `0x6e7d54 = "y"`、`0x6e7d4c = "p"`、`0x6e7d48 = "r"`（即 `Ypr` 的 yaw/pitch/roll ✓），
然后交给引擎的矩阵/向量例程（`0x567e00`，就是旧清单里的"Matrix.set 数学 567e00"）。
⇒ 这一族**读的是 Java 侧状态**（不是引擎对象），因此**可精确实现、也可对拍**；缺的只是那几个矩阵例程的数学
（下一轮连同 `distance 0x4068f0` / `toYpr 0x406540` 一起做）。
`FUN_006435f0` = `vm_get_float_field(obj, 字段名)` —— 与 `FUN_00643bf0`(`vm_get_int_field(obj, 索引)`) 是兄弟。

**闸门**：静态面（上一轮）`classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 82,102,568 步 / errs=0；`native_stubs 179`；
implemented `126 → 128`，backlog `241 → 239`。

**④ 双跑结论（`vm/intstr3.log` / `probe/out_intstr3.json`）= `ok=11 bad=0`** —— 两条 native 全部逐字符一致：
`Integer.toString` 6/6（`0`/`12345`/`-7`/`2147483647`/`-2147483647`/`1000000`）；
`String.append(I)` 5/5（`"abc"+42`→`abc*`、`""+-5`→空、`"x"+0`→`x`、`"score: "+123456789`→`score: 촕`、`"n"+-2147483647`→`n`）。

★ **探针推翻了我对 `append(I)` 的第一版语义**（我按"拼十进制文本"实现，5 例全 BAD）：
真机行为是**追加"码点为该实参的那个宽字符"**，不是格式化数字 —— 证据链：`42 → '*'`（0x2A=42 ✓）、
`123456789 → U+CD15`（0xCD15 = 123456789 & 0xFFFF ✓ 逐字符相同）、`0 → 终止文本`（追加 NUL ⇒ `x` 而非 `x0` ✓）、
负数 → 低 16 位那个（不可打印/高位）字符。改成 `buf += (wchar_t)(arg & 0xFFFF)` 后 5/5 通过。

★ 双跑还暴露了两个**比较层**（不是语义层）的坑，都已修在 `tools/probe_diff.py`：
① 真机侧 dumper 会**丢掉不可打印字符**（`U+0001`、`U+FFFB` 回来后消失），tufa dump 则转义保留 ⇒
 `norm_text` 两侧统一剔除不可打印字符（字符本身由可打印用例证明，见上）；
② tufa dump 把非 ASCII 转义成**字面 `\uXXXX` 六字符**，而解析链只处理 `\s \q \n \t \r \\` ⇒
 先做 `re.sub(r"\\u([0-9a-fA-F]{4})")` 解码，否则 `U+CD15` 一侧是字符、另一侧是转义文本，假 BAD。

#### 10.41 `Vector3` 族读通了：整链是**纯数学**（下一轮实现 + 对拍的对象）

`Vector3.rotate(Ypr)Ljava.lang.Vector3;` = `0x4065a0`（85 条指令读全）：
```
unwrap(this, &this_out, &ypr)                        [0x652ab0]
读 Ypr 的三个浮点（按字段名）："y"=0x6e7d54 / "p"=0x6e7d4c / "r"=0x6e7d48   [0x6435f0]
0x567e00(&局部向量, 三个浮点)   # = Matrix.set：movss [ecx+0],[ecx+4],[ecx+8]; ret 0xc（纯打包）
0x562900(&矩阵, &Ypr)           # = 「由 Ypr 建 3x4 旋转矩阵」
0x643a40(this_向量, &局部向量)  # = 读 receiver 的 x/y/z
  3 行 x 3 分量的 SSE 乘法（mulss/addss 顺序已逐条记下：row0=xmm3*xmm0+xmm2*xmm1+xmm4*xmm2 …）
0x643a90(this_向量, &结果)      # = 写回 receiver
```

★ `0x562900` = **`Matrix.setFromYpr(out, Ypr*)`，纯 x87/SSE 三角 + 四则，无引擎状态、无全局读**：
读 `[eax]/[eax+4]/[eax+8]` 三个角 → 各自 `fsin`/`fcos` → 组合成 3x4（步长 0x10，用前 12 字节），
`ret 4`。⇒ 可精确实现；唯一风险是 **x87 `fsin`/`fcos` vs 宿主 libm 的最后一位**（同 MSVC `_vswprintf`
那一类问题），用 `(float)sinl((long double)a)` 近似 + **对拍裁决**（探针拿小角度与常见角度格点比位模式）。

★ `0x562b20`（`distance` 用）= 归一化型：算 `x²+y²+z²`，与 `[0x6eca90]` 比（**1e-6**，`comiss; ja` ⇒
平方长度小于 1e-6 直接跳过），再算 `|平方长度 − [0x6e766c]=1.0|` 若 > 1e-6 才 `fsqrt` + `fdivr 1.0`
（= 1/√len² 倒数）乘各分量 ⇒ **已单位化则不动**（避免精度损失）。两个常量按**数值**读（1e-6=0x358637bd、
1.0=0x3f800000），不要贴小端字节。

⇒ 下一轮：`0x643a40`/`0x643a90`（读/写 Vector3 的 x/y/z，名字串在 0x6e7d58="x" / 0x6e7d50="z"）
+ `0x562900` + `0x562b20` + 三条 native（`rotate`/`distance`/`toYpr`），全部走 `probe_diff` 逐位对拍。

**10.41 附：三个辅助件也读穿了（整族无引擎状态）**
```
0x643a40(vec, float* out)  读 x/y/z：0x6435f0(obj,"x"@0x6e7d58)->out[0]、"y"@0x6e7d54->out[1]、"z"@0x6e7d50->out[2]
                           （两个参数都做 null 守卫；0x6435f0 = vm_get_float_field）
0x643a90(vec, const float* in) 写 x/y/z：0x6431f0(obj,"x"|"y"|"z", value)   ⇒ = vm_set_float_field
0x567ed0(&out, &xyz)       = 「由向量求 Ypr」（toYpr 用的第三个引擎例程，待读内部）
```
`Vector3.toYpr()`（`0x406540`，33 条）：unwrap → `0x643a40` 读 xyz → `0x567ed0` 求 ypr →
`0x649170(this+8, "java.lang.Ypr")`（= load_class 按名）→ `0x656100(cls)`（= newInstance 分配）→
`0x643ae0(&ypr_local, inst)` 把 y/p/r 写进新对象 → 返回该实例。
⇒ **三条 native 的实现面 = 纯数学 + 按字段名读写 + 造一个 `java.lang.Ypr` 实例**，
tufa_vm 里 `intern`/`inst_new`/按名字段读写全都有 ⇒ 不需要任何引擎状态模型。

#### 10.42 `java.lang.Vector3` 的四条值型 native 落地并对拍 **12/12 逐位一致**

地址（v3 注册表找回）：`length()F`=`0x406810`、`sqrlength()F`=`0x406880`、`distance(Ljava.lang.Vector3;)F`=`0x4068f0`、`normalize()V`=`0x406960`。

**形状判明**：这四条**只读 receiver 的 Java 字段**（按名字读，`FUN_006435f0` = `vm_get_float_field(obj,名)`，
名串 `0x6e7d58="x"`、`0x6e7d54="y"`、`0x6e7d50="z"`），写回走 `FUN_00643a90` = `vm_set_float_field` ⇒
**不碰任何引擎状态**，因此可以精确实现、也可以逐位对拍（这是"选球先按返回类型是不是值分桶"的第一批果实）。

```
量          x87 数学                                  实现口径
length      fsqrt(x*x + y*y + z*z)                     80 位中间量（long double）+ 一次写回 f32
sqrlength   x*x + y*y + z*z                            同上（不舍入）
distance    fsqrt(dx*dx + dy*dy + dz*dz)               差值在 80 位里做
normalize   if (len != 0) v *= (1.0f/len)              这里的 divss/mulss 真的是 f32（SSE），按 f32 做
```
`normalize` 的判据是 `len != 0`（x87 `fucompi` + `lahf/test ah,0x44` 惯例 ⇒ 只有长度恰好为 0 才跳过），
且**写完不返回**（写回在分支之后无条件执行）。

**对拍结果 `probe/out_vec3.json` = `ok=12 bad=0`**（`vm/vec3.log`）：

| 用例 | 两侧一致的证据 |
|---|---|
| `sqrlength(3,4,12)` | `0x43290000` = 169.0 精确 |
| `length(3,4,12)` | `0x41500000` = 13.0 |
| `length(1,1,1)` | `0x3FDDB3D7` = √3（无理数也逐位相同 ⇒ 80 位路径对） |
| `length(0,0,0)` / 同点 `distance` | 0 |
| `length(1e-20,0,0)` | `0x1E3CE508`（次正规区） |
| `length(1e18,1e18,0)` | `0x5D97EF1E` ≈ 1.4142e18（平方后仍留 80 位精度） |
| `distance((0,0,0),(1,2,2))` | 3.0 精确 |
| `distance((1.5,2.5,3.5),(-4.25,0.75,100))` | `0x42AF2305` ≈ 87.61 |
| `sqrlength(2.5,-3.75,0.125)` | `0x41A4B400` = 20.578125 |
| `normalize(3,4,0)` 后再测长度 | `0x3F800000` = 1.0（写回生效） |
| `normalize(0,0,5)` 后再测长度 | 1.0 |

**★ 本轮抓到的工具级假故障（差点写成"新 native 全错"）**：探针 `("O", [类, ctor描述符, 实参])` 里
ctor 描述符写了 `"(FFF)V"`（带返回类型）⇒ 生成的 `NT` 条目是 `(FFF)V`，而容器里存的是 `(FFF)`
（构造器不带返回类型，docs/65 §9.1）⇒ `resolve_member` 失败、真机报 `unresolved method ref N`、
**12 例全 0**。改成 `"(FFF)"` 后立刻全绿。诊断手法：`tufa_src.py --dump probe/<类>.class | grep NT`
与容器的 `NT` 行逐字比对。另注：`json.dump(indent=1)` 后数组元素各占一行，**用 sed 改不动**。

**闸门**：静态 `classes 1058 / fields 5403 / 0 bad`；`Init.<init>` 85,234,979 步 errs=0（实现变多 ⇒ 跑得更深）；`native_stubs 179`；
implemented 128→**132**，backlog 238→**235**。

#### 10.43 `Vector3.toYpr` / `Ypr.toVector3` 落地并对拍 **14/14**；★发现了 x87 `fpatan` 的零符号规则

**两条 native 与两个共享例程**
```
0x562900 Matrix.setFromYpr(out, ypr*)   三个角各 fsin/fcos（x87）+ SSE 组合；行距 0x10、只用前 12 字节：
        M[0..2]  = (S2*S1*S0 + C2*C0,  C2*S1*S0 - S2*C0,   C1*S0)
        M[3..5]  = (S2*C1,             C2*C1,              -S1)
        M[6..8]  = (S2*S1*C0 - C2*S0,  C2*S1*C0 + S2*S0,   C1*C0)
        （S0/C0=sin/cos(yaw)、S1/C1=sin/cos(pitch)、S2/C2=sin/cos(roll)）
0x567ed0 ypr-from-xyz   yaw = fpatan(-x, -z)；pitch = fpatan(y, sqrt(x*x+z*z))；roll = 0
        退化（x==0 && z==0）：yaw=0、pitch=±pi/2、roll=0（常量 0x6ec27c/0x6f662c）
0x406540 Vector3.toYpr()Ljava.lang.Ypr;        读 xyz -> 0x567ed0 -> load_class("java.lang.Ypr")
                                               -> newInstance -> 按名写 y/p/r
0x406bf0 Ypr.toVector3()Ljava.lang.Vector3;    读 y/p/r -> 0x562900 -> v = -(M[2], M[6], M[10])
                                               -> load_class("java.lang.Vector3") -> 按名写 x/y/z
```
★ `toVector3` 取的是矩阵的**第三列**（`M[2],M[6],M[10]`），不是"一行"——我第一版按行取，
**自检立刻否掉**（`Ypr.toVector3().length()` 必须是 1.0，按行得到 1.1403；按列得到恰好 `0x3F800000`）。
即：**对象结果的 native，先用"必然成立的不变量"筛自己的实现，再上双跑**。

**验证手法（返回对象也能比）**：`Vector3`/`Ypr` 的 `toString()` 都是 Java 字节码 ⇒ 探针用链式
`("M",[类,"toString","()Ljava.lang.String;",[]])`，字段类型写 `Ljava.lang.String;` 就能比文本（抓轴序/正负）；
再用值型 native 做强度检查：`Ypr.toVector3().length()` 比位模式、`v.toYpr().toVector3()` 与 `v` 的 `distance`
（对 (1,2,2)：两侧都恰好 2.0 = |v|-1）。

**★★ 关键语义发现：x87 `fpatan` 在结果为零时返回 `+0.0`，而 C 的 `atan2` 保留分子符号**
判据：`Vector3(0,0,-1).toYpr()` 引擎给 `+0.000`，我的 `atan2l(-0.0,+1.0)` 给 `-0.000`
（`toString` 文本逐字符不同 ⇒ 单例 BAD，`vm/ypr.log`）。修法：结果为零则 `+= 0.0f`（把 `-0.0` 收成 `+0.0`）。
并用 `F_bits 0x80000000` 真塞负零做了三个判别用例（x=-0.0 / y=-0.0 / 两个都 -0.0）⇒ 修后 14/14。

**对拍结果**：`vm/ypr.log` = `ok=10 bad=1`（就是那条零符号）⇒ 修后 `vm/ypr2.log` / `probe/out_ypr2.json` = **`ok=14 bad=0`**。

#### 10.44 `Vector3.rotate(Ypr)` 落地并对拍 **8/8**

`0x4065a0`（85 条指令读全）：读 Ypr 的 y/p/r → `0x567e00` 打包 → **`0x562900` 建 3x4** → 读 receiver 的 xyz →
三个 SSE 点积（消耗矩阵第 0x34/0x44/0x54 起的三行，逐条照抄顺序）→ `0x643a90` **写回 receiver** → 返回 `this`
（**原地修改、不新建对象**）。⇒ 语义 = `M·v`，与 `Ypr.toVector3()` 共用同一矩阵（后者取 −第三列 ⇒ 前向轴相反的手性约定 ✓ 自洽）。

**先自查（不上真机就能否错的硬不变量）**：`(1,2,3).rotate(Ypr(0,0,0))` = `1.000,2.000,3.000`（ypr 全零时矩阵恰为单位阵）；
`rotate(...).length()` 在 ypr(0.7,-0.4,0.25) 与 ypr(π/2,0,0) 两例上给出**同一个位模式**（保长）✓。

**对拍**：`vm/rot.log` / `probe/out_rotate.json` = **`ok=8 bad=0`**（三个基向量的像 + 恒等 + 保长 + 两个任意角）。

#### 10.45 `System.timeWarp(F)F` 落地；native 剩余面首次落成**可验收的边界清单**

**① `System.timeWarp(F)F` = `0x405d60`**（8 调用点）：先把全局 `[0x771164]`（镜像值 `0x3d913d7c` = 0.07091805）读进返回槽，
再做 `if (f >= [0x733b0c] (=0.0f)) { 引擎调用 0x4338e0(0x771050, f); [0x770fe4] = 0; }`，最后返回那个**调用前**读到的全局值。
⇒ 返回值与 f 无关、也不依赖引擎调用的结果 ⇒ **可对拍**；只有"引擎调用是否反写 0x771164"这一条未建模，
用同一 clinit 内**两次调用**的顺序探针检验（若真机第二次返回变了，就说明引擎改了它）。

**② 剩余 native 面 = 边界清单（新生成物 `out_native_boundary.md`）**：由 `tools/native_shape_scan.py` 的
`out_native_shapes.json` 机械生成（**232 行 / 906 调用点**，每行带形状、调用点、实现地址、指令数）：

| 形状 | 行数 | 调用点 | 为什么留在边界 |
|---|---|---|---|
| engine-read | 157 | 600 | 读引擎对象字段（视口/资源/网络对象）⇒ 沙箱造不出，模型只能给出其 NULL 守卫路径的值 |
| complex | 43 | 157 | 多次调用/多分支，需逐条读后定性 |
| no-address | 20 | 104 | 注册表查不到实现地址 ⇒ 无 C 体可读 |
| field-getter | 7 | 20 | 只读写 `[recv+off]` 的 Java 字段 ⇒ **下一批可精确实现** |
| engine-call | 3 | 23 | unwrap + 一次引擎动作、不写 Java 字段 ⇒ Java 可见 no-op |
| trivial | 2 | 2 | 极短，待逐条读 |

⇒ **验收口径写明**：engine-read / no-address 这些**不以"实现"收口，而以"实机验收"收口**（`docs/66`），
文档里不再把它们算成"未完成"。下一批可精确实现的是 `field-getter`（7 行）与 `trivial`（2 行）。

**闸门**：静态 `1058/5403/0 bad`；`Init.<init>` 87,472,737 步 errs=0；implemented 137，backlog 232。

#### 10.46 把「引擎读」桶再切三刀：新工具 `tools/native_guard_scan.py`

上一节把 232 行边界整体记成"engine-read ⇒ 实机验收"太粗。本轮按**引擎指针有没有 null 检查**机械二分
（判据从反汇编直接读：指针来源是 `call 0x643bf0`(vm_get_int_field) 或已知单例全局 ⇒ 找它第一次被解引用的位置 ⇒
中间有没有 `test reg,reg`；**`0x8dcb2c` 要排除**——它是"字段索引"全局，不是指针，docs/65 §10.31）：

| 桶 | 行数 | 调用点 | 含义 / 验收口径 |
|---|---|---|---|
| **guarded-obj** | 54 | 323 | 引擎指针**有 null 检查** ⇒ 无引擎附着时**模型返回 0 就是引擎自己的 null 路径** ⇒ 沙箱下**已经忠实**，不算"未实现" |
| **unguarded-obj** | 64 | 129 | 指针**无 null 检查**（如 `System.simTime` 直接 `mov eax,[0x743230]; fld [eax]`）⇒ 新进程里本就是未定义值，我们的 0 是**模型选择** ⇒ **只能实机验收** |
| **other** | 46 | 168 | 值不来自引擎指针（来自 Java 字段/实参/其它路径）⇒ **下一批可精确实现** |

产物 `out_native_guards.json`（每行带桶、实现地址、指针来源、指令数）+ `out_native_boundary.md`（按桶重排）。

⇒ 对上级的结论：那 204 行"边界"里真正需要实机的只有 **64 行**（129 点）；**54 行**在沙箱下已忠实；
**46 行**是可继续实现的（其中 `java.lang.String.length()I`(0x661730, 7 点)、`gui.Texture.destroy`(10)、
`gfx.GfxEngine.setShadowmap/setLightlet`(17/14) 是最明显的前几名）。

#### 10.47 `System.timeWarp(F)F` 收口 **7/7**；★探针逼出"引擎状态型"native 的正确读法

**第一次双跑 = 0/5**（我按"返回镜像里的全局常量"实现）：
```
真机: -1.0 -> 1.0 | 0.0 -> 1.0 | 5.0 -> 0.0 | -0.5 -> 5.0 | 0.25 -> 5.0
```
把它当**时序**读只有一个解释（三步全部对上）：
① 返回值 = **调用前**读到的"时间倍率状态"；② **负数 f 跳过**引擎调用 ⇒ 状态不变（`-1.0→1.0`；`-0.5→5.0` 沿用上次的 5.0）；
③ **非负 f** 触发引擎调用 ⇒ **状态 := f**（`0.0` 之后返回 0.0、`5.0` 之后返回 5.0）；④ 新进程里状态初值 = **1.0**。

★ **我上轮犯的错**：把镜像 `[0x771164] = 0x3d913d7c (0.0709)` 当运行时初值 —— 它在**引擎引导期被改写**。
⇒ 这是"**镜像值 ≠ 运行时值**"的第二次现身（第一次是 `0x771ab8` 的 bss）：**能写全局的 native，其全局初值要靠顺序探针测，不能从镜像读**。
修后本侧 7 例完全复现真机前五行并一致外推（`1.0,1.0,0.0,5.0,5.0,0.25,2.0`），**第二次双跑 `ok=7 bad=0`**（`vm/timewarp2.log`、`probe/out_timewarp2.json`）。

★ 方法论（已进 skill）：**"写在函数开头、出现在 epilogue 的局部" = 该值是调用前快照**；这类"读状态→可能写状态→返回旧状态"的 native
（典型：倍率/模式/开关型 setter）必须用**同一 clinit 内多次调用**的顺序探针来定，单次调用看不出写回行为。

#### 10.48 `java.lang.String.length()I` 落地 —— 顺带修掉**模型里一个真缺陷：字符串常量没按 UTF-8 解码**

`0x661730`：`vm_get_int_field(this)` 取宽字符缓冲 → `FUN_006675de`（两字节一步走到 NUL，返回 `字节数/2 − 1`，
那 −1 是扣掉被数进去的 NUL）⇒ **正是字符数**。实现 = 宽字符计数。第一次双跑 **6/8**：

```
BAD  length("촕"        )  tufa=3   real=1      ← 真机把 CJK 当 1 个字符
BAD  length("LASR 反编译 line") tufa=19  real=13
```
★ 根因不是 `length`，而是**模型里的字符串常量加载**：`tufa_vm.c` 的 `widen()` 把容器的 **UTF-8 字节逐个**当成 UTF-16 码元
（`w[o++] = (u8)s[i]`）⇒ 一个 CJK 字符变 3 个。真机按 JVM 规则**解码 UTF-8 → UTF-16**（所以 13 = 5+3+5 ✓）。
修法：`widen()` 改成真解码（2/3/4 字节序列 → 码点；≥U+10000 出代理对；非法字节回退旧行为以免丢字节；ASCII 完全不变）。
修后本侧 `1` 与 `13` ✓，**双跑 8/8**。

⇒ 教训：**含非 ASCII 的探针用例是"字符串表示层"的探照灯**；ASCII-only 的用例永远照不出这类缺陷。
`String.length` 之前从未被调用过（语料 7 点），是本次从"other 桶"里挑出来才把这个问题带出来的。

#### 10.49 修 `widen()` 后的回归检查 + **native 层合并回归闸门**

① **回归**：`widen()` 是全局字符串面（所有字符串常量、类名/方法名都走它），改完立刻
   (a) 重跑静态闸门 `classes 1058 / fields 5403 / 0 bad` ✓、(b) `Init.<init>` **95,403,758 步 errs=0**（步数从 82.4M 变到 95.4M ⇒ 解码真改了执行路径）、
   (c) 重跑 `spec_intstr.json`（`String.append/toString` 老链路）**11/11** ✓ ⇒ 无回归。
② **新闸门 `probe/spec_regress.json`（114 例 / 16 族）**：`tools/spec_merge.py` 把本次会话验证过的各族用例合并
   （float 格式 · 整型/字符串 · 宽字符串/WStringList · Vector3/Ypr 数学 · 两条 rotate · timeWarp · gfxstate · `<i>` · 纯计算），
   一条命令重验整个 native 层：

   ```
   python tools/spec_merge.py
   python tools/probe_diff.py --spec-file probe/spec_regress.json --chunk-size 12
   ```
   为什么需要它：**各族原本是分批验证的**，改动一旦碰到共享代码（字符串处理、浮点格式化、native dispatch 表）
   就可能悄悄打歪另一族 —— 本次 `widen()` 就是同类风险，只是被单独复跑挡住了。
   （gui 族需要 `PROBE_STUBS=""`，与这批默认桩不同，仍分开跑。）

**本轮登记**（读到底后落）：`gui.Texture.destroy()V`(0x5bb8a0)、`gfx.GfxEngine.setShadowmap(String)`(0x40a600)、
`setLightlet(String)`(0x40a6b0) 三条引擎调用型 no-op；`java.lang.String.length()I`(0x661730) 精确实现。
implemented **141**，backlog **228**；登记落地用"名字从 backlog 列表消失"核对（不只看计数）。

#### 10.50 合并回归闸门第一次跑：**它自己先暴露出 6 处"非回归"的假 BAD**（这正是它该干的事）

`probe/spec_regress.json` 首跑 `ok=95 bad=19`（114 例）。逐条定性后**没有一条是 native 层回归**：

| 类别 | 条数 | 定性 |
|---|---|---|
| 真机侧整块丢失（`real=None`，chunk 4 报 `StopEmulation: data-exec 0x20199b10`） | 13 | **harness 故障**：该 chunk 里有一个重类用例把真 VM 带崩 ⇒ 整 chunk 的真值全丢。不是语义差 |
| `intValue("abc")` tufa=0 real=**540738480** | 1 | **早已记档**：原版 `swscanf` 不匹配时返回值是**未初始化栈垃圾** ⇒ 不可复现 |
| `WStringList` 的 2 例（`getCount`/`toString`） | 2 | 这些用例走的是**gui 控件工厂**（`("C", …)` 带布局串）⇒ 需要 `PROBE_STUBS=""`，属于 gui 组，混进默认桩组必错 |
| `GmRace/GmTrial.getMapSplineId(II)I`、`PointPairSeries.Interpolate(FFFFF)F` | 3 | **探针用例自身构造错了**：这三条在容器里是**有 Java 代码的实例方法**（`M 9 107 106 14 2` ⇒ code=14，非 native），却被写成 `static=True` ⇒ 两侧都拿不到接收者，真机返回"最后一个实参"（残留在 eax 里），我们返回 0。**0 个语料调用点** ⇒ 游戏自身字节码不依赖它们 |

⇒ **闸门改造**：`tools/spec_merge.py` 明确排除 `spec_wsl.json`（gui 组）与 `spec_pure.json`（待修构造），
合并集变成 **105 例 / 14 族**（全部是此前逐族验证过的），并在脚本里就地写下排除理由。

★ 教训（已进 skill）：**合并闸门的"假 BAD"必须先按"真机有没有给值"分流** —— `real=None` 是 harness 丢块，
不是语义差；否则会把 13 条 harness 故障当成 13 个回归去查。

#### 10.51 合并回归闸门第二跑 **103/105**，并落 6 条新 native

**闸门**：`vm/regress2.log` = `ok=103 bad=2`，**两个 BAD 都是早已记档的 `swscanf` 未匹配返回栈垃圾**
（`intValue("abc")` real=540738480、`floatValue("xyz")` real=540738768 —— 两个垃圾值还相差 288，典型的栈残留）；
`real=None` 计数 **0** ⇒ 再无 harness 丢块 ⇒ 这 105 例现在是一条可复跑的 native 层闸门。

**新登记 6 条**（都读到 ret；容器 flags 顺带确认了静态性：`265`=public|native|**static**、`257`=public|native=实例）：
| native | 地址 | 形状 | 落法 |
|---|---|---|---|
| `GfxEngine.preparetrack()V` | 0x40b170 | unwrap + 一次引擎调用 `0x4d6d60` | no-op |
| `Sound.initRooms(I)I` | 0x40ed20 | `0x565c00(0x897380, arg)` 后 **`mov eax,1`** | **恒返回 1** |
| `Sound.initReverb()I` | 0x40ef00 | `0x565c60(0x897380)` 后 **`mov eax,1`** | **恒返回 1** |
| `Thread.sleep(I)V` / `sleep(II)V` | 0x661b70 | VM 的 sleep 原语（两个 Java 签名同址） | no-op（单线程模型） |
| `Object.wait()V` | 0x662730 | yield 原语 | no-op（单线程模型） |

`initRooms/initReverb` 的返回**与引擎调用无关** ⇒ 可对拍（`probe/spec_sound.json` 3 例）；
`sleep`/`wait` 的 body 要 `[this+0xc]`（真线程对象）⇒ **沙箱里构造不出、不可对拍**，按 body 读数登记并注明。

#### 10.52 ★ 实机第一次读数（主菜单画面，只读挂接）

工具 `tools/live_read.py`：`OpenProcess(PROCESS_VM_READ|QUERY_INFORMATION)` + `ReadProcessMemory`，
**不挂调试器、不暂停线程、不写内存**（游戏全程满速运行）。一个 WOW64 坑：64 位宿主进程枚举 32 位目标
必须用 `EnumProcessModulesEx(..., LIST_MODULES_ALL)`，否则只看到 64 位 ntdll、拿不到 LASR.exe 的基址。

实测（`out_live_menu.txt`，module base = 0x400000 = 镜像基址，无 ASLR 偏移）：

| 全局 | 地址 | 实机值 | 意义 |
|---|---|---|---|
| timeWarp 状态 | `0x771164` | **1.0** (`0x3f800000`) | ★ **证实**顺序探针推出的"运行时初值 = 1.0"，而镜像里是 0.070918 ⇒ 该 native 的模型口径正确 |
| `System.simTime` 指针 | `0x743230` | → `0x779f20`，f32 = **113.4658** | sim 时钟（秒）；这行 unguarded-obj 的真值形态 = 单调递增计时器 |
| Replay 单例 | `0x770fc0` | → `0x0d76fd00`；`+0x18 pos=113.4672`、`+0x1c len=0.0`、`+0x48 mode&0xff=0` | `length()F` 的实机值 **0.0**（未载入录像）；`pos()F` 是 `pos/len` ⇒ **除零**（引擎确实没有守卫） |
| Network 单例 | `0x74233c` | **0** | `Network.getIndex(I)I` 的 null 路径 = 返回 0 ✓ 与模型一致 |
| MetaServer 全局 | `0x75f218` | `0x04f48f38` | 非空 ⇒ 它的虚调用路径会真的走 |
| gfx 单例 | `0x7432c8` | `0x77a7f8` | 非空 |
| 字段索引全局 | `0x8dcb2c` | **0** | 每次 native 调用前才被写（主菜单空闲时是 0） |
| `0x733b0c` / `0x6e766c` / `0x6eca90` | — | 0.0 / 1.0 / 1e-6 | 三个常量与语料读数一致 ✓ |
| `0x770fe4`（timeWarp 会清它） | `0x770fe4` | **1.49968** | 实机活跃值（模型未建模，仅记录） |
| 每 kind 偏移表 | `0x7439b0` | **0** | ⚠ **本条判断错了，就地订正（见 §10.53）**：`0x7439b0` 不是"指向表的指针"，**表本身就内联在这个地址**；把它当指针读才得到 0。表是**静态数据**，与有没有车无关 |

#### 10.53 ★ 订正 §10.52 的最后一格 + 车屏幕第一次读数

**订正**：`0x7439b0` 是**内联的每 kind 偏移表**（不是指针）。把它当 `ptr` 读，主菜单与有车画面**都得到 0**——
这本身就说明判断错了（有车仍为 0 就不可能是"没有车所以没建表"）。按 22×i32 直接读，实机值与镜像**逐项相同**
（静态数据）：

```
kind :  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15  16  17  18  19  20  21
off  :  0 152 152 152 100 100 100 100 100 100   0   0 100 100 100   0   0   0 100 100   0 100
```
⇒ `getSfxTable(i)` 的 `node + table[kind]` 里**表这一半是静态常量**（可直接从镜像取），
缺的**只是每个实例的 node 指针**（Java 对象 fields[0] 里那个 native 指针）。

**车屏幕上的尝试（对象发现，`tools/live_find_part.py`）**：只读扫 RW 堆，判据 = `+0x00` 指向镜像、
`+0x24` 是合法 kind，再验 `node[kind] → +0x64 → +0x98 → +0x68` 全链可读。8 个候选里**大多数是假阳性**
（"vtable" 值是 `0x4d0049`/`0x610073` 这类 ASCII、链尾读到字符串表）⇒ 判据太松，下一轮要收紧：
vtable 必须 4 字节对齐且 `[vtable]` 指向 `.text`、`+0x10` 的 type-list 头要能走通、kind 必须非 0。

★ 教训：**"读出来是 0" 本身是判据** —— 有车/无车都读到 0，就该先怀疑"这不是指针"，而不是继续等下一个画面。

#### 10.54 `Chassis.getSfxTable(i)` 读全 + 实机节点发现（有车画面）

**静态读全**（`0x4884e0`，前 25 条只到"i<0 → NULL"，续读 0x488512.. 才见真身）：
```
if (i < 0) return NULL;
native = vm_get_int_field(this);  node = [native+8];      两者为空 → NULL
if (FUN_0055a230(node, 0xa0000000, 0) < 0) return NULL;    valid 校验
sub = node + DAT_007439B0[byte(node+0x24)];
q   = [sub + 0x64];
if (i > 3) return NULL;                                    ★ 只有 0..3
switch (i): 0 -> [q+0x24d4] + 0x128   1 -> + 0x370   2 -> + 0x5b8   3 -> + 0x804
```
⇒ **一张表 = 一个轮子**，挂在**车轴/悬挂结构** `q+0x24d4`（正是 `Chassis.setSteerWheel` 用的那个字段）✓ 与 §10.29 自洽。

**模型缺陷（已修）**：旧实现只挡了 `i < 0`，`i > 3` 仍返回缓存对象 —— 引擎那边是 **NULL**。
修法 `if (i < 0 || i > 3 || !t) return 0;`。注：`getSfxTable(` 在语料里**一个调用点都没有**
（`grep out_pseudo` 为空）⇒ 这条差异是**保真性**修正，不影响游戏自身行为。

**实机节点发现（`tools/live_find_part2.py`，只读）**：收紧判据后（kind 非 0 且表项非 0、Q/R/V 必须是堆指针、
**Vehicle 首 dword 必须是指进 `.text` 的真 vtable**）一次扫到 5 个结构自洽的引擎节点（kind 8 与 12），
`N[0:0x40]` 里规律出现 `2c076f00`（vtable 0x6f072c）+ 同区堆指针 ⇒ 当时判为"链在真车上是通的"
  ⚠ **下一轮被 §10.55 推翻并撤回**：拿 `q+0x24d4`（轴结构）当轴心读四个轮子表时，"轴指针"是不可读的 `0x8b04e600` ⇒
  这些候选只是"凑巧满足签名"的假阳性，**不能**用来读 per-instance 真值。
可以继续用于读每个实例的 wheel 子结构（模型按构造不可能复现那些内容 ⇒ 边界，但**规则**现在是精确的）。

#### 10.55 ✗ per-instance 真值的实机读取：这条路没走通（§10.54 的过度结论已就地撤回）

**做法**（`tools/live_wheels.py`，只读）：实机堆里按签名找 Part 节点 → `sub=N+table[kind]` →
`q=[sub+0x64]` → `R=[q+0x98]` → `Vehicle=[R+0x68]` → `axle=[q+0x24d4]` → 四个轮子表
`axle+{0x128,0x370,0x5b8,0x804}`。

**结果**：第一个候选 `q+0x24d4` 直接是 0；加"轴必须非空"再筛，挑中的那个 `axle = 0x8b04e600`，
四个轮子地址**全部不可读** ⇒ 链是断的。

**结论**：我的堆扫描签名**区分不了"真 Part 节点"和"凑巧字节合适的内存"**（`+0x24` 落在 1..21、
`[sub+0x64]` 指向可读内存、Vehicle 首 dword 像 vtable 就会通过）⇒ 不能用它读 per-instance 真值。
§10.54 里"这条链在真车上是通的"是过度结论，已在原处标注撤回。

**仍然成立的**：① kind 表 = 内联静态数据（实机 == 镜像，22 项）；② `getSfxTable` 的**公式**
（含 `i>3 → NULL`、四个轮子偏移）来自反汇编，模型的 null 笔径已按它修正；③ `q+0x24d4` 与
`setSteerWheel` 同字段，那一条来自更早一轮的反汇编。

**要走通 per-instance 真值，只有两条不靠猜的路（下一轮二选一）**：
* **A. VM 侧对象发现**：从真 VM 的类表/实例登记里取到 `java.game.parts.Chassis` 的实例，直接读它的
  `fields[0]` → native 指针 → 再走链。**精确**，但需要先摸清真 VM 的实例登记结构。
* **B. 插桩**：像力回馈代理那样注入一个**只读 DLL**，在 `getSfxTable`/`Part.getVehicle` 入口把
  `node/q/axle` 打日志（需要你同意加载一个 DLL；安装目录仍然不写）。

#### 10.56 Route A（VM 侧对象发现）进展：实例表结构读出来了，命名层还差一级

**已读实的两件（只读，游戏在跑）**：
1. **实例表**在 `[0x8ddb50]`（不是 env —— `Instance::initialize` 里是 `mov ecx,[0x8ddb50]; push inst; call 0x644780`）。
   `0x644780` 的算术 `(inst - seg_base) / 24` 与常量 `0x4e20` 证明：**实例 0x18 字节、每段 20000 个**；
   实机 `{segs=..., count=3}` ✓（先前按 env 读得到 count=37,815,680 是错的口径）。
2. **活实例判据**：`[I+0x14] & 0x10`（`Instance::initialize` 里 `or eax,0x10` 写的就是这个）+ `[I+0]` 指向的
   结构满足 `[+0xc] == env`。用它枚举得到 **24,179 个活实例 / 1,656 个"类状"结构** ✓ 数量级合理
   （先前不过滤得 41,782/19,090，全是空闲槽的自由链）。

**顺带读实的类结构**（按名字可精确定位）：类结构 `+0x2c` 处是**内联 ASCII 类名** ——
`0x04efa8bc` 就是 `java.game.parts.Chassis`（`[+0xc]==env` ✓），它的 `[+0x54]` 指着
`java.game.parts.Wheel` 那个类 ✓ ⇒ **"按名字找类"这条路是通的**。

**卡住的一级**：实例的 `[0]` **不指向**这些带名字的类结构（1,656 个"类状"结构里，前 0x80 字节
**没有一个**含 `Chassis/Wheel/Part` 字样）。二者之间还有一层（很可能是每类的 "class info" 结构，
`Instance::initialize` 里 `[class+0xc]` 那个指针所指的东西，其 `+0x20` 是实例计数）。
⇒ **per-instance 的轮子真值仍未读到**，下一步要问的问题很具体：`[I+0]` 与带名字的类结构之间的偏移/间接关系是什么。

工具：`tools/live_find_class2.py`（按名字找类 + 实例表枚举，含口径注释）、`tools/_live_live_inst.py`（活实例过滤）、
`tools/_live_cls_dump.py`（类结构 0x60 字节 + 指针探测）。

#### 10.57 ★ 收束：per-instance 引擎真值**不属于"重编译后功能一致"的验收面**（附本轮到此为止的证据）

**这一轮又试了三层，仍没拿到 per-instance 值**（全部只读）：
* 实例记录（1,657 个）的前 0x60 字节里**没有任何一个** dword 指向"点号分隔的 ASCII 类名" ⇒
  类名不是以 ASCII 指针形式挂在记录前置字段里（很可能是 Java String 对象再指一层）；
* 前一轮按"名字缓冲区地址"去匹配实例记录也是 0 命中 ⇒ **当前画面里没有 `Chassis` 的活实例**
  （车身上的零件是 `vehicles.classes.*` 那一族 Part，`Chassis` 只在比赛/玩家车路径上出现）。

**但更重要的是范围判断**（这才是该收的地方）：
| 层 | 谁实现 | 我们是否需要"造出真值" |
|---|---|---|
| VM（字节码解释器、对象模型、native 分派） | **我们要重编译的就是这一层** | 需要：已用对拍逐位验过 |
| native 函数本身（引擎 C 代码） | **原版的一部分、不重写** | 只需要**调用契约**正确（返回类型/栈/副作用形态），不需要复现它读到的引擎内存值 |
| 引擎数据结构（node/wheel/chassis） | 原版引擎内部 | **不需要**：重编译产物里这些仍由原版引擎写读；我们的模型只是在**沙箱**里用 0 顶替 |

⇒ 那 64 行 `unguarded-obj`（读引擎状态、无 null 守卫）在**重编译产物里不存在**——它们会直接调用原版 native。
沙箱里返回 0 只是为了让 `<clinit>`/方法级探针能跑完，不该被当成"未完成的实现"。
**结论**：per-instance 真值这条线**到此为止**（不再往 VM 的 GC/类登记里挖），`out_native_boundary.md` 里
`guarded-obj`/`unguarded-obj` 两桶统一标注"沙箱顶替值，重编译产物由原版 native 提供"。

**本轮实机收获仍然入账**（已写进 §10.52–10.56）：`0x771164` 运行时 = 1.0（证实 timeWarp 口径）、
kind 表是内联静态数据且实机==镜像、`getSfxTable` 的 `i>3→NULL` 修正、实例表 `[0x8ddb50]` 的
`{segs,count}` + `0x18/20000` 结构、类名可精确定位（`+0x2c` 内联于带名类结构）。

#### 10.58 `FindFile.close` + `Network.getIndex` 落地；FindFile 的 first/next 撞上**表驱动注册**缺口

**落地两条**（都读到 ret）：
| native | 地址 | 形状 | 落法 |
|---|---|---|---|
| `java.io.FindFile.close()V` | 0x404510 | `h = 按名取 this."handle"` → **先写 handle = −1** → `if (h>=0) 引擎 0x560390(h)` | 写字段（引擎关句柄不建模）⇒ **Java 可见副作用保真**；类里确有 `handle`/`flags` 字段 |
| `java.net.Network.getIndex(I)I` | 0x4098b0 | `ecx=[0x74233c]; if(!ecx) return 0;` | 单例为空的路径就是模型的 0 ⇒ **显式登记** |

闸门：implemented 147→**149**，backlog 222→**220**；两条名字**已从 backlog 列表消失**（登记落地核对）；
静态 `1058/5403/0 bad`；`Init.<init>` 88,875,004 步 errs=0；**`native_stubs 178→176`** ⇒ 两条新登记在驱动跑批里真的被命中。

**✗ 新缺口（FindFile 的目录枚举族）**：`FindFile.first(String;I)String`（7 调用点）与 `next()String`
在容器里是 native，但 (a) v3 注册表里**没有** `java.io.FindFile`；(b) 整个 exe 里 `"java.io.FindFile"`
字符串**只出现一次**（`.rdata` 的字符串簇），而**没有任何 `push imm32` 或双字引用指向它**
⇒ 这一族是**表驱动注册**（不是逐条压栈那种形态）。⇒ 不能猜地址，得先扩一个"表驱动注册"扫描器变体
（与上一轮补 54 条缺口同类，只是形态不同）。

★ 顺带记：`FindFile.close` 的字段写**探针观测不到**（void 返回 + 没有 Java getter 能链式读 handle），
所以它是"**读 body 落地、非探针可验**"的条目 —— 与 `Thread.sleep`/`Object.wait` 同类，文档里要分开标。

#### 10.59 ★ 表驱动/无引用注册的恢复手法：用**字段名字符串**定位整个类族（FindFile 实例）

**问题**：`java.io.FindFile` 在 v3 注册表里完全没有；exe 里 `"java.io.FindFile"` 字符串只出现一次且
**没有任何引用**（`push imm32`/双字都没有）⇒ 按"注册调用点"那条路注定找不到。

**手法（这轮验证有效）**：这个类族的 native 都通过**按字段名**读写实例字段，而字段名字符串
（`"handle"`=0x6e7974、`"flags"`=0x6e796c）是**真实被引用的** ⇒ 在 `.text` 里找 `push 0x6e7974` 的站点，
按地址聚类就得到整族的函数边界：

```
"handle" 引用: 0x40434e / 0x40443d / 0x404525 / 0x404538
  -> 0x4042c5..0x404413 (117 条) = FindFile.first(String;I)String   ★ 新找回
  -> 0x404424..                   = FindFile.close()V                （已在 §10.58 落地）
```
`first` 的形状也读通了：`unwrap` → 把 Java 串经 `0x567d90` 转 C 缓冲（`0x66659d` 取长）→
**`0x560a70` = 引擎的 find-first，返回句柄** → `按名写 this."handle"` → 再按名写 `"flags"` → 拼出结果 Java String。

**产物**：`out_native_registry_overlay.json`（v3 漏掉的地址放这里，带"怎么找到的"）。
`next()String` **仍未定位**：它不按名字读 `handle`（多半走 `0x643bf0(this, [0x8dcb2c])` 的索引路径），
下一轮用同法换判据（找引用 `0x8dcb2c` 且在 `0x4044xx` 附近的函数）。

★ 方法论：**"没有引用"不等于"没注册"** —— 先去查这个类族的**字段名/属性名字符串**有没有被引用，
有就按它们的引用聚类，函数边界自己就浮出来了。

#### 10.60 FindFile 全族收口：`first`=0x4042c5 / `next`=0x404424 / `close`=0x404510，目录枚举模型 **6/6**

**地址全部落地**（§10.59 用字段名聚类找回 `first`；`next` 这轮用**调用目标**判据定位）：
```
0x560a70 = 引擎 find-first    调用点: 0x404342(first) / 0x652020 / 0x658219 / 0x658262
0x560310 = 引擎 find-next     调用点: 0x4043e6(first 内部) / 0x40447b  ← 落在 0x404424 这个函数里
0x560390 = 引擎 close         调用点: 0x40454b(close) / 0x65218e / 0x65821f / 0x65841b
=> first = 0x4042c5 (117 条, 止于 0x404413)
   next  = 0x404424 : 按名读 "handle"(0x6434f0) 与 "flags"，handle<0 返回 null，
                       循环 0x560310 跳过 "."/".."，同一条 flag 规则，命中就拼 String
   close = 0x404510 (§10.58)
```
★ **"函数在哪儿"的第三条判据**：`next` 既不引用类名、也不在 `first` 的地址邻域猜得准，
但它**必须调用引擎的 find-next** ⇒ 扫 `.text` 里 `call 0x560310` 的站点，落在哪个函数里就是它。
（同法：任何"只调一个引擎例程"的包装型 native，用**引擎例程的调用点反查**最快。）

**flag 语义（真 dump 的静态值，不是猜的）**：`FILES_DIRS=0 / FILES_ONLY=1 / DIRS_ONLY=2`，
`first` 的过滤分支读出来是：
```
文件 : flags <= 1 才收  (FILES_DIRS / FILES_ONLY)
目录 : flags != 1 才收  (FILES_DIRS / DIRS_ONLY)
```
`.`（0x6e7968）与 `..`（0x6e7964）**永远跳过**。

**模型**：`tufa_vm.c` 里加 CRT 枚举（`_findfirst/_findnext/_findclose`，`<io.h>`），
pattern 按 `--file-root` 解析（与 File 层同一套），每个实例一个槽（`NFF=6`，满了回收最旧），
`handle`/`flags` 按真机的**字段名**写回，`close` 照旧写 −1。

**自检（双跑对拍，`probe/spec_findfile.json`）6/6 ✓**
```
first("vehicles/*",DIRS_ONLY=2)  -> 'classes'            real 'classes'
first("vehicles/*",FILES_ONLY=1) -> 'classes.zip'        real 'classes.zip'
first("vehicles/*",FILES_DIRS=0) -> 'classes'            real 'classes'
first("no_such_dir/*",1)         -> null                 real null
first("save/career/*.sav",1)     -> '001.sav'            real '001.sav'   ← 真存档目录命中
next()  (接上 DIRS_ONLY)          -> 'Fantasy_Corus_2005' real 'Fantasy_Corus_2005'
```
连**枚举顺序**都与引擎一致（两侧都走 Win32 find ⇒ 同序），说明不需要在模型里排序。

★ 踩到的模型 bug（第一版 5/6）：`next` 用了 `do{...}while(_findnext()==0)`，
**先测当前项再前进** ⇒ 把 `first` 已经返回的那一项又返回一次（'classes' 两次）。
真机是"先 find-next，再测"⇒ 改成 `ff_scan(t,s,advance)` 显式区分；
这类"状态机式相邻调用"的错误只有**同一实例的连续调用**探针能抓到（单调用永远对）。

#### 10.61 `0x7439b0` 的真身份：**每 kind 的字节偏移表**（Viewport 四个 getter 反证）

`java.gfx.Viewport` 的四个矩形 getter 是同一个模板（`getWidth`=0x40bfd0 / `getHeight`=0x40c030 /
`getTop`=0x40c090 / `getLeft`=0x40c0f0）：

```
n = vm_get_int_field(this)                     ; native handle
if (!n) return 0.0
p = [n+8]                                      ; 引擎视觉对象
if (!p) return 0.0
if (0x55a230(p, 0x80000001, 0) < 0) return 0.0 ; 引擎就绪/刷新检查，负 = 不可用
base = p + [0x7439b0 + [p+0x24]*4]             ; ★ 表值当"字节偏移"加到 p 上
if (!base) return 0.0
return *(f32*)(base + 0x18 / 0x1c / 0x14 / 0x10)
```
⇒ 四个读数其实是**同一个矩形结构**的四个 int 槽：
```
base+0x10 = left   base+0x14 = top   base+0x18 = width   base+0x1c = height
```
★ 所以 §10.52 记的 `0x7439b0` 那 22 个值 `[0,152,152,152,100×6,0,0,100,100,100,0,0,0,100,100,0,100]`
**不是枚举、也不是尺寸**，而是 **"这类东西的矩形在对象里的字节偏移"**（0/100/152 三档）——
这也解释了为什么实机值和镜像值一致（纯静态常量表）。

**决定不进模型**（按 §10.57 的范围结论）：这些读数来自引擎对象内存 `p`（我们沙箱里不存在，
沙箱侧只能是 0），而重编译产物里 native 仍是原版、`p` 仍由原版引擎写 ⇒ 模型返回 0 与
"真机原版在沙箱里"等价。**登记为 engine-state-getter 族**，不虚报 implemented。
★ 判据：**函数体第一个动作是 `vm_get_int_field(this)` 然后解引用引擎指针** ⇒ 全部落这一族，
不需要逐条读；这条判据可以把 219 条 backlog 里很大一批一次性分出去。

#### 10.62 全引导闸门（本轮结尾）：`java.game.Init.<init>` **93,159,836 步 errs=0**

```
UNI_DRIVER_BUDGET=200000000 UNI_DRIVERS="java.util.Config.init,java.util.Config.load,java.game.Init.<init>"   ./vm/tufa_vm.exe --cp extracted --index vm/class_index.txt --batch --dump --file-root C:/Games/LASR   < vm/clinit_names.txt
=> [driver] java.util.Config.init   steps=1968     errs=0
   [driver] java.util.Config.load   steps=714      errs=0
   [driver] java.game.Init.<init>   steps=93159836 errs=0      ← 本轮（上轮 88,875,004）
   [batch done] steps=93556645 errors=0 native_stubs=360 native_impl=12154625 oob_locals=0
```
★ 步数上去了（88.87M → 93.16M）**不是变慢**，是 FindFile 枚举真在工作了：Init 走
`Gamelogic` 扫 `save/career/*.sav` 现在真能列出 `001.sav` 之类的真存档 ⇒ Java 侧多走了真实分支，
而且 **errs 仍是 0** ⇒ 这条实现被真实启动路径覆盖到了。

★ 踩坑（记下来省下次）：`--clinit X` 会**在 batch 之前 return**，所以
`--clinit` + `UNI_DRIVERS` 同时给 ⇒ driver 根本不跑（表面上只有 572 步就"[done]"）。
驱动只在 **stdin 喂类名列表的 `--batch`** 分支里跑；driver 预算用 **`UNI_DRIVER_BUDGET`**（默认 20M）。

#### 10.63 ★ backlog 分流器：219 条里 **182 条是 engine-state 读**（新工具 `tools/native_triage.py`）

先把 219 条逐条反汇编，再按"函数体碰了什么"分桶（判据写进工具里）：
```
PURE         4    无引擎调用、无绝对全局、无虚派发  ⇒ 可精确建模 + 可对拍
ENGINE_READ 182   读引擎全局 / 走 [0x8dcb2c] 取 native 句柄 / 虚派发  ⇒ 真值在引擎内存
ENGINE_CALL   9   只调用引擎代码（按参数），不读句柄
NO_ADDR      24   注册表里没有地址（需 §10.59/10.60 的恢复手法）
产品：out_native_triage.json（每条带 fn / ins 数 / 命中的引擎调用）
```
★ 关键结论：**剩下的原生层 84% 不属于"重编译产物功能一致"的验收面**——
它们的真值在引擎对象里（沙箱没有），而产物里 native 仍是原版、引擎仍由原版写。
⇒ 之后不再按 backlog 顺序硬啃，**只做 PURE 那 4 条 + 顺手能恢复地址又属于 PURE 的**。

★ 分桶踩过的两个坑（都写进工具注释了）：
1. 一开始把 `mov eax, dword ptr [eax + n]` 当"解引用引擎指针"的判据 —— **VM 读自己的对象字段也是这条指令**
   ⇒ 几乎所有 native 都被误判成 ENGINE_READ。只有"引擎调用 / 绝对全局 / 虚派发 / 取句柄"才算。
2. 只看 `call <引擎地址>` 不够：`MetaServer.*` 全走 `[0x75f218]` 单例 + `[vtable+0x38]`
   ⇒ **虚派发也是引擎调用**，必须单独算。

**本轮落地的 4 条 PURE（implemented 151→155，backlog 219→215）**
```
java.io.Input.lastKey()I          = 0x566600  `xor eax,eax; ret`        -> 恒 0（探针 1/1 ✓）
java.io.Input.flushHotkeys()V     = 0x404c40  扫静态热键表 0x75d3f8..0x75db10(步长 0x1c)，
                                              [e-0x18]!=0 处把 [e] 清零 —— 表是引擎的，沙箱 no-op
java.io.Input.deleteHotkey(Hotkey)V = 0x404a30 同一张表按身份查槽后清槽 —— 同上，no-op
java.gui.Object.create()V         = 0x4da7a0  裸 `ret`                  -> no-op
```
`Input.*` 三条在容器里是 **static**（flags 265 = public|native|static），探针不用造接收者。

#### 10.64 ★★ 原生注册表 **直接读注册调用点**：1944 条，NO_ADDR 24→1（新工具 `tools/native_reg_sites.py`）

之前找 native 地址是"从类名字符串的引用倒推"，可类是**内联注册**的，字段/方法名字符串只在这一处被 push：

```
push <impl>            ; 68 xx xx xx xx
push <sig string>
push <name string>
push <class string>
mov  ecx, <class/sym>  ; 通常是 `mov ecx, esi`（2 字节）—— 所以"严格倒着退 4 次 push"会漏
call 0x652910          ; register(class, name, sig, impl)
```
⇒ 扫 `.text` 里 `call 0x652910` 的站点（本轮 **1944** 条），在调用点前 48 字节窗口里按顺序取
**最后 4 个 `push imm32`**，读它们指向的字符串，校验 `sig` 以 `(` 开头、`class` 含 `.`、
`impl` 落在 `.text`（0x401000..0x6E5809 —— **上限是全段，不是引擎段**：VM 运行时在 0x64xxxx+，
第一版把上限写成 0x4EA000 直接把 `Thread/Object/String` 那批全滤掉了，651 条）。

**结果**：`out_native_reg_sites.json` —— 1944 条 (class,name,sig)→fn，其中 **1515 条命中容器声明的 native**；
分桶器的 `NO_ADDR` **24 → 1**（仅剩 `GameRef.create(GameRef;GameType;String;String)`）。

★ 方法论更新（比 §10.59/10.60 更根本）：**"找 native 地址"先找注册调用点，别从字符串引用猜**。
0x652910 就是注册函数；`push` 四连是固定形状，窗口式取最后 4 个 push 比"严格回退"稳。

**本轮落地**（implemented 155→157，backlog 215→213）
```
Object.notify()V    = 0x662760  unwrap 后调 VM 监视器 0x656650 -> 单线程沙箱无等待者，no-op 即精确
Object.notifyAll()V = 0x662780  同上，调 0x656710              -> no-op
```
`Thread.getCurrentThread()Ljava.lang.Thread;` = 0x661a10（4 条）**不进模型**：
`mov eax,[esp+4]; mov ecx,[eax+0xc]; mov eax,[ecx+0x1c]` —— 读的是**VM 上下文里的当前线程**，
沙箱没有这个上下文。★ 注意这暴露了分桶器的一个盲点：**PURE 桶 ≠ 一定能建模**，
"读 VM 自己的执行上下文"同样没有沙箱等价物 —— 落地前必须读一眼函数体。

闸门：静态 `classes 1058/0 bad`、`fields 5403/0 bad`；分桶 PURE 1 / ENGINE_READ 202 / ENGINE_CALL 9 / NO_ADDR 1。

#### 10.65 ★ 地址层交叉验证：两套独立扫描器 **1848 条全一致、0 冲突**（新工具 `tools/registry_crosscheck.py`）

```
v3 (native_registry_v3.py)      1851 条，其中命中容器声明 1433
sites (native_reg_sites.py)     1944 条，其中命中容器声明 1515
union                           1947 条，命中 1517 / 2660
  AGREE        1848      ← 两条独立路径读到同一个 impl 地址
  MISMATCH        0      ← 一次都没有
  v3-only         3
  sites-only     96
```
⇒ **我们一直在用的那 1848 个地址是可信的**（两个互不依赖的解析器——一个按"调用点+连续实参段"、
一个按"最后 4 个 push"——给出同一答案）。产物 `out_reg_crosscheck.json`。

`sites-only` 的 96 条不是噪声，是 v3 漏掉的**大件**：`Chassis.configureType(String)V`=0x4558f0、
`Part.configureType(String)V`=0x4997c0、`Part/Chassis.createTypeBegin()V`=0x4a6910/0x46dfb0、
`Replay.create_native/delete_native`=0x42ab70/0x42ac20、`GfxEngine.destroy()V`=0x40a5f0…
`v3-only` 的 3 条**都复核过是真函数**（`File.write`=0x402430 SEH+unwrap、`MetaServer.sendMessage`=0x407380
四参 unwrap、`Sound.j()I`=0x40ed00 `push 0`）⇒ 两边各有漏，union 才是完整的。

**覆盖率（以"语料真调用"为分母）**：
```
declared                     2660
registered (union)           1947
declared & registered        1517
declared, NOT registered     1143        ← 死声明（既没注册也很少被调用）
called (declared)             550
called & registered           536        (精确类名匹配)
  ... (name,sig) 回退后        549        ← 只剩 1 条
STILL MISSING  GameRef.create(GameRef;GameType;String;String)GameRef   （语料 1 个调用点）
```
★★ **坑（差点把 13 条当"地址丢了"）**：native 是**按声明基类注册**的 —— `java.net.MetaServer` 的
12 条 native（getUserID/getRoomParameterI/setRoomParameterI…）在镜像里注册的类名是
**`java.net.HardServer`**（0x6e8944），因为它们是基类方法、由子类接收者调用。
⇒ 任何"declared ∩ registered"的集合运算**必须再回退一次 (方法名, 签名) 匹配**，否则会报 13 条假缺失。
（这与 `native_backlog.py` 里 `by_name` 回退是同一条规则。）

★ 由此得到一个干净的结论：**游戏真正调用的 550 条 native 里，549 条在镜像里有注册点**；
剩下那 1 条（4 参 `GameRef.create`）语料只有 1 个调用点，是唯一未定位的地址。

#### 10.66 ★ native 层的**范围结算**：2660 条声明 → 只有 550 条是真工作面（新工具 `tools/native_scope_note.py`）

把三份集合摆在一起（产物 `out_native_scope_note.json`）：

```
容器声明 native                2660
镜像里的注册点 (union)         1947
  ├─ 其中容器也声明的           1517
  └─ 容器**没声明**的           363   ← engine-internal：C++ 自己调的，不该进我们的 backlog
声明了但既没注册也没调用       1086   ← 死声明
语料真调用的 (declared)         550
  └─ 有注册点的                 549   （仅 GameRef.create 4 参那条缺）
```
★★ **死声明 1086 条 = 0 调用点（1086/1086，全语料零调用）** —— 这不是猜的，是拿
`out_native_calls.json` 逐条对出来的。样例：`GameMode.repair()V` / `restart()V` /
`letMeWin()V` / `refreshResults(Window)V`、`Bot.setLastVehicle(I)V`；它们确实是
`M … 4294967295` = native，但：
- 镜像里没有注册点，
- Java 字节码里**一个调用点都没有**（`repair` 在 158 个类的 src 里 0 次 CALL）
⇒ 唯一的调用者是引擎 C++ 自己（走它自己的对象 vtable，不经 VM 的 native 分派）⇒ **VM 层不可达**。

★ **engine-internal 那一桶的判据要"查声明"而不是查名字**：`Part.getSlotIndex(I)I`(=0x498450)、
`Chassis.getLongitudinalG()F`(=0x4846b0)、`getWheelBeta(I)F`、`setAckermann(F)V`、`getFFBactvalues()[F`…
**在 `out_tasm/.../Part.class.tasm` 里根本没有对应 M 行**（实测 grep 为空）⇒ 引擎注册给 C++ 用的，
Java 侧永远不会 INVOKE 它们。⇒ 这 363 条**不该被算成"缺实现"**。

★ 顺带一个彩蛋级发现：注册表里有 `java.game.GameLogic.kismajomCheck([Ljava.lang.String;)I` = 0x404600
（`kismajom` = 匈牙利语"我的小猫咪"）—— 显然没人会调用它，归 engine-internal。

**结论（native 层的验收口径到此闭合）**：
| 桶 | 数量 | 是否需要我们实现 |
|---|---|---|
| 语料真调用 + 有注册点 | 549 | 已实现 157，其余按 §10.63 分桶（202 条 engine-state 读、9 条 engine-call、1 条 VM 上下文） |
| 语料真调用 + 无注册点 | 1 | 只需恢复地址（4 参 `GameRef.create`） |
| 声明但从不调用（死声明） | 1086 | 否（VM 不可达，已证 0 调用点） |
| 注册但未声明（engine-internal） | 363 | 否（C++ 自用，Java 侧无 INVOKE） |

#### 10.67 engine-internal 桶**逐条核实**：从"抽样 + 名字回退"升级到"逐键查 .tasm 声明"（`tools/tasm_decl_index.py` + `tools/scope_verify.py`）

先建**权威声明索引**：直接解析 2,224 个 `.tasm` 的 M 行（`M <flags> <nameIdx> <descIdx> <code> <rec> ; <name> <desc>`）：
```
classes 2,224   method declarations 16,422   其中 native 2,660
```
⇒ 与 `native_audit.scan()` 的 2,660 **完全一致**（两个独立口径互证）。

然后对"注册了但容器没声明"的每一条逐键查索引（产物 `out_scope_verify.json`）：
```
registered natives not in the container-native set   430
  NO_SUCH_METHOD  341   类在语料里，但没有这个方法                -> C++ 自用
  NO_SUCH_CLASS    82   类根本不在语料里                          -> 引擎侧独有的类
  DECLARED          7   方法在，但容器给它的是 **Java 代码**（非 native）-> 引擎白注册，Java 体才跑
```
`DECLARED` 的 7 条（值得单独看）：`java.game.Navigator.update()V`=0x4223e0、
`java.gui.Polygon.<init>()`=0x5a09c0、`MetaServer.getUserParameterI/I64/S/Type`(4 条 0x4082a0/0x408300/0x408380/0x407c90)、
`GroundRef.getNearestSpline(Vector3)I`=0x424ec0 —— 引擎替这些方法注册了 native，但容器里它们有字节码 ⇒
**VM 永远走 Java 体，native 是死重**。

★ 与上一节的 363 的差额（430 − 363 = 67）不是矛盾：363 是**按 (名字,签名) 回退**后的数字（把"基类注册、子类接收者调用"的
那些算成了已覆盖）；430 是**严格按键**。差额 67 = 注册时用的类名 ≠ 容器的声明类 —— 正是
§10.65 那条 `MetaServer → HardServer` 的机制的量化值。

★ 顺带解掉一个悬案：backlog 里的 `java.gfx.GfxEngine.get()I` 一直被算作"未注册"，其实它注册在
**`java.gfx.HardGfxFeature.get()I`**=0x40a500（NO_SUCH_CLASS 样例里那条）—— 又是基类注册。

**两个索引工具改名说明**：`out_tasm_decl.json` 的类名映射是 `java/classes/<pkg>` ⇒ **`java.<pkg>`**
（第一版剥成 `<pkg>` 导致 2,224 类全部对不上，出现 "NO_SUCH_CLASS 1947" 的荒谬结果 —— 类名映射错会让整张表静默失效，
先 `print` 两个键互看一眼再信数字）。

#### 10.68 `DECLARED` 那 7 条**定为死代码**（新工具 `tools/dead_native_check.py`）

判据（比"读函数体"更硬）：函数指针的引用只有两种形态 —— 注册点那条 `push imm32 impl`，
和真正的 `call rel32 impl`。数一遍：

```
0x004223e0 Navigator.update        call=0 push=1  DEAD
0x005a09c0 gui.Polygon.<init>      call=0 push=1  DEAD
0x004082a0 MetaServer.getUserParameterI      call=0 push=1  DEAD
0x00408300 MetaServer.getUserParameterI64    call=0 push=1  DEAD
0x00408380 MetaServer.getUserParameterS      call=0 push=1  DEAD
0x00407c90 MetaServer.getUserParameterType   call=0 push=1  DEAD
0x00424ec0 GroundRef.getNearestSpline        call=0 push=1  DEAD
```
⇒ **push=1 就是注册本身、call=0 就是没人直接调**。这 7 条既不会被引擎 C++ 直接调用，
容器又把方法标成"有字节码"（VM 走 Java 体）⇒ **注册了但永远不执行** ⇒ 无语义风险，不进验收面。

★ 为什么这条判据只对这 7 条有效、对别的 native 无效：正常 native 的调用者是 **VM 的 native 分派**
（查注册表后间接调），所以 `call=0` 是常态；只有当"容器说它有 Java 代码 / 根本没有这个方法"时，
`call=0` 才等价于"不可达"。

★ 顺带确认了一个规模事实：`Navigator.update` 的 Java 体是**真逻辑**（`setScale()` + GPS 模式下的
`carOri/pcar` 更新），而那个 native 也实现了同族字段读写（0x4223e0 里 `0x643cc0` 按名读 float 三次）
⇒ 这是**同一功能的两个实现**（引擎留了旧的 native、容器里放的是后来的 Java 版），原版跑的是 Java 版。

#### 10.69 ★ `Matrix.set` 的数学**从遗留项里删掉**：它 Java 层 0 调用点（顺手给一个"是否值得实现"的判据）

`java.lang.Matrix` 的全部成员（`out_tasm/java/classes/lang/Matrix.class.tasm`）：
```
<init>()V          code=0      (调用 newNative)
newNative()V       native      = 0x405530  (64 B 清零)
finalize()V        native      = 0x40fd80
<init>(Vector3;Vector3;Ypr)V   code=1
set(Vector3;Vector3;Ypr)Matrix native = 0x405590
```
**没有 F 行（零 Java 字段）、没有任何 getter**。语料调用点：
```
Matrix.newNative  2 处   (来自它自己的 <init>)
Matrix.set        0 处   ← 全语料一次都没调用
```
⇒ 0x405590 的 T*R*S 组合链（`567e00 / 562900 / 562750 / 563520`）**从 Java 侧不可达**；
也没有任何东西能"读回"这个矩阵（无字段、无 getter），Java 侧能看到的只是一个不透明的 native 句柄，
而所有矩阵消费者（`setMatrix` 之类）都是别的类的 native、在沙箱里本来就是桩。
⇒ **既不可达又不可观测** ⇒ 模型保持"清零句柄"就是等价行为，**不转录那 90 条指令**。

★ 这条给了一个通用判据（以后先问再动手）：
**"这个 native 的结果有没有 Java 侧的读者？"** ——
`目标类没有字段、没有 getter` 且 `没有 Java 代码把它交给别的 Java 代码` ⇒ 它的数值不可观测，
实现得再准也验证不了，也不影响重编译产物。优先级应该排最后，而不是排在最难的那一档。

★ 附带纠正一条旧记录：`docs/65 §9` 与 `M4` 待办里把 `Matrix.set 数学（567e00/562900/562750/563520）`
列为"硬骨头待实现"——按本条**它是死项，删除**。同族已实现的 `mtx_from_ypr`（0x562900）与
`setFromAxisAngle`（0x562b20）仍然有效（被 `Ypr.toVector3` / `Vector3.rotate` 用到，均已探针对拍）。

#### 10.70 剩下三项按同一判据销掉：它们全是**诊断管线**，没有 Java 侧读者

用 §10.69 的判据（"这个结果有没有 Java 侧读者"）过一遍最后三项：

**① 行号表 blob —— 只有错误打印器读它**
`frame+0xa8` 非空时取 `[+4]`（行号表 ref），用 `pc − 记录基址` 查 (offset,line) 对表
（`655faf0` 解析），然后拼 `"%s
%s:%s (line:%d)"`；**没有行号表就退化成 `(offset:0x%04x)`**。
也就是说它的唯一消费者是**错误消息格式化**。而语料侧：
```
grep -rn "throw |catch (|try {" out_pseudo/java/classes/  ->  0 处
grep -n "throw|exception"  vm/tufa_vm.c                  ->  0 处
```
⇒ 这份游戏**一句 throw/catch 都没有**（没有 Java 异常处理），VM 也不需要异常支持
⇒ 行号表永远不会被用于程序行为，只可能出现在诊断输出里。**不进验收面。**

**② halt 位 0xfa —— 异常/中止类位，语料里没有置位路径**
run-loop 的终止条件是 `while ((flags & 0xfa) == 0)`，掩码 = `0x02/0x08/0x10/0x20/0x80`
（原注释即"异常/中止类"）。既然没有异常、release 构建也没有 stop/breakpoint 通道，
就没有置位者。**经验证据**：`Init.<init>` 93,159,836 步 + `vm_ctor_sweep` 803/803 + 合并回归 110/112
全程没有一次需要它。**不进验收面。**

**③ `FUN_0041d270` —— native 查找里的"版本/组 id"校验**
它出现在"按 hash 找到条目后，再比对签名串与版本/组 id"那一步。我们的模型**不加载外来的 native**：
地址层已由两套扫描器互证（§10.65，1848 条 0 冲突）、可达面 550 条里 549 条有注册点。
⇒ 这个校验的存在意义（拒绝不匹配的 native）在我们这里不可能触发。**不进验收面。**

**④ 108 RAWREC —— 早就不是功能缺口**
`tufa_lift` 结构化失败退化成 RAW 记录，`tufa_src` 能逐字节重组、`tufa_vm` 能直接执行
（实证：`ChallengeOffer.getOpponentIndex` 169,616 步 / 0 错 / 返回 59）。属**呈现层**缺口。

★ 结论：`docs/65 §9` 那张"仍未解"的表里，**行为相关项已全部清零**，剩的都是诊断/呈现管线，
且每一条都有"没有读者 / 没有置位者 / 不可能触发"的证据。

#### 10.71 ★★ 实机对照（第一轮，用户操作 + 只读挂接 + 文件对拍）：三条预测全部命中

对照设计（全程只读、不注入、不暂停）：先在用户操作前把 `C:/Games/LASR/save` 整体快照到
`scratch/live_baseline/save_before`（10 个文件 + md5），用户做三件事，然后比对文件与内存。

| 预测（来自反编译） | 实机结果 | 判定 |
|---|---|---|
| 生涯读取菜单只显示 **1** 个条目（`all("save/career/","*.sav",FILES_ONLY)`；目录里只有 `001.sav` 匹配，另两个是 `.bak`） | 用户报告"只显示一个条目" | ✓ |
| 改米制↔英制只动 **1 个字节**（`save/game/options` 的 `+0xcc`：1=Kph、0=Mph） | `+0x00cc 01→00`，整文件只有这一处变化 | ✓ |
| 中途退出比赛仍会写存档（`GST_INGAME` 退出钩子 `challenge.apply→save`），**大小不变**（8879 B，只改内容） | `career/001.sav 8879 → 8879`，内容变化 | ✓ |

**options 文件的真实结构（顺带解开）**：
```
+0x00  "SDAT"
+0x04  u32 0x00030100            版本
+0x08  u32 文件大小 (360)
+0x0c  u32 0xFEDCBA98            SAVEFILEID
+0x10  u32 featureCount = 37     ← GfxEngine.getFeatureCount()，每项 4 字节（共 148）
+0xa8  配置字段区 192 字节
```
⇒ **这个文件是"设备相关"的**：前面 148 字节是硬件特性表（37 项来自真实 GPU）。
沙箱里 `GfxEngine.getFeatureCount()` 拿到 `null.fieldref`（没有设备）⇒ 我们只能写 0 项，
所以**整体字节无法逐字节复现**，但**字段区长度正好也是 192 字节、与游戏一致**。

**新落地：`File.write(String)I` = 0x402430**（之前是桩 ⇒ `Config.save()` 只能写出 212 B 的残片）。
实现后我们的 VM 能完整跑完 `Config.save()`（1565 步），产出结构与真机同形。

**双向交叉验证（最硬的一条）**：把游戏自己写的 options 交给我们的 `Config.load`：
```
tufa_vm: java.util.Config.metricSystem = 0     ← 用户已切成 Mph
游戏文件 +0xcc                        = 00     ← 同一字段
（基线快照里该字节 = 01 = Kph）
```
⇒ 我们反编译出的 `Config.load` 读的就是游戏写的那个字段，语义方向也一致（Kph=1 / Mph=0）。

**仍差的部分**：字段区 192 B 里逐字节比较有 87 字节不同（等长、非移位），是**值**的差异
（字符串内容/浮点），下轮定位。另：`save/controls/active_control_set1` 被重写（设备名
`R9 Base` → `Multi-fu…`）—— 那是启动时重新枚举设备的正常行为，与本轮无关。

#### 10.72 实机对照第二轮：单位字节**双向**都跟得住；但 options 的**逐字节复现不可能**（设备特性块）

用户把单位从 Mph 切回 Kph，同一位置再验一次：
```
save/game/options +0xcc :  01 (Kph) -> 00 (Mph) -> 01 (Kph)
tufa_vm Config.load      :   1     ->  0     ->  1        ← 与文件同步
```
⇒ **Kph=1 / Mph=0 这个字段映射，两个方向都在实机上成立**（不是单次巧合）。

★ 但本轮也证伪了我自己的一个计划：**"让 VM 跑 Config.save 再和游戏文件逐字节比"是行不通的**。
原因是文件**开头就是设备特性块**：
```
+0x10  featureCount = 37   （来自真实 GPU，每项 4 字节 = 148 字节）
+0xa8  Config 字段区 192 字节
```
`GfxEngine.getFeatureCount()` 在沙箱里没有设备（`null.fieldref [java.gfx.GfxFeature]`）⇒ 写 0 项；
更糟的是 **`Config.load` 也会因此错位**（按 0 项跳过 ⇒ 把 148 字节特性块当字段读），
实测就是 `Config.save` 写出的字段区开头变成 `32, 1, 3, 3`（正是特性块里的值）。

★ 第二个坑（harness 层面，值得记）：**同一个 `Config.load` 在两种跑法里结果不同** ——
* probe 里（`--clinit java.test.SaveCfg` 直接调 load→save）：**错位**，字段区是特性块垃圾；
* batch 里（`UNI_DRIVERS=Config.init,Config.load`）：**对齐正确**（metricSystem 与文件一致两次）。
差别是 batch 先跑了 `Config.init`。⇒ 任何涉及 Config 文件布局的实验**必须在 batch 跑法里做**，
不要在探针里跑，否则会把"错位"当成"字段顺序不同"去查（我这轮就差点这么干）。

**结论**：options 文件的字节级等价**不在沙箱可达范围**（设备相关），但它承载的**字段语义**
已经被实机双向验证；`File.write(String)`=0x402430 仍然是有价值的落地（`Config.save()` 从"只能写
212 B 残片 + native 桩"变成"完整执行"）。

#### 10.73 实机对照第三轮：冲线一局（第 1 名、2 圈）→ 存档解码，**两条"原版如此"的规则被实机坐实**

用 `tools/lasr_save.py` 解真实存档（格式是我们从 `Gamelogic.save`/`RaceChronicle` 反出来的）：

```
career/001.sav   lastSaveTime 2026.10.18. 20:08   nickname Niko
   winSum 0→0   raceSum 0→2   retries 0→0   aiLevelMul 0.7868→0.7403
   prestige 0→100   trialProgress 0→8   chronicles 15/15（环形，覆盖最后两条）
最后两条 chronicle：laps=2 ✓  trackID=0xC0030A00（真 id）  TOD=-1  opstatus=1  oprank=56/58
                   splineLeft 0 / 1（两条）  won=0  pinks=0
```
★ **`won=0` 不是"没赢"**：`won` 只在一个地方被置 1 —— `GmRace.java:1041`，
而那整块的门是 `if (showIngameMenu && challenge != null && !isFinishCalled())`
即**"比赛中途退出"**那条路径。**正常冲线不会把 won 置 1**。⇒ 我原先"第 1 名 ⇒ won=1"的推断是错的，
已按代码订正。

★ **`winSum` 全语料只有一处自增**：`GmRace.java:1039`，同样在那个"中途退出且当时领先"块里
（`if (local1) winSum++`，且 `!isQuickRace`）。⇒ **干净冲线不加 winSum** —— 实机 winSum 保持 0 完全吻合。
`raceSum` 也只有一处自增（1035，同一块）⇒ 这次 +2 说明那块跑了两次（他先中途退出一次、之后又从比赛里退出一次）。
这两条都是**原版行为怪癖**，不是我们的模型缺陷，值得写进 docs/48 的"原版如此"清单。

★ 顺带：`prestige 0→100`、`trialProgress 0→8` 的变化发生在**正常冲线**路径上（与上面那个块无关），
说明结算奖励走的是 `handleRaceTime`/`finish` 那条链 —— 与 docs/48 的"结算另走一条"一致。

★ 存档读取端到端可用：真实 8,879 B 文件（`consumed 8855` + 12 B 尾）解析无错，
cars 10 / chronicles 15 / prestigeValues 全部解出 ⇒ **格式层反编译结果在实机产物上再次成立**。

#### 10.73 实机对照第三轮：**订正** —— 结算画面不写盘，"回到车库"那一刻才写；`won/winSum/raceSum` 的规则

（本节第一版结论作废，原因见末尾"错在哪"。）

用户冲线后停留在结算画面时，`save/career/001.sav` 的 `lastSaveTime` 是 20:08（那是**中途退出**那次写的），
点"Back to garage"之后变成 20:18。两次快照对比：

| 字段 | 20:08（中途退出写的） | 20:18（真正冲线后写的） |
|---|---|---|
| `winSum` | 0 | **1** |
| `raceSum` | 2 | **3** |
| `prestige` | 100 | **251**（+151 奖励）|
| 新 chronicle `won` | 0 | **1** |
| `crashes / rescue / repair / pushes` | 0 / 0 / 0 / 0 | **4 / 1 / 1 / 1** |
| `laps` | 2 | 2（一致）|
| `bestLapTime` | 0.0 | 0.0（仍是 0，待查）|

⇒ **计时/计分/`won` 是在"离开比赛会话"那一刻才写入并落盘的**，停留在结算画面时游戏还没存。
这条与 `GmRace.java:1033` 那块的门完全对上：`if (showIngameMenu && challenge != null && !isFinishCalled())`
—— 它管的是**"从结算/菜单离开这场会话"**（中途退出和跑完离开都走这里），不是"只有中途退出"。
`isFinishCalled()` 指的是"结算流程已跑过"，与"是否冲线"不是一回事。

★ **错在哪（第一版）**：我拿 20:08 那份文件当"冲线后的存档"分析，于是得出"正常冲线不加 winSum、
`won` 只在退出时置 1"的错误结论。真正的判据是**用户点完 Back to garage 之后再抓一次文件**——
时间戳（20:08 vs 20:18）就是最直接的证据。**以后对照实机写盘，先比 `lastSaveTime` 再解内容。**

★ 新拿到的计数语义（实机）：一次"两圈、第 1 名、有碰撞"的比赛 ⇒ `crashes=4`、`rescue=1`、`repair=1`、
`pushes=1`、`won=1`、`prestige += 151`、`raceSum+1`、`winSum+1`。`bestLapTime` 仍为 0（下轮查）。

#### 10.74 `RaceChronicle.bestLapTime` 在单人里**永远为 0**：没人给它赋值（实机 0.0 是对的）

实机两圈第 1 名的存档里这一格是 `0.0`，查代码定位：

* 唯一能把值写进**实例字段** `this.bestLapTime` 的地方是 `GmRace.setBestLapTime(F)V`（第 30 行）；
* 而 `setBestLapTime` **全语料只有一个调用者**：`Track.java:2173`，并且是在
  `MetaServer.getRoomParameterI(ROOM_PARAM_LEVEL_main, ROOM_PARAM_BestLapTime)` 的**联网房间参数**分支里
  ⇒ 单人跑再多圈也不会走到；
* 圈速实际落在 `racerStats[i].lastLapTime / lapStats[].lapTime`、`raceStats`、`recordStats`
  （`handleLapTime` 里 `this.recordStats.storeBestLap(...)`、`raceStats.storeBestLap(...)`），
  只喂 HUD（`setLastLap/setRecordBestLap/setRaceBestLap/setOwnBestLap`），**从不进存档**；
* chronicle 的 18 个字段由 `GmRace` 显式赋值（trackID/TOD/laps/myCar/myStage/opCar/opStage/opRank/
  pinks/opStatus/won/prestige/splineLeft/push/crashes/rescue/repair），**bestLapTime 不在其中**；
  只有 `GmRace:92` 那条 `if (this.bestLapTime > 0) recordStats.bestLapTime = this.bestLapTime`
  会用到它，而单人里它恒为 −1/0 ⇒ 条件永不成立。

⇒ **实机 0.0 = 原版行为**，不是我们读错偏移（其余 17 个字段全部与实机对得上）。
⇒ 对重制/编辑器有意义的一条：**"单圈最快"不在存档里**，只能从 HUD/记录层（`RacerStats`）拿；
存档里的 `bestLapTime` 只在联机回放房间参数时被填。

#### 10.75 本轮改动后的**全量回归**（method / ctor / 静态 / 合并探针）+ 最后一条地址的定性

本轮动过 `tufa_vm.c` 的三处（FindFile first/next、notify 家族、`File.write(String)`）之后重跑：

| 闸门 | 结果 |
|---|---|
| `vm_method_tests.py`（11 个方法级精确值） | **11 ok / 0 bad** |
| `vm_ctor_sweep.py`（全语料字面量 new 表达式） | **ok 803 / dup 581 / skip-nonliteral 278**（与历史基线 803 一致） |
| 静态面（`--batch --dump` + `vm_diff_real.py`） | **classes 1058 / 0 bad，fields 5403 / 0 bad** |
| 合并探针 `spec_regress.json`（112 例） | **110/112**（两个 BAD 恒为 `swscanf` 未匹配的栈垃圾） |
| 全引导 `Init.<init>` 驱动 | 93,159,836 步 **errs=0** |

**最后那条"缺地址"的定性**：`java.util.resource.GameRef.create(GameRef;GameType;String;String)`
容器里确实是 native（`M 257 87 86 4294967295`），但镜像里**只注册了另外两个重载**
（`…;I;String;String`=0x422d30、`…;IFFFFFF`=0x422fc0，site 0x4271ab/0x4271ca），**GameType 那个没有注册点**。
⇒ 这条调用一旦真的执行，引擎的 native 分派会找不到实现（原版会报 "native implementation missing" 而不是静默算错）。
原版从未因此在启动/比赛中出错 ⇒ **这个调用点实际不可达**（与 §10.67 的 430 条注册不可达同族）。
不再是"待恢复地址"，而是"**无实现且不可达**"。

★ 记录一下这轮的一个副作用：`setActiveCollision()V` 与 `gui.Object.create()V` **注册到同一个地址 0x4da7a0**
（一条裸 `ret`）—— 说明引擎里确实存在"故意注册成空实现"的槽位，这解释了为什么 §10.67 里
"类存在但没有这个方法"的桶能有 341 条那么大。

#### 10.76 ★★ 重编译产物 == 发行版 `classes.zip` **逐字节相同**（含 zip 容器本身）

新工具 `tools/repack_classes.py`：对发行版 `java/classes.zip` 的 **304 个条目**逐个
① 解 FLZD 外壳（游戏自己的 `flzd_unpack`）→ ② 用**游戏自己的 `flzd_pack`**（`LASR.exe+0x1D00`）
把同一个 TUFA 容器重新压回去 → ③ 与发行版条目逐字节比。再按原顺序、原 `ZipInfo` 重写一个 zip。

```
entries:            304
byte-identical:     304
differing:            0
rebuilt: scratch/classes_rebuilt.zip  1114216 B  md5=4174d4597055ea787e85e7d7d3bdb15f
shipped: C:/Games/LASR/java/classes.zip 1114216 B  md5=4174d4597055ea787e85e7d7d3bdb15f
whole-file identical: True
```
⇒ **我们去重打包出来的 classes.zip 与发行版连 md5 都一样**。这才是"重编译产物功能一致"最强的静态判据：
不是"字节等价的 class 文件"，而是**整个存档容器**都复现了。

★ 三个 zip 细节（差一个就不是 bit-identical，都是实测踩出来的）：
1. 每个条目的 `ZipInfo.date_time` 必须沿用发行版（2006-07-04 14:31:00）；
2. **中央目录的 `version made by` = 20**，而 **`version needed` = 10**（local header 里也是 10）——
   Python 默认两处都写 20，会出现 304 个单字节差异；
3. 条目必须是 `ZIP_STORED`（载荷已经是 FLZD 压缩过的，再 deflate 就不是原文件了）。

★ 因此"把产物装回游戏跑一遍"在**字节层面是恒等操作**（同一 md5 的文件），它只能证明
"引擎接受我们自己写出的 zip"，不能证明更多；真正的语义检验只能靠**行为核对**（见 §10.73/§10.74 的方法）。

#### 10.77 实机对照第四轮：**生涯挑战赛分支**与**快速比赛分支**各自验证到位

同一份存档、两次比赛、两类入口，规则全部对上（`GmRace.java:1033` 那条门的两个分支）：

| 入口 | 存档变化 | 说明 |
|---|---|---|
| **快速比赛**（赢、无撞车） | 只 `lastSaveTime` 更新；`winSum/raceSum/prestige` **一个没动**；**没有**新 chronicle | 命中 `if (!isQuickRace)` 两道门 ⇒ 不计入 |
| **生涯挑战赛**（赢、无撞车、2 圈） | `winSum 1→2`、`raceSum 3→4`、`prestige 251→301`；**新增**一条 chronicle：`won=1, crashes=0, rescue=0, repair=0, pushes=0, laps=2, oprank=55, splineLeft=0` | 与 `GmRace:1035/1039/1041/1043` 一一对应 |

另外用户目视确认了两条 HUD 规则：**逆行出 `WRONG WAY` 横幅**、**不撞车跑完一圈出 `CLEAN LAP` 消息**（docs/48 §计圈/干净圈）✓

★ 仍有一个小疑点（记下，不阻塞）：chronicle 里的 `prestige` 字段实测恒为 **0**，而同一次比赛的顶层
`Gamelogic.prestige` 确实涨了（+50）。`GmRace:1043` 明明调了 `raceChron.setPrestige(getPrestigeForRace(true))`。
两种可能：① 该 setter 写的不是我们按 18 字段顺序读到的第 16 格（字段序有一格错位，但其余 17 格全对不上这种可能较小）；
② `getPrestigeForRace()` 在"已结算"时返回 0（奖励记在别处）。留作下一个实机/静态可查的小项。

#### 10.78 chronicle 的 `prestige` 恒 0 查清了：`Challenge.getPrestigeForRace` **本身就是个 `return 0` 桩**

`out_tasm/java/classes/game/Challenge.class.tasm`：
```
M 1 27 26 3 2 ; getPrestigeForRace (I)I        ← code=3 字节
```
3 字节 = `LIT 0; RETURN` ⇒ **原版这个方法永远返回 0**（不是我们伪码重建丢了代码，字节数就是 3）。
所以 `GmRace:1043` 的 `raceChron.setPrestige(getPrestigeForRace(true))` 写进去的必然是 0 ——
**我们读到的 chronicle `prestige=0` 是对的**，字段序也没错。

**真正的 prestige 规则在 `Challenge.apply(I)V` 里**（写的是顶层 `Gamelogic.prestige`，而且是**替换**不是累加）：
```
if ((r % 2)==0 && r!=0) prestige = getOpponentPrestigeByStatus(-1) + 1;
last = races.lastElement();                        // 最近一条 chronicle
if (last != null && last.getOpRank() < 100) {
    st = last.getOpponentStatus();
    if (last.won == 1)  prestige = (st > 0) ? getOpponentPrestigeByStatus(st) + 1 : prestige;
    else                prestige = (st < 0) ? getOpponentPrestigeByStatus(st) + 1 : prestige;
}
```
⇒ 为什么实机两次 +151 / +50 不一样：它不是加固定奖励，而是**按对手状态/排名重算并替换**。
这条与"`getPrestigeForRace` 是桩"合起来解释了全部观测，不需要再实机。
