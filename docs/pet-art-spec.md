# Doudou 宠物美术规范 v1

> **⚠ 文档状态(2026-05)**:此文档保留作为 **设计探索过程史**。原规划是 18 张人工/AI 画的 SVG sprite 并存 Claude/Codex 两套对比。**实际实现已经偏离**——
> 走了 AI imagegen → PNG → LVGL `lv_image_dsc_t` 的路线,身体 + 脸部件都是 PNG 而非 SVG。当前真相见:
>
> - **源 PNG**:`packages/simulator/public/pet-art/` —— 19 张透明背景 PNG(`pet_body_glossy.png` / `pet_body_neutral.png` / `cheek.png` / 6 eye / 5 mouth / 6 acc)
> - **打包工具**:`scripts/build_pet_art.py` —— 缩放 + RGB565A8 编码 + 写 C 数组
> - **设备资产**:`packages/firmware/main/pet_art/*.c` + `pet_art.h`(19 个 `lv_image_dsc_t`,~146KB raw)
> - **模拟器渲染**:`packages/simulator/src/ui/pet.ts` + `style.css` 分层 sprite + CSS transform 动画
> - **固件渲染**:`packages/firmware/main/pet_ui.c`(`lv_image_create` + `lv_anim_t` 动画)
> - **生成审批参考图的迭代历史**:`docs/pet-art-references/codex-imagegen-v*.log`
>
> 角色定调 / 状态 → sprite 映射(下文)仍然成立。其余的 SVG path / 多套对比 / 史莱姆 path morph 等只是设计阶段的探索,不代表当前代码。

> 18 个 sprite 的精确规格。Codex 与 Claude 各按此 spec 出一套 SVG 进 `packages/firmware/assets-src/pet/{codex,claude}/`,然后人眼并排选优胜方案。

## 角色定调

**名字**:豆豆(Bean)
**性格**:小巧、好奇、温柔但偶尔慌张。不爱表演,只是默默存在 — 看到主人在做事就高兴,看到主人不在就睡。
**视觉关键词**:**卡通史莱姆 / 果冻软糖 / 圆滚滚 / 大眼晶亮 / 蹲坐稳定 / 边缘永远在缓动**

## 身体定调:坡状圆顶豆豆

豆豆 **是一个矮胖软糖坡丘**,像一个包子、一团麻糬、一座小山丘 —— 没有任何尖点,顶部是一段宽阔的圆弧,从最高点向两侧平滑斜坡到地面。

**核心比例(死规则)**:
- **宽 > 高**(约 1.7 : 1) —— 比之前还要更矮胖,像一团摊开的麻糬
- **底部接近平** —— 与地面有一段平直接触面,坐感稳定
- **两侧斜坡** —— 从中线最高点向左右两侧平滑下降到底盘,中段微微外凸但不强调"腰"
- **顶部圆顶,无尖** —— 最高点是一整段宽弧线在中心区域,**绝对不收成一个尖** 或 "softserve 水滴顶"。任何"尖"都是失败

参考形态:**包子 / mochi / 蘑菇头顶 / 坐着的雪人下半身 / 馒头**。**不要** 画成:
- 圆球(没有平底,会滚)
- 任何带尖点的轮廓(水滴、softserve、Dragon Quest slime)
- 竖立的椭圆(高 > 宽)
- 雨滴(顶部细底部宽,会被识别为"水滴"角色)

### 对称(铁律)

**稳定/静止状态(idle 帧、状态切换完成后的 hold 帧、参考图):必须左右镜像对称(垂直中线)**。

- 身体轮廓:严格 mirror-symmetric across the vertical center axis
- 顶部最高点必须落在垂直中线上;最高点 **是一段弧线**,不是一个尖
- 两眼:同尺寸、同高度、关于中线镜像
- 两腮红:同尺寸、关于中线镜像
- 嘴:水平居中
- 配件(浮动)**不需要** 对称(它就是浮动装饰,放右上或左上都行;锁单侧即可)
- **唯一例外**:身体表面的白色高光按"左上光源"画(自然光照惯例)。这是体积感所需,不算违反对称(因为我们建模的是 3D 灯光,而不是 2D 图形)

