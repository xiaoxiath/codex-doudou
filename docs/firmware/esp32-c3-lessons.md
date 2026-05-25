# ESP32-C3 + GC9A01 + CST816D 开发踩坑总结

本文是 Doudou 固件从 MVP-0 烧到 MVP-1c 期间踩出来的坑的合订本,主要面向
后续在 `1.28inch_ESP32-2424S012` (ESP32-C3 + 240×240 圆 LCD + 触摸) 上做
LVGL/Wi-Fi/WebSocket 应用的人。

更宽的架构和规划见 `docs/technical-plan.md`,本文只写"为什么这样做、不
这样做会怎么炸"。

---

## 1. 硬件层 (GC9A01 显示)

### 1.1 颜色"反相+粉紫" → 三件套必须同时配齐

GC9A01 自带 `Invert ON` 指令并不是可选的,vendor 的 Arduino 例程默认
开启;同时面板 pixel order 是 BGR,而 ESP-LVGL 默认按 RGB 推像素。
任一项漏掉都会得到偏粉紫/偏青的反色画面。

正确配置 (`pins.h` / `lvgl_port.c`):

| 项 | 值 | 备注 |
|----|------|------|
| `LCD_CMD_INVON` (0x21) | 必发 | vendor demo 在 init seq 里发,我们也要 |
| pixel order | BGR | `esp_lcd_panel_set_color_format` 或 panel 自己的 madctl bit3 |
| 字节序 | swap | RGB565 在 LVGL 里是小端,GC9A01 要大端,刷之前 `lv_draw_sw_rgb565_swap(px, w*h)` |

刷字节序的开销实测可忽略 (~240 像素一行约几 μs),不要为了"性能"省掉。
Wokwi 仿真不需要 invert/BGR/swap,用 `CONFIG_DOUDOU_LCD_FOR_WOKWI`
条件编译隔离。

### 1.2 LCD/触摸的电源时序

CST816D 上电后 **必须** 走完一次复位脉冲 (RST 拉低 10 ms → 拉高
300 ms),INT 引脚要先输出 HIGH→LOW 让芯片进入空闲态,再切回 input
**且开内部上拉**。开漏的 INT 信号在 floating input 下读 I²C 会拿
垃圾数据。

---

## 2. 触摸层 (CST816D)

### 2.1 gesture 寄存器会"卡值"

`0x01` 读出来的手势 ID 在一次触摸后会在芯片里 latch ~1 秒。如果你按
100 Hz 轮询 + 每次都打日志,UART 会被 130 行/秒淹没,**WS 任务因此
饿死**,bridge 直接判定设备掉线。

修法 (touch.c:160-167):

```c
static uint8_t s_last_gesture = 0xFF;
if (gesture != s_last_gesture) {
    if (gesture != 0) ESP_LOGI(TAG, "gesture=0x%02x", gesture);
    s_last_gesture = gesture;
}
```

只在变化沿打。这一改让 WS 心跳能稳定保持。

### 2.2 一次按下不能触发两次

gesture latch + 我们 100 Hz 轮询,会出现"一次手指划过,bridge 收到两次
swipe"。需要在 **按下边沿** 锁一个 `fired_this_press`:

```c
if (e.pressed) {
    last_press_us = now_us;
    if (!was_pressed) fired_this_press = false;   // 新的一次按下
}
was_pressed = e.pressed;
bool recent_press = (now_us - last_press_us) < 800 * 1000;
if (recent_press && !fired_this_press
    && e.gesture != last.gesture && e.gesture != NONE) {
    fired_this_press = true;
    /* dispatch */
}
```

只用 `gesture != last.gesture` 是不够的 —— 同一次按下里 gesture 寄存器
偶尔会复读。

### 2.3 "幽灵滑动"

手都没碰到屏幕,设备自己换屏。原因是 reset 时序不对 + INT 引脚悬空
读到的随机值被当成 gesture。修好 §1.2 的复位时序 + INT pull-up 后
消失。

---

## 3. LVGL v9 在 C3 上的注意事项

### 3.1 partial buffer,绝不要全屏 double buffer

C3 只有 400 KB SRAM,扣掉 Wi-Fi/TLS heap 和 WS/JSON 临时缓冲后,LVGL
画布预算 < 30 KB。我们用 `240 × 40 × 2 = 19200` 的 partial buffer,
两块共 ~38 KB。

