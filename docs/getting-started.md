# Getting started

零到能在浏览器看到豆豆 → 真机烧录跑起来的完整路径。预计 30 分钟(不算 ESP-IDF 装机)。

## 0. 你需要什么

| 项 | 说明 |
|---|---|
| **Node.js v22+ / pnpm v10+** | 跑 Bridge、Simulator、TS 单测 |
| **Codex CLI(可选,但推荐)** | Bridge 用它做 follow mode 跟踪 Codex Desktop 会话 |
| **ESP-IDF v5.3**(只在烧真机时) | 固件构建 + 烧录工具链。装机:`mkdir -p ~/esp && cd ~/esp && git clone -b release/v5.3 https://github.com/espressif/esp-idf.git && cd esp-idf && ./install.sh esp32c3` |
| **真机(可选)** | ESP32-2424S012(¥40-60)。没有也能用 Wokwi 在浏览器里跑真 .bin |

## 1. 装依赖 + build

```bash
git clone https://github.com/xiaoxiath/codex-doudou.git
cd codex-doudou
pnpm install
pnpm -r build      # 编译所有 TS workspace
```

`pnpm -r build` 一次性把 `device-protocol`、`bridge`、`simulator`、`codex-app-server-protocol` 全编了。失败的话先看是不是 Node 版本太老(<22)。

## 2. 跑 Bridge + 浏览器模拟器

```bash
node packages/bridge/dist/cli.js
```

打开 [http://localhost:8788/](http://localhost:8788/) → 看到圆屏豆豆模拟器。
打开 [http://localhost:8788/log.html](http://localhost:8788/log.html) → 看审批历史。

默认模式是 `DOUDOU_SOURCE=follow`:Bridge 只读 tail `~/.codex/sessions/.../rollout-*.jsonl`,跟踪 Codex Desktop 在干什么。**不需要 Codex 也能跑** —— 但跟踪流就是空的。要看 demo 模式:

```bash
DOUDOU_CODEX=stub node packages/bridge/dist/cli.js
```

Stub 模式会按 6 秒一次循环推 idle → thinking → executing → done,可以验证 UI 全套状态。

## 3. (可选)接 Codex 审批 hook

让 Codex 的 `PermissionRequest`(运行命令、写文件、网络访问等高风险动作)弹到设备上,你在屏上点同意/拒绝。详见 [esp32-codex-approval-bridge.md](esp32-codex-approval-bridge.md)。

```bash
mkdir -p ~/.codex/esp32-approval
cp scripts/permission_request_hook.py ~/.codex/esp32-approval/
chmod +x ~/.codex/esp32-approval/permission_request_hook.py
# 然后按 docs/esp32-codex-approval-bridge.md 修改 ~/.codex/hooks.json
```

## 4. 真机烧固件

```bash
# 一次性:export ESP-IDF 到当前 shell
. ~/esp/esp-idf/export.sh

cd packages/firmware
cp sdkconfig.local.example sdkconfig.local
# 编辑 sdkconfig.local 填入 Wi-Fi SSID/密码 + Bridge 主机
# Bridge 主机可以是 mDNS 名(默认 doudou-bridge.local)或 LAN IP

idf.py build flash monitor
# 用 USB-C 接板子。第一次烧用 BOOT+RESET 进 DFU 模式
```

烧完串口能看到 boot log → 屏幕亮 → Wi-Fi 连上 → 连 Bridge → 显示豆豆。如果黑屏 / 颜色错 / 触摸不响应,先看 [docs/firmware/esp32-c3-lessons.md](firmware/esp32-c3-lessons.md) 的踩坑列表。

## 5. 不用真硬件改 UI:Wokwi

VSCode 装 "Wokwi for VSCode" 扩展 → 打开 `packages/firmware/diagram.json` → 点 ▶。
浏览器里跑的是真 `doudou.bin`,LCD + 按键全仿真。改完固件代码重新 `idf.py build` 然后重启 Wokwi 就能看效果。详见 [docs/wokwi-guide.md](wokwi-guide.md)。

Wokwi **不仿真** Wi-Fi、BLE、CST816D,所以纯协议/链路验证还得靠真机。

## 6. 切到 BLE 模式(不用 Wi-Fi)

```bash
# 固件端:用 BLE defaults 重新配置
cd packages/firmware
rm sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.ble" reconfigure
idf.py build flash monitor

# Bridge 端:装 noble 然后用 DOUDOU_BLE=1 启动
pnpm --filter @doudou/bridge add @abandonware/noble
DOUDOU_BLE=1 node packages/bridge/dist/cli.js
```

BLE 链路自动配对(LE Secure Connections + Just Works + NVS 持久 bond)。第一次连会做 pair → 写 LTK 到 NVS → 后续重连秒到。详见 [CLAUDE.md](../CLAUDE.md) 的 "Transport: WS vs BLE" 章节。

## 7. 跑测试

```bash
pnpm test              # TS host tests(154 个) + firmware protocol_parse(43 个)
pnpm test:ts           # 只 TS,快迭代
pnpm firmware:test     # 只 firmware,不用 IDF
```

固件 build 验证:`cd packages/firmware && idf.py build`(需要 IDF export 过)。

## 常见问题

**Bridge 启动后浏览器空白 / 模拟器看不到豆豆**
→ 看 Bridge 日志(终端)。多半是端口 8788 被占。试 `DOUDOU_PORT=8789 node packages/bridge/dist/cli.js`。

**Codex Desktop 在跑但豆豆不响应**
→ 默认扫最近 90 天的 rollout。如果你刚装 Codex Desktop 没多久,先在 Desktop 里发一条消息让它生成 rollout 文件。

**真机连不上 Bridge**
→ 确认电脑和板子在同一 LAN(Wi-Fi 5GHz vs 2.4GHz 有的路由器隔离)。`sdkconfig.local` 里的 `CONFIG_DOUDOU_BRIDGE_HOST` 改成 Bridge 主机的 LAN IP(跳过 mDNS,更可靠)。

**触摸完全不响应 / 触摸自己乱触发**
→ 看 [docs/firmware/esp32-c3-lessons.md §2 触摸层](firmware/esp32-c3-lessons.md#2-触摸层-cst816d)。CST816D 的 gesture 寄存器会 latch,我们做了 hysteresis + bus garbage filter,需要确认烧的是最新固件。