**动画过程中** 允许不对称(squash 时整体压扁可以微偏、wobble 时左右切换、tap 时朝点击方向缩) —— 那是 SVG path morph 的中间帧,不是稳定态。

为什么这条铁律重要:不对称的"静止"豆豆看起来像 **没站直的醉鬼**,体感不稳定、廉价。对称的豆豆有"端正坐着"的安定感,kawaii 的核心。

### 对称(铁律)

**稳定/静止状态(idle 帧、状态切换完成后的 hold 帧、参考图):必须左右镜像对称(垂直中线)**。

- 身体轮廓:严格 mirror-symmetric across the vertical center axis
- 顶部尖点必须落在垂直中线上,**不能偏左或偏右**
- 两眼:同尺寸、同高度、关于中线镜像
- 两腮红:同尺寸、关于中线镜像
- 嘴:水平居中
- 配件(浮动)**不需要** 对称(它就是浮动装饰,放右上或左上都行;锁单侧即可)
- **唯一例外**:身体表面的白色高光按"左上光源"画(自然光照惯例)。这是体积感所需,不算违反对称(因为我们建模的是 3D 灯光,而不是 2D 图形)

**动画过程中** 允许不对称(squash 时整体压扁可以微偏、wobble 时左右切换、tap 时朝点击方向缩) —— 那是 SVG path morph 的中间帧,不是稳定态。

为什么这条铁律重要:不对称的"静止"豆豆看起来像 **没站直的醉鬼**,体感不稳定、廉价。对称的豆豆有"端正坐着"的安定感,kawaii 的核心。

### 标准轮廓 SVG path

身体走 inline SVG `<path>`,viewBox `0 0 160 110`:

坡状圆顶用一道大跨度的顶部弧 + 两条侧斜坡 + 平直底盘组成。
所有 path 必须严格 bilateral-symmetric around x=80(对称铁律见上)。

**Path A(基准静止,完全对称)**:
```
M 10 60
C 10 14, 150 14, 150 60
C 150 88, 132 96, 118 96
L 42 96
C 28 96, 10 88, 10 60 Z
```

**Path B(右肩轻起,动画用)**:
```
M 10 62
C 12 18, 148 12, 152 60
C 152 86, 134 96, 118 96
L 42 96
C 26 96, 8 88, 10 62 Z
```

**Path C(压扁蹲低)**:
```
M 8 64
C 8 24, 152 24, 152 64
C 152 90, 132 98, 118 98
L 42 98
C 28 98, 8 90, 8 64 Z
```

注意 Path A 和 Path C 都是 bilateral-symmetric;Path B 故意右倾(动画
中间帧)。SMIL 在 A→B→C→A 之间 spline 插值,稳定态只有 A 或 C,都是
对称的。

**SMIL 动画**:
```xml
<animate attributeName="d"
         values="A;B;C;A"
         dur="5800ms" repeatCount="indefinite"
         calcMode="spline"
         keySplines="0.4 0 0.6 1; 0.4 0 0.6 1; 0.4 0 0.6 1"/>
```

三个 keyframe 之间无限插值 → 史莱姆轮廓 **永远在缓慢起伏**,像一团活的果冻。

### 流体视觉规则(必须全部满足)

1. **轮廓永远在动**:SMIL `<animate>` 在 A/B/C 之间 spline 插值,主层 5.8s,内层高光异步 7.1s,叠加出"持续起伏"
2. **果冻内胆**:在主体上 **再叠一份 65% 缩放的同形 path**,左上偏移 (-22, -28),白色 40% 透明 —— 看起来像一个透明果冻里有更亮的"果心"
3. **地面阴影**:身体底下一条窄椭圆 (`rx=48 ry=6`) 黑色 18% 透明,给"坐在桌面"的实感
4. **外发光**:`filter: drop-shadow(0 8px 14px rgba(0,0,0,0.35))` + `drop-shadow(0 0 30px var(--state-glow))` —— 同状态色光晕
5. **transform-origin = 50% 100%**(底部中央) —— 所有挤压/呼吸/抖动都以底盘为锚,挤压时 **底不动、顶尖伸缩**,像真水球
6. **呼吸**:`scaleY(1.04) scaleX(0.98) translateY(-2px)`,2.6s 循环
7. **状态切换 = squash-stretch**(700ms):20% 时 scaleX(1.20) scaleY(0.78) 压扁,50% 时 scaleX(0.86) scaleY(1.20) translateY(-8px) 拉长,有"弹起"重量感
8. **触摸 = jelly-tap**(650ms):4 周期阻尼振动,scale 在 1.14×0.84 ↔ 0.90×1.14 之间衰减

