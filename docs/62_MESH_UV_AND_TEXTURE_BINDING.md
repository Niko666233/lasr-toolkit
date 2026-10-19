# 62 · 网格顶点 UV 与「网格 ↔ 贴图」绑定（B7 收口）

> 承接 `docs/53`（材质表 / 页模型）与 `docs/57`（零件装配链）。本轮解掉两个**独立**的绑定 bug，
> 两个都直接表现为「导入 Blender / Unity 后贴图不对」：
>
> | 症状 | 真凶 | 归属 |
> |---|---|---|
> | 自动分配的贴图不正确 | 材质表的**层序**被当成漫反射：旧流程取「第一条 `texture`」当 `map_Kd` | §1 |
> | 就算手动选对贴图也错乱（和 UV 对不上） | ① 导出器把「顶点**最后 8 字节**」当 UV；stride 44/60/64/68 上那 8 字节是**切线/副法线**。② **INVO 的 UV 是 D3D9 约定（v=0 在贴图顶行），导出 OBJ 必须翻 V**；不翻 = 整张贴图上下镜像 | §2 |
> | 车壳选的贴图不对（用户实测：chassis 该用 `stock_mask`） | 车壳图集**名字不可靠**：Tornado 的完整图集叫 `stock_mask.png`，`interior.png` 只有内饰 | §1.4 |
>
> 工具：`tools/invo_obj.py`（改）、`tools/tex_roles.py`（新）、`tools/mesh_qa.py`（新，UV 叠图 / 带贴图渲染核对）、
> `tools/rpak_assets.py`（改）、`remaster/tools/pack_unity.py`（改）、`remaster/tools/verify_unity_package.py`（+4 项断言）、
> `Scripts/Editor/LasrImportSelfTest.cs`（+UV 硬断言与 stride 样本）。
> 产物：`out_tex_roles.json`、重出的 764 OBJ / 665 MTL、Unity 包。
> 校验：`verify_unity_package.py` **23/23**、`validate.py` **80/80**、Unity 导入自检 **exit 0 / 0 ✗**。

---

## 0. 一句话结论

1. **B7 解决**：把 RPAK 目录记录**尾部的 `(group<<16|type, index)`** 提出来建**全量 id→名字表**，
   零件 `texture_id` / `mask_texture` / `shadow_texture` 的 433 条引用从 **12/433 → 347/347 全部解名**
   （见 `remaster/tools/verify_unity_package.py` 的 [G] 断言）。
2. **UV 读取偏移**：788 个顶点块里 **230 个的 UV 不在最后 8 字节**；正确布局是
   `pos(12) normal(12) 顶点色(u32) [4B] uv0(8) [uv1/切线/副法线…]`，即 **uv0 紧跟位置/法线/色之后**。
3. **另一个从没被发现的数据通路 bug**：`resolve_part_textures()` 返回的字典键少了 `assets/meshes/`
   前缀，于是 433 条 mask/shadow **只进了日志统计，一条都没写进 `material_map.json` /
   `mesh_textures.csv`**（§3）。

---

## 1. 主贴图：材质表的层序不是漫反射顺序

### 1.1 现象与旧做法

`docs/53 §1.2` 给的材质表里，一条材质是一串 `texture 0x<page><id>`，**层义未标**（`docs/53 §4` 留的未决项）。
旧流程（`rpak_assets.mtl_for` / `stage_mtl`）直接**按出现顺序**写 `.mtl`，第一条当 `map_Kd`。
实测第一条经常不是漫反射：

| 存档 :: 网格 | 第一条 `texture` | 真实主贴图 |
|---|---|---|
| `vehicles/Fantasy_Corus_2005.rpk :: FL_door` | `muffler_shadow.dds` | `stock_interior` |
| `vehicles/Fantasy_Corus_2005.rpk :: chassis_6` | `muffler_shadow.dds` | `stock_interior` |
| `maps/boulevard.rpk :: 2`（主赛道面） | `glow_yellow.dds` | （21 层混合，见 §4）|
| `particles/backfire_03` | `on.png` | `backfire_xx` |

### 1.2 层序的实测规律（车包）

车包的**车身件**材质是一条固定骨架，各层的 id 逐个存档不同、**位置相同**：

```
[可选 p2 的 mask/shadow/灯层…] , p0「基础层」, p0:0（空槽）, p0 dirt_*, p0 placeholder
```

`p0` 的基础层对 6 个存档解到车壳图集，另外 4 个解到**占位/图标/软阴影**：