`240 × 240 × 2 = 115 KB` 全屏 buffer 会直接 OOM 在第一次 flush。

### 3.2 `transform_scale` 在 sprite 上是昂贵的

之前给 body 做"呼吸 256→272"用 `lv_obj_set_style_transform_scale`,
每帧都会让 LVGL 重新双线性采样整个 200×200 的 RGB565A8 sprite。在
C3 上单帧能跑到 200+ ms,叠加 wiggle 时会把渲染队列灌爆,LVGL
卡在 `lv_timer_handler` 不返回 → 看起来像"系统死了"。

替代方案:
- 呼吸用 **边缘光晕透明度** 渐变 (一个独立的圆环 widget,改 opa,不动 sprite)
- wiggle 用 `translate_x` (整图平移,不重采样) 而不是 `transform_rotation`

### 3.3 负的 `transform_scale_x` ≠ 镜像

LVGL v9 里 `transform_scale_x = -256` 不会水平翻转,会让 sprite 直接
不可见。要做镜像得拿翻转过的源图。

### 3.4 边缘光晕不要挂 `lv_layer_top`

挂在顶层后只要它 invalidate 就把整个屏幕都拖下水。改成 PET 屏的子
widget,失效区域只在自己的 bounding box 内。

---

## 4. 字体 / 文字

### 4.1 中文必须做子集

完整 GB18030 字模 > 1.5 MB,不可能装进 1.79 MB OTA slot。用 lv_font_conv
做 GB2312 一级子集 (3755 字 + ASCII),2bpp 大概 ~200 KB,够用。
生僻字看不到就当 fallback 到 `?` 即可。

### 4.2 单行 + 跑马灯

圆屏顶部空间窄,标题强制单行,溢出用 `LV_LABEL_LONG_SCROLL_CIRCULAR`。
不要用 `LONG_WRAP` —— 两行会把头像挤下去。底部 4 个圆点用 flex-row +
`LV_ALIGN_BOTTOM_MID`,保持各屏一致。

---

## 5. Wi-Fi + WebSocket

### 5.1 mDNS 不要走 `mdns_query_a` 解 IP 字面量

如果 bridge host 配的是 `192.168.1.42` 这种 IPv4 字面量,
`mdns_query_a("192.168.1.42", ...)` 会卡 4 秒等不到响应。要在调用
前先 `inet_aton` 判断,IPv4 字面量直接跳过 mDNS。

### 5.2 UART 日志洪水 = WS 心跳超时

§2.1 已经说过一次。这里再强调:**任何 ≥ 50 Hz 的高频日志都要做去
重**,否则 WS keepalive 任务的 CPU 配额会被 UART 写挤掉,bridge 90 秒
没收到 pong 就 close。

### 5.3 断线时清掉错误标题

WS 断 → `on_ws_disconnected` 把标题改成 "Bridge disconnected",
WS 重连 → `on_ws_connected` 一定要 `doudou_pet_set_title("")` 清掉,
否则会一直挂着错误提示直到 bridge 推下一条 `session_info`。

---

## 6. Flash / 分区

### 6.1 OTA 槽位 1.5 MB → 1.75 MB

LVGL + 中文字模 + sprite art + Wi-Fi/WS + cJSON,加起来 `doudou.bin`
≈ 1024 KB,留给后续加 BLE / TLS 余量不足。分区调整成 ota_0/ota_1 =
`0x1C0000` 各 1.75 MB,assets 缩到 384 KB。

> **注意**:`docs/firmware/partitions.csv` 里的偏移一旦改了,所有已经
> 烧过 OTA 的设备升级时要带迁移逻辑。Bringup 期可以随便改,有用户群
> 之后必须配 OTA migration plan。

### 6.2 sprite 大小别贪

19 个 sprite 总占 ~146 KB raw RGB565A8。再往上加新表情会很快吃光
预算,优先压尺寸:
- body 200×200 (1:1,贴源 PNG 比例)
- 眼睛 48×48 (匹配 simulator CSS 的 48px)
- 嘴 48×32 (源 PNG 是 3:2)
- 配饰 48×48
- 腮红 24×24

---

## 7. 动画与状态机