### Web/CSS 实现

主体 = **inline SVG**,SMIL `<animate>` 自然在浏览器执行,不需要 JS:

```html
<svg class="body-svg" viewBox="0 0 160 110">
  <path d="..." fill="var(--body-color)">
    <animate attributeName="d" values="A;B;C;A" dur="5800ms" .../>
  </path>
  <path d="..." fill="rgba(255,255,255,0.42)"
        transform="translate(-22 -28) scale(0.65)">
    <animate attributeName="d" values="A;C;B;A" dur="7100ms" .../>
  </path>
  <ellipse cx="80" cy="107" rx="48" ry="6" fill="rgba(0,0,0,0.18)"/>
</svg>
```

呼吸 + squash + tap 通过外层 div 的 CSS `transform` 叠加。

### Firmware/LVGL 实现路线

LVGL 不支持 SVG path 动画。两条路线:

**MVP 路线(简化)**:
- Path A 栅格化成 PNG(160×110 → 实际显示约 180×125,~10KB RGB565A8)
- `lv_image_recolor` 上状态色
- 呼吸 = `transform_scale` 256↔272,2.6s
- squash/tap = 短暂改 scale_x / scale_y 不同步
- **失去边缘形变**,但保留史莱姆形 + 呼吸 + squash,够 MVP 用

**V1 路线(预渲染帧)**:
- 把 A/B/C 之间用 SVG 插值出 12 帧 (~12 × 8KB = 96KB)
- `lv_animimg_set_src_array` 循环播
- 配合 recolor,所有状态共用 12 帧

## 全局约束

1. **格式**:SVG 1.1,UTF-8 不带 BOM,单文件无外链。
2. **根属性**:`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 W H" width="W" height="H">` — 这里 W/H 指目标像素尺寸,见下表。
3. **背景**:**完全透明**。不要画矩形背景。
4. **抗锯齿**:用 SVG 默认渲染(不要 `shape-rendering="crispEdges"`)。我们最终用 `rsvg-convert` 栅格化到 PNG,LVGL 在屏上还会再做一次混色。
5. **填色优先**:能用 `fill` 就不用 `stroke`。需要"线条"时也用细长 `<path>` 填充而不是 `stroke`,避免栅格化时线条变胖。
6. **不用图层/group 复杂嵌套**:扁平 path/circle/ellipse/rect 即可,后续 LVGL 不读 group。
7. **不用 filter / mask / gradient**:LVGL 栅格化后是位图,但保持源 SVG 纯净有助于版本控制。

## 调色板(必须严格使用这 8 色,RGB565 友好)

**脸部 sprite 用色:**
```
名字           HEX        用途
--------------- ---------- ----------------------------
INK_BLACK       #1a1a1a    眼/嘴主色(不用纯黑,避免 RGB565 银幕看着死)
GLINT_WHITE     #ffffff    眼睛高光、闪烁配件
CHEEK_PINK      #ffaec0    腮红、心型配件
ACC_AMBER       #f0b830    "?" 黄
ACC_CYAN        #6cc4f0    "💧" 水滴、"Z" 蓝
ACC_RED         #ef5350    "!" 红、X 眼
ACC_GREEN       #6ddf7d    勾、辅助绿
GEAR_GRAY       #8a8f99    齿轮
```

**身体本底色(运行时按 state 选,sprite 不烧):**
```
状态           主色        外发光(同色加亮 30%)
--------------- ---------- ----------------------------
idle            #f5d98a    #fce8ad
thinking        #87b3ff    #a8c8ff
executing       #ffc44d    #ffd47a
waiting         #c79aff    #d8b7ff
done            #6ddf7d    #94e9a3
error           #ef5350    #f47878
sleep           #d4c899    #e6dcb5
```