| 车包 | 基础层 id | 解出的名字 | 存档里真正的车壳图集 |
|---|---|---|---|
| Corus / Hornet / NSR_Desert / Takura_Cyclone | 13 | `stock_interior`（Cyclone = `interior`）| 同左 ✓ |
| Fantasy_Quaddro / Raptor_ZX | 18 | `stock_interior` | 同左 ✓ |
| **Takura_Tornado** | 18 | **`item_icons`（size 0 的存根）** | `interior.png`（id 4）|
| **Phoenix_RS** | 18 | **`item_icons`（存根）** | `interior.png`（id 46）|
| **Phoenix_Trend** | 18 | **`softshadow`** | `interior.dds`（id 231）|
| **Fujin_MX** | 230 | `item_icons`（存根）| `stock_interior`（id 13）|

⇒ **材质表那条「基础层」id 不能当漫反射的唯一依据**（6/10 对，4/10 对到存根/无关图）。
以**图集名**为准：`stock_interior` / `interior[.png|.dds|.tga]`（10 个车包全部命中，1024²、≈1 MB DDS）。

> 顺带证伪一条旧笔记：`docs/58 §4` 写的「RPAK 目录里 13=glosspaint_0 / 14=paintjob_0 / 11=stock_interior」
> 是按**目录里的位置**读的，不是记录的 `index`。按尾字段读：**13=stock_interior、14=glosspaint_0、
> 32=paintjob_0** —— id 空间本身是闭合的。

### 1.4 车壳图集：名字不可靠 ⇒ 人工核实覆盖表

这一段由用户临时手动修改，待完善。

所有车的chassis的对应贴图都是stock_mask + stock_shadow +原厂车漆色，interior对应stock_interior或interior。其他配件的贴图和网格名基本一致，若有例外则需人工核实。

### 1.5 名字对上的**专用贴图**（另一种「自动分配不对」）

有些件的 UV 包围盒**正好铺满 [0,1]²** ⇒ 它们配的是**自己那张小贴图**，不是整车图集：

| 网格 | 真实贴图 | 证据 |
|---|---|---|
| `steering_wheel` | `steering_wheel_int.png`（128²） | UV 叠图：轮毂圆盘 + 圆环 + 辐条**严丝合缝**压在贴图美术上；叠 `stock_interior` 是一片乱 |
| `U_rim_ST` / `U_rim_TR` | `U_rim_*.png` | 同上（材质表里本来就是这一层） |
| Tornado `interior` 等 | `interior.png` | 名字 + bbox 铺满 |

规则（`tex_roles.dedicated_texture`，全库只命中 **12 条**）：
**UV bbox ≈ [0,1]² ∧ 存档里有一条「带图像扩展名」的非网格记录名字 = 网格名或 `<网格名>_int/_diff/_d`**。
★ 必须要求**带扩展名** —— 网格同级变体记录（`chassis_6` / `FL_door` / `F_bumper_2`）都不带扩展名，
放开就会把 `Fujin/Hornet chassis → chassis`、`Tornado FL_door → FL_door` 这种假阳性放进来。

```
角色 → 规则（对真名字做正则，名字来自记录流，不是猜的）
  none    placeholder|dummy|_new|all|none             ← 空槽
  icons   item_icons|*_grid          ← UI 图标图集，永远不是 3D 贴图
  shadow  *shadow*                   ← 与 docs/57 §0 的 shadowTexture 对应
  mask    *mask*                     ← 同上 maskTexture
  dirt    ^dirt|dirts_wrecks         ← configureType("dirt_texture …")
  gloss   ^gloss
  glow    glow|light_glow|spec_flare
  sky     skydome|cubemap|^far$
  paint   ^paintjob                  ← 涂装（运行时由 frontend.rpk 1280+ 段叠加，docs/53 §3）
  body    ^(stock_interior|interior|wrecked|chassis|normal|_misc_)
  rim     rim|chrome|^tyre|^wheel
  atlas   _comb$|^track_|asphalt|sidewalk|…
```

主贴图判定顺序：`body 层 → 存档车壳图集（archive-body）→ 第一条非辅助层 → 第一条`。
选出的 base、mask、shadow、dirt、gloss、glow、paintjobs 与**完整层表**都写进 `out_tex_roles.json`。

统计（1034 条材质）：`base` 来源 `archive-body` 277 / `body` 326 / `first-non-aux` 319 / `fallback-first` 27 / 无 85。

---

## 2. 顶点 UV：不是「最后 8 字节」

### 2.1 症状

