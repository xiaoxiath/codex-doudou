# Wokwi 虚拟跑豆豆固件

## 是什么

[Wokwi](https://wokwi.com) 在浏览器/VSCode 里指令级模拟 ESP32-C3,加载 **你刚 `idf.py build` 出来的真 `doudou.bin`**,接上虚拟的 LCD + 按钮,UI 改动几秒就能看到。**Wi-Fi 是 gateway mock,BLE 不支持** —— 视觉/逻辑迭代用它,真链路还得回真机。

## 一次性安装(VSCode 路线,免登录)

1. VSCode 装扩展:**Wokwi for VSCode** (官方,搜 "Wokwi")
2. 第一次启动会要求免费 license:点扩展提示 → 浏览器登录 → 拷回授权码 → 完事
3. 仓库已经放了:
   - `packages/firmware/wokwi.toml`(指向 `build/doudou.bin` + `build/doudou.elf`)
   - `packages/firmware/diagram.json`(虚拟接线:C3 + LCD + 触摸按钮替代)

## 用法

```bash
cd packages/firmware
. ~/esp/esp-idf/export.sh

# 推荐:用 Wokwi 专用 profile build —— 颜色看起来才对
rm sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.wokwi" reconfigure
idf.py build               # 生成 build/doudou.bin + build/doudou.elf

# 在 VSCode 里打开 packages/firmware/diagram.json
# 顶上有个 ▶ 按钮 — 点了直接开跑
```

**为什么需要 wokwi 专用 build**:Wokwi 只有 ILI9341 LCD 模型(没 GC9A01)。真机 GC9A01 需要 **色彩反转 + BGR 像素序**;ILI9341 完全相反。直接烧默认 build,Wokwi 里图像会变 **负片 + 蓝红互换**(豆豆的黄色变青绿色)。`sdkconfig.defaults.wokwi` 关掉这两个 quirk,Wokwi 视觉就正常了。**真机烧此 build 颜色会反**,所以这个 profile 仅供预览。

回真机:
```bash
rm sdkconfig
idf.py reconfigure         # 回默认 profile(GC9A01 quirks 打开)
idf.py build flash monitor
```

跑起来你看到的:
- **左上**:虚拟 C3 board(GPIO 状态实时)
- **右上**:240×240 LCD(豆豆 + 状态屏)
- **下方**:3 个按钮代替触摸 —— `tap`(单点) / `←` `→`(模拟滑动切屏)
- **底部串口**:`ESP_LOGI` 全量输出,Ctrl+C 停

## 关于硬件失真

| 项 | 真实板子 | Wokwi 模拟 |
|---|---|---|
| LCD 驱动 | GC9A01(圆 240×240) | ILI9341(矩形 240×**320**)。固件画 240×240,**底部 80 行是未初始化内容**(竖纹/花屏)。心理上裁掉底部 80 行即可,顶上 240×240 是真实的 UI |
| LCD 色彩 | 默认 build → 真机正确 | 必须用 `sdkconfig.defaults.wokwi` build,否则 **负片 + BGR**(豆豆变青色) |
| 触摸 | CST816D I2C + 手势识别 | 3 个虚拟按钮 → 拉低 GPIO0/8/9。**不会** 触发 single/double/long-press/slide 真实手势码 |
| Wi-Fi | 真 STA + DHCP | gateway mock(能 "连上" 但访问不了真 bridge) |
| BLE | NimBLE peripheral | **不支持**,选 BLE build 会卡 advertising |
| 时钟同步 | 从 bridge welcome 拿 | 没真链路 → 时间停在 1970 |

## 建议工作流

1. **改 LVGL UI**(pet_ui.c 位置/动画/字号)→ Wokwi 看(秒级反馈)
2. **改协议解析**(protocol_parse.c)→ `pnpm firmware:test`(host 单测,更快)
3. **真链路 / Wi-Fi / BLE** → 真机 `idf.py flash monitor`,没替代品

## 调试小技巧

- **断点**:Wokwi 支持 GDB,VSCode 自带界面,在 .c 文件左边栏点行号下断点
- **核心 dump**:崩了 Wokwi 直接显示 backtrace,符号已经从 .elf 解过
- **重启** 比烧机快 50 倍 —— 改了代码 `idf.py build` 后再点 ▶
- **不要在 Wokwi 里跑 BLE build**:`sdkconfig.defaults.ble` 配置会触发 NimBLE 初始化失败 → boot loop

## Wokwi 在线版(不用 VSCode)

把 `wokwi.toml`、`diagram.json`、`build/doudou.bin`、`build/doudou.elf` 上传到 [wokwi.com](https://wokwi.com),建一个 "Custom Firmware ESP32-C3" 项目即可。不过本地 VSCode 路线更顺手,推荐那个。