- 所有 sprite 都 **不画身体** — 身体由 CSS/LVGL 在底下绘流体形状(见 §身体定调),sprite 叠在身体上层。

## 18 个 sprite 总清单

| # | 文件名 | 尺寸 | 类别 | 用途 |
|---:|---|---|---|---|
| 1 | `eye_normal.svg`   | 16×16 | eye | 默认睁眼 |
| 2 | `eye_blink.svg`    | 16×16 | eye | 眨眼(平闭) |
| 3 | `eye_sleep.svg`    | 16×16 | eye | 睡眠(向下弧线) |
| 4 | `eye_x.svg`        | 16×16 | eye | 错误,十字 |
| 5 | `eye_surprise.svg` | 16×16 | eye | 惊讶大圆 |
| 6 | `eye_happy.svg`    | 16×16 | eye | 开心 ^^ |
| 7 | `mouth_smile.svg`  | 32×16 | mouth | 默认微笑 |
| 8 | `mouth_open.svg`   | 32×16 | mouth | 张嘴小 O |
| 9 | `mouth_grin.svg`   | 32×16 | mouth | 大笑(开心) |
| 10 | `mouth_wobble.svg` | 32×16 | mouth | 不安波浪 |
| 11 | `mouth_sad.svg`    | 32×16 | mouth | 哭嘴下弯 |
| 12 | `acc_zzz.svg`      | 24×24 | accessory | 睡眠 Z 字 |
| 13 | `acc_question.svg` | 24×24 | accessory | 思考 ? |
| 14 | `acc_gear.svg`     | 24×24 | accessory | 执行齿轮 |
| 15 | `acc_sparkle.svg`  | 24×24 | accessory | 完成闪光 |
| 16 | `acc_sweat.svg`    | 24×24 | accessory | 紧张水滴 |
| 17 | `acc_alert.svg`    | 24×24 | accessory | 错误警示 ! |
| 18 | `cheek.svg`        | 12×8  | cheek | 腮红椭圆 |

## 单 sprite 详规

### 眼睛 16×16(默认在豆豆脸部 24×24 区域内的左/右眼位置)

> 双眼对称使用同一个 sprite,水平镜像不是必须 — 让眼睛在视觉上左右等价。

**1. `eye_normal.svg`**
- INK_BLACK 椭圆,**水平稍扁**(rx=4 ry=5),`cx=8 cy=9`
- GLINT_WHITE 高光小圆,`cx=6 cy=7 r=1.5`,**左上方**
- 整体占视区中下部,头顶留出 4px 空白

**2. `eye_blink.svg`**
- 一道短弧线(下凸),整体高度 ≤ 3px
- 用填充 path:从 (3,9) 到 (13,9) 上凸为一条香蕉形,厚度 2px
- 中线大约 y=9(略低于中线,与 normal 视线对齐)

**3. `eye_sleep.svg`**
- 弧线倒过来(像微笑形状),代表"安心闭眼"
- 比 blink 更长更弯,弧度大,体现放松
- 仍是 INK_BLACK,2-3px 厚

**4. `eye_x.svg`**
- 两笔 **INK_BLACK** 短粗笔画交叉(原来用 ACC_RED 红色,与红色 error 身体撞色 — 改纯黑确保 error 状态下仍清晰可读)
- 各长约 9px,厚 2.5px
- 中心在 (8, 9)
- 端点圆滑(stroke-linecap="round")

**5. `eye_surprise.svg`**
- 大圆瞳孔,INK_BLACK,`r=5`,`cx=8 cy=9`
- 内含 **两个** GLINT_WHITE 高光:大的 `cx=6.5 cy=7 r=1.5`,小的 `cx=10 cy=11 r=0.7`(双高光显得"亮晶晶")

**6. `eye_happy.svg`**
- 倒 U / "^" 形,INK_BLACK 填充
- 从 (2,11) 上凸到 (8,6) 再下到 (14,11),整体像一弯眯起来的眼
- 厚度 2px,顶部圆滑

### 嘴 32×16

> 嘴居中放在豆豆下半脸,水平居中。

**7. `mouth_smile.svg`**
- 一道温柔上扬的弧线,从 (8, 8) 到 (24, 8),最低点 (16, 12)
- INK_BLACK 填充,厚度 2px,两端略微变细收尾