`tools/invo_obj.py` 原注写着「uv = 顶点最后 8 字节，stride 36/60/64/68 都成立」。
实测**不成立**：stride 44/60/64/68 的最后 8 字节是**切线/副法线**（单位向量，|v|≈1）。
在 UV 空间里画出来就是**一整片半径 ≈1 的弧** —— 于是贴图「和 UV 对不上」。

```
Tornado chassis（stride 60, flags 0x604c1）逐字段实测：
  off  0/ 4/ 8  = 位置（±百米级）
  off 12/16/20  = 法线（贴到 ±1）
  off 24        = 打包色（u32；按 float 读会得到 NaN）
  off 28/32     = ★ UV（整块落在 [0.002,0.998]）
  off 36…59     = 切线 / 副法线（±1）  ← 旧版读的就是这里
```

### 2.2 逐 (stride, flags) 实测表（788 个 `kind=4` 块投票）

| stride | flags | UV 偏移 | 旧版（最后 8 字节）|
|---|---|---|---|
| 24 | 0x281 | 16 | 16 ✓ |
| 28 | 0x381 | 20 | 20 ✓ |
| 28 | 0x0c1 | **无 UV** | 20 ✗（读到法线）|
| 32 | 0x1c1 | 无 UV | 24 ✗ |
| 36 | 0x2c1 / 0x4c1 | 28 | 28 ✓ |
| 40 | 0x2c5/0x3c1/0x4c5/0x4e3/0x5c1 | 32 | 32 ✓ |
| 44 | 0x6c1 / 0xac1 | 28 | 36 ✗ |
| 44 | 0x4e5 | 36 | 36 ✓ |
| 48 | 0x4e9 | 40 | 40 ✓ |
| 52 | 0xec1 | 28 | 44 ✗ |
| 60 | 0x602c1/0x604c1/0x608c1 | 28 | 52 ✗ |
| 64 | 0x602c5/0x602e3/0x603c1/0x605c1/0x608c5 | 32 | 56 ✗ |
| 68 | 0x606c1 | 28 | 60 ✗ |

⇒ 只有 36 / 40 是碰巧对的，**230/788 个块**曾经读错。

### 2.3 新算法（`invo_obj.uv_offset`）

1. 采样整块，按偏好顺序 `(28,32,24,36,40,48,44,52,20,16,12,8,4,0)` 找**两个分量都落在 [0,1]** 的偏移
   —— 范围本身就是很强的判据（位置 ±百米、法线贴 ±1、色值按 float 读是 NaN）。
2. 不中再放宽到 `|v|<=8`（平铺 UV 的网格）。
3. 都不中 ⇒ **该块没有 UV**（不编造）。

★ **关键护栏**：法线占 12..23，所以**检测到法线时 UV 只能从 24 起**。
少了这条，一块 y 朝上的平地被读成 `(normal.y, normal.z)` —— 实测
`maps/boulevard/0008_ISCX.iscx`（`mirror_black`，stride 28 / flags 0xc1，425 万字节）就是这么错的。
法线检测 = 顶点 12..23 的模长是否 ≈1（>90% 采样命中）。

### 2.4 顺带修好的两件小事

* **kind=0 块 = 子网格自己的材质记录**（带真名字）。旧版丢掉它，OBJ 里组名是无意义的
  `submesh_0_stride44`、且整个文件只有一个 `usemtl`。现在：
  `o chassis` / `o uvegkulso`（匈牙利语「外玻璃」）/ `o torott_uveg`（「碎玻璃」）。
* **NaN**：源数据里 12/24k 个 UV 是 NaN，旧版直接写进 OBJ（Unity 导入会炸出 IndexError），现在填 0。

### 2.5 ★ V 翻转：INVO 的 UV 是 D3D9 约定（本 bug 的第二次修正）

即使 UV 偏移取对了，贴图仍然上下镜像。用户实测给出判据：**仪表盘网格的 UV 落在贴图右上区域，
而仪表盘美术在右下区域** ⇒ 上下翻一下就对。

根因：**INVO 里的 UV 是按 D3D9 写的（v=0 = 贴图顶行）**，而 OBJ/Blender/Unity 的 OBJ 导入器是
**左下原点（v=0 = 底行）** ⇒ 导出 OBJ 时必须写 `1 - v`
（`invo_obj.read_submeshes` 里的 `w = 1.0 - w`）。glTF 与 D3D9 同向（左上原点），
所以 GLB 那条路上 `pack_unity` 从 OBJ 再翻一次正好抵回来 —— **这两次翻转都不能删**。

**独立验证（不靠我肉眼认图集）**：用 `steering_wheel.obj`（UV bbox 正好 [0,1]²）叠在它自己的
`steering_wheel_int.png`（128²）上：