### 7.1 比例匹配源 PNG

源 PNG 是 1254×1254 (1:1),早期我误以为"圆屏要拉宽"渲染成 272×200,
出来豆豆脸被压扁。**记住:sprite 目标尺寸宽高比必须等于源 PNG 宽高
比**,见 `scripts/build_pet_art.py` 顶部注释。

### 7.2 五官位置按百分比反算

从 simulator 的 CSS 反算各五官中心相对 body 的百分比,再 × 200 得到
像素坐标:
- 腮红 cheek_l (32, 112), cheek_r (144, 112)
- 眼睛 eye_l (52, 84),    eye_r (104, 84)
- 嘴   mouth ((W-48)/2, 120)

不要凭感觉调,会有"右眼偏内"之类的复发问题。

### 7.3 SLEEPING 是固件本地状态

device protocol 没有 sleeping。但用户在房间里走开 1 分钟后,豆豆应该
能自己睡觉 —— 用一个 `lv_timer`,任何 `set_state` 都重置计时,60s 后
切到 `DOUDOU_PET_SLEEPING`。下次收到协议状态自动退出。

### 7.4 边缘光晕颜色 = 状态色

```
idle      0xfce8ad  暖黄
thinking  0xa8c8ff  蓝
executing 0xffd47a  橙
waiting   0xd8b7ff  紫
done      0x94e9a3  绿
error     0xf47878  红
sleeping  0x6b7390  暗灰蓝
```

LVGL `lv_obj_set_style_bg_opa` 在一个圆环 widget 上做 ease 渐变就够,
不需要复杂粒子。

---

## 8. Bridge / 数据来源

### 8.1 Codex Desktop 不创建新 rollout

Desktop 一个 thread 会一直追加同一个 `rollout-*.jsonl` 文件,可能是
半个月前创建的。Bridge 默认只扫最近 3 天的目录 → 看不到桌面的活跃
thread。我们把回溯窗口拉到了 **90 天**。

### 8.2 `thread_name_updated` 事件单独处理

`session_info` 默认只带 session_id / source / cwd / cli_version /
git_branch,不带 thread title。Codex 后续会发 `event_msg.type =
thread_name_updated` 的事件,里面才有 `thread_name`。要在 `onEventMsg`
里加专门的 case 把它转成 `session_info.thread_title` 再推给设备。

### 8.3 follow mode 是只读

Bridge 永不写 `~/.codex/`。要测试设备状态变化的最简方式是在 Codex
Desktop 里发一条 prompt,bridge tail rollout 文件抓到 task_started/
agent_message 自然就推过来了。

---

## 9. 调试技巧

### 9.1 host 单测覆盖纯逻辑

`packages/firmware/test/host/` 里的 protocol_parse 单测不需要 IDF,
直接 gcc 编译运行。每加一种新消息类型都先在这写测试,再去板子上
跑。43 条测试目前都用这种方式维护。

### 9.2 Wokwi 仿真做 UI 迭代

不用每次烧板子。`packages/firmware/diagram.json` 在 VSCode Wokwi 扩展
里能直接跑 `doudou.bin` + LCD + 按键。Wokwi 没 Wi-Fi/BLE/CST816D,
所以协议链路还是得靠真机。

### 9.3 看 `idf.py size-components`

ROM 现在 ota slot 还剩 ~1% 余量 (1.81/1.79 MB)。每次加大功能前跑一
下 `idf.py size-components`,谁占得多一目了然 (lvgl 通常榜首)。

---

## 10. 不要做的事

- 不要把 OpenAI/Codex 凭据透过 protocol 推到设备上。设备永远不应该
  知道任何 token。
- 不要在设备侧解析 Codex 的原始事件 —— Bridge 必须先 normalize。
- 不要在 MVP 阶段加 TLS。Wi-Fi + LVGL + JSON + cJSON 在 C3 上已经
  贴着堆顶,TLS 握手会偶发 OOM。LAN scope + pairing token 现阶段
  够用。
- BOOT/RESET 按键只用于刷机,不是用户交互入口。唯一输入是触摸。
- sdkconfig.local 里有 Wi-Fi 密码 —— 已经在 `.gitignore` 里覆盖,
  但任何时候 `git add -A` 之前先 `git status` 检查一遍。