**8. `mouth_open.svg`**
- INK_BLACK 椭圆,`cx=16 cy=8 rx=4 ry=5`(竖向小 O)
- 内部下方留一道 CHEEK_PINK 小弯线代表舌尖(可选,精简也可不画)

**9. `mouth_grin.svg`**
- 张开的大笑嘴,上唇是直/微弯的水平线 (10, 7)→(22, 7),下唇是个大下弯弧到 (10, 7)→(16, 13)→(22, 7)
- 整体闭合 path 填充 INK_BLACK
- 嘴内可以留一抹 GLINT_WHITE 微亮(显得在笑出声)

**10. `mouth_wobble.svg`**
- 波浪线:从 (8, 9) 起,上下波动 2-3 次到 (24, 9),振幅 2px
- INK_BLACK,厚 2px

**11. `mouth_sad.svg`**
- 与 smile 镜像,下弯的弧
- 从 (8, 8) 到 (24, 8),最高点 (16, 4)
- INK_BLACK,厚 2px

### 配件 24×24

> 配件浮动在豆豆头顶或右侧,带 alpha 边缘。所有配件设计要在小尺寸下仍易识别。
>
> **艺术化方向(v2)**:不要做成扁平 icon。每个配件应该是 **一个小画面** —
> 主元素 + 1-3 个散落副元素(小星/微点/拖尾/动势线)+ 多色阶填充(主色 +
> 高光淡色)。轮廓允许轻微倾斜或非对称,**避免严格几何居中**。
> 目标:观感像 *水彩点缀* 而不是 *材料库图标*。
>
> 多色阶用法:每个配件至少 2 种色阶(主色 + 主色 30% 亮的同系),用于
> 高光或同系副元素;部分配件可叠 GLINT_WHITE 中心小光点。

**12. `acc_zzz.svg`** — 飘升的 Z 群 + 微气泡
- 3 个 Z 大小递增(小→中→大),从左下向右上飘
- 每个 Z **略带倾斜**(-5° ~ -10°)显得在空气中漂浮,不是排版整齐的字
- 颜色梯度:大 Z 用 ACC_CYAN,中 Z 浅一阶,小 Z 最浅
- **副元素**:2-3 个微小气泡圆点(r=0.5-0.7,浅 cyan)散落在 Z 之间,像 ZZZ 升起时的小气泡
- 大 Z 笔画 ~2.2px,中 ~1.7px,小 ~1.3px

**13. `acc_question.svg`** — 钩问号 + 闪粒
- 主问号居中,ACC_AMBER 钩 + 同色实心圆点
- **内层高光**:钩的左上侧叠一道更浅 amber(2px 半径短弧),做立体感
- **副元素**:右上角 1 颗小 4 角星(浅 amber,长尖 ~3px),暗示"灵光乍现"
- 整体不强求对称,问号微向左倾

**14. `acc_gear.svg`** — 双齿轮叠加 + 内圈高亮
- **主齿轮**(8 齿)居中偏左,外径 ~12px,GEAR_GRAY
- **副齿轮**(6 齿)右下方叠 30% 大小,深灰 `#5e6470`,故意只露出部分
- 主齿轮 **内圈高亮环**:在外缘 + 中心孔之间叠一圈更亮的 `#9ea3ad`(显得有金属质感)
- 中心孔黑色 `#1a1a1a`

**15. `acc_sparkle.svg`** — 星簇而非单星
- **主星**:1 颗大 4 角星,ACC_AMBER,中心稍偏左(11, 12),长尖 ~7px
- **副星**:2 颗中等 4 角星(浅 amber `#ffd060`),分别在右上 (19, 5) 和右下 (20, 19)
- **粒子**:4-5 颗微小 r=0.5-0.7 圆点散布,色阶最浅(`#ffe09c`)
- 主星中心叠一颗 r=1.3 GLINT_WHITE 小球做"亮芯"