| 方向 | 结果 |
|---|---|
| 不翻（旧） | 「轮毂圆盘 + 圆环 + 辐条」的 UV 岛落在贴图**右下角的空处** ✗ |
| **翻转后** | 同一组 UV 岛**严丝合缝**压在贴图**右上角那个圆形轮盘**上（圆盘=轮毂、大弧=轮缘）✓ |

车壳同理：翻转后 Tornado 车壳的 UV 岛逐块压住仪表两个圆表盘 / 收音机旋钮 / 挡位杆 ✓。

⚠ **教训（写进 skill）**：`docs/53 §3b` 当年写「OBJ 的 vt 是左下原点、预览器 `(1-v)*h` 采样目视验证过」
——那是**循环论证**（预览器自己就假设了左下原点），加上车包图集又黑又密，肉眼根本分不出来。
判断纹理方向要**找有明确上下方向的标志物**（方向盘/仪表圆盘/文字/logo），别靠「看着挺像」。

### 2.6 验证（都是纯离线，不需要实机）

| 手段 | 结果 |
|---|---|
| **方向盘标志物对拍**（最强证据）| `steering_wheel.obj` 的「轮毂+轮缘+辐条」UV 岛翻转后**严丝合缝**压在 `steering_wheel_int.png` 的圆形轮盘上；不翻则落在右下角空处 |
| UV 线框叠在贴图上（`tools/mesh_qa.py uv`） | 翻转 + 偏移双修后：Tornado 车壳的 UV 岛逐块压住仪表圆表盘 / 收音机旋钮 / 挡位杆；Corus 车壳同样贴合 |
| 带贴图的正交视图（`tools/mesh_qa.py render`） | Tornado 侧视 = **认得出来的车身**（车门/车窗/轮拱 + 透过的仪表台），不是乱纹。注意该渲染是画家算法无深度缓冲，内饰会盖在车壳上 —— 看对位请用上面的 UV 叠图 |
| Unity 导入自检（`LasrImportSelfTest`） | Corus 原样匹配 4420 / Tornado 4245 / Fujin U_rim_ST 2438 ⇒ **导出↔导入一致**（Unity 不再翻 V）。注意这条**证明不了**原始约定对不对 —— 那是方向问题的循环论证，靠上面的方向盘标志物 |
| 导出物全体 UV 越界率 | 车包全部 0 个越界顶点（修前 Tornado 一类网格成片越界）|
| 新增硬断言 | `LasrImportSelfTest` 里「导入 UV 与源 vt 匹配率 < 50% ⇒ ✗」，这类 bug 以后跑不掉 |

复现命令：

```bash
PY=./.capenv/Scripts/python.exe
$PY tools/mesh_qa.py uv     assets/meshes/vehicles/Takura_Tornado_2002/chassis.obj \
                            assets/textures/vehicles/Takura_Tornado_2002/interior.png \
                            -o preview_tmp/uv_tornado.png --size 900
$PY tools/mesh_qa.py render assets/meshes/vehicles/Takura_Tornado_2002/chassis.obj \
                            assets/textures/vehicles/Takura_Tornado_2002/interior.png \
                            -o preview_tmp/render_tornado.png --view iso
```

> **Blender 侧**：OBJ 的 `vt` 是标准「左下原点」，Blender/Unity 的 OBJ 导入器都原样使用，
> 不需要翻 V（自检的 `原样匹配 ≫ 翻转匹配` 就是这条的机器证据）；GLB 按 glTF 规范已翻 V。

---

## 3. 地图/场景网格：**本来就没有顶点 UV**

`maps/**` 里有 37 个网格（47 个块）**一个 UV 都没有**：`mirror_black`（stride 28 / flags 0xc1）的
28 字节 = `pos(12) + normal(12) + 色(4)`，塞不下 UV。

依据：这些存档的材质表里**只有** `mesh / flags / shd_center|diru|dirv|vbase|vup / texture`，
**没有任何 uv 变换**（对比车包偶见 `shd_uv 0.0 0.0 1.0 1.0`），而赛道面是**有图案的**
（`asphalt` / `sidewalk` / `advertises` 多层）⇒ 纹理坐标只能是 **shader 按世界坐标平面投影**生成。

重制侧怎么办：
* 正式做法：在 shader 里做**平面 / 三平面投影**（世界 XZ 或按面法线选轴）。
* 查看/预览：`invo_obj.planar_uv(subs, tile_m)` 会补一份 `uv = (x,z)/tile_m` 平面 UV；
  `rpak_assets.py --meshes` 对**没有 UV 的块**自动补（`PLANAR = 10.0` m）。
  这只是「看得见贴图」的默认值，**真正的 tile 尺寸要按赛道实测调**（记为未决）。

---

## 4. B7：零件贴图 id → 文件名（闭）

### 4.1 正解 = 目录记录的**尾字段**

`docs/53 §1.1` 早就写了记录格式的最后两个字段：

```
u32 kind       // (group << 16) | type
u32 index      // ★ 该资源在本存档里的 id —— 材质表/零件类引用的就是它
```

`tools/material_table.archive_table()` 读的就是它。**旧流程只在「材质表里出现过的 id」里找名字**
（`pack_unity.resolve_part_textures` 遍历 `out_material_tables.json` 的 `texture` 条目建表），
所以 433 条零件贴图引用只解出 12 条。改成用**全量 id→名字表**（`out_tex_roles.json` 的 `id_table`）
后 **347/347 全部解名**。`out_tex_roles.json` 现已把 39 个存档的全量表都存下来。

### 4.2 还差一层（✗ 未决，别当已验证）

「id → 名字」是机械核对 ✓，但「**这个 id 是哪个零件的 mask/shadow**」没闭合：

| 零件类 | mask/shadow id | 解出的名字 | 问题 |
|---|---|---|---|
| `Corus chassis` | 124 / 125 | `F_bumper_6_mask.dds` / `F_bumper_6_shadow.dds` | 车壳的 mask 竟是 **WB(`_6`) 前杠**的 |
| `Corus F_bumper` | 93 / 106 | **`mask` / `shadow`**（size 0 的**抽象类记录**）| 指向类而不是实例 |
| 车壳图集同族 | 121 / 206 | `stock_mask.dds` / `stock_shadow.dds` | 这才是「看起来对」的那对 |

⇒ 与 `docs/57 §8.2` 记录的「WB 区整体平移一件」是同一族问题：**抽象类记录（`mask`/`shadow`，size 0）
与具体实例（`*_mask.dds`）混在同一个 id 空间里**。修法与 WB 重绑同路（族前缀匹配 + 取 id 最近者），
留作下一步；在此之前 `pack_unity` 把这一路标 `role_confidence = inferred`。

### 4.3 为什么这条一直没被发现

`resolve_part_textures()` 返回的字典键少了 `assets/meshes/` 前缀，而消费方按
`f"assets/meshes/{rel}"` 取用 ⇒ **每条 mask/shadow 都取不到**，只有**计数**进了日志
（所以 docs 里一直写着「337 个解出」而包里一个都没有）。本轮修掉，并加了 [G] 断言看住。

现在 `mesh_textures.csv` 是 7 列：

```csv
obj,base_texture,all_textures,base_texture_path,mask_texture,shadow_texture,dirt_textures
```

* 第 4 列 `base_texture_path` 是**完整资产路径** —— 贴图不总与网格同目录，page 4/5 的贴图来自
  全局池 `maps/texture.rpk`（落在 `Textures/maps/texture/`），C# 侧老规矩「同目录 + 同名」找不到
  （`boulevard/skydome` 曾因此漏绑 `sign.png`）。
* mask/shadow/dirt 只作**数据**提供（语义未闭，见 §4.2），`LasrModelMaterialFixer` **不自动绑**它们。

---

## 5. 回归看住

| 看住什么 | 在哪 |
|---|---|
| 导入 UV 与源 vt 一致（匹配率 ≥50%）| `LasrImportSelfTest.Run`（样本含 stride 36/60/64 + 地图 + 粒子）|
| 10 个车包 chassis 主贴图 = 车壳图集 | `verify_unity_package.py` [G] |
| 零件 mask/shadow 解名 ≥70% | 同上 |
| `mesh_textures.csv` 带完整路径且文件都在包里 | 同上 ×2 |
| 全量 id 表可用 | 同上（读 `remaster/data/mesh_assets`/`out_tex_roles.json` 生成物）|

跑法：

```bash
PY=./.capenv/Scripts/python.exe
$PY tools/rpak_assets.py --meshes --mtl      # 重出 764 OBJ + 665 MTL
$PY tools/tex_roles.py                       # 重出 out_tex_roles.json（改了角色规则才要）
$PY remaster/tools/pack_unity.py             # 重出 Unity 包
$PY remaster/tools/verify_unity_package.py   # 23 项
"/c/Program Files/Unity/Hub/Editor/6000.6.3f1/Editor/Unity.exe" -batchmode -nographics -quit \
  -projectPath remaster/unity_project -executeMethod Lasr.ImportSelfTest.Run
```