**16. `acc_sweat.svg`** — 水滴串 + 高光
- **主水滴**:中下位 (cx=12),完整 teardrop,ACC_CYAN + 内侧白色高光椭圆
- **次水滴**:右上 (cx=18, cy=6),约 50% 大小,色阶亮一阶 `#8ad2f4`,内白点
- **微水滴**:右上角 (cx=21, cy=2.5),最小 ~30%,最浅 `#aedef6`
- 三滴 **由小到大** 从右上飘到中下,像滴落动势

**17. `acc_alert.svg`** — 感叹号 + 漫画冲击线
- 主感叹号 ACC_RED 填充,梯形竖条 + 圆点,内侧叠 `#ff7e7c` 浅红高光带
- **冲击线**:4-5 道 ACC_RED 短粗笔画从感叹号 **向外辐射**
  - 左两道(从 ! 左侧射向 (3,6) (3,13))
  - 右两道(对称)
  - 顶部一道短竖
- 每道 1.5px,圆头,与漫画"!"冲击感一致

### 颊红 12×8

**18. `cheek.svg`**
- 单一 CHEEK_PINK 椭圆,`cx=6 cy=4 rx=5 ry=3`
- 不加描边

## 状态 → sprite 组合表

LVGL 在脸上根据 state 切换组合。**两眼默认水平镜像 normal**,以下是各 state 的覆盖:

| State            | 左眼       | 右眼       | 嘴          | 配件        | 颊红 |
|------------------|------------|------------|-------------|-------------|------|
| `idle`           | normal     | normal     | smile       | (none)      | ✓    |
| `thinking`       | normal     | normal     | (none)      | question    | -    |
| `executing`      | normal     | normal     | open        | gear        | -    |
| `waiting_*`      | surprise   | surprise   | open        | (none)      | -    |
| `done`           | happy      | happy      | grin        | sparkle     | ✓    |
| `error`          | x          | x          | sad         | alert       | -    |
| `sleep`(深 idle) | sleep      | sleep      | smile       | zzz         | ✓    |
| `tense`(临时)   | normal     | normal     | wobble      | sweat       | -    |

> `sleep` 和 `tense` 为内部子状态;由 firmware 在 idle 超过 60s 进入 sleep,在 reconnecting 时短暂用 tense。

## 动画规范(仅供 firmware 参考,不影响 SVG 出图)

| 动画 | 实现 | 参数 |
|---|---|---|
| 身体呼吸 | `lv_obj transform_scale` 256↔272 | 1800ms 来回,`ease_in_out` |
| 眨眼 | 把眼 sprite 切到 `eye_blink` 80ms,再回 | 每 4-8s 随机 |
| 配件浮动 | `lv_obj` y 位移 0 ↔ -6,opacity 200↔255 | 1500ms 来回 |
| 触摸抖动 | 身体 `transform_rotation` -100↔+100(0.1°) | 150ms 双向 ×2 |

## SVG 模板(参考)

每个文件结构如下,只填实际内容:

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="16" height="16">
  <!-- ALL content here. NO background. NO defs (no gradients/masks). -->
  <ellipse cx="8" cy="9" rx="4" ry="5" fill="#1a1a1a"/>
  <circle cx="6" cy="7" r="1.5" fill="#ffffff"/>
</svg>
```

## 文件命名 & 输出位置

- Claude 出图 → `packages/firmware/assets-src/pet/claude/<name>.svg`
- Codex 出图 → `packages/firmware/assets-src/pet/codex/<name>.svg`

每方都必须产出全部 18 个文件名(见清单),哪怕某个状态简单到只有两条线也要给出完整 SVG。

## 对比口径

最终选谁的标准:

1. **辨识度**:配件 24×24 缩到屏上(实际占豆豆右上 ~32×32 px),不靠近看也能秒识出是 zZz / ? / ⚙ / ✨ / 💧 / !
2. **风格一致**:眼/嘴/配件三类是否像"出自同一只豆豆"
3. **栅格化稳定性**:rsvg-convert 到目标 PNG 后,边缘不糊不裂
4. **角色温度**:整体看上去要"软"、要"可爱",不能冷硬

## 与模拟器对齐

最终选定的那一套 SVG 直接 inline 进 `packages/simulator/src/ui/pet-art.ts`(浏览器原生渲染),硬件端走 SVG → PNG → LVGL `lv_image_dsc_t` 数组。**同一份 SVG 源、两端一致呈现**。
