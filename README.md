# Codex Doudou(豆豆)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.3-blue.svg)](https://docs.espressif.com/projects/esp-idf/en/v5.3/esp32c3/index.html)
[![LVGL](https://img.shields.io/badge/LVGL-v9.5-green.svg)](https://docs.lvgl.io/master/)

> A 1.28-inch round ESP32-C3 desktop companion for Codex.
> **Shows** what Codex is doing, **takes** its approval prompts on a touch screen — glance at your desk + tap, no context switch.
>
> 一个 1.28 英寸圆屏 ESP32-C3 桌面伴侣。**显示** Codex 在干嘛、**接收** 它的审批请求。一眼扫桌面 + 点一下屏幕,不用切回 Codex 客户端。

```
Codex Desktop  <──JSON-RPC──>  Doudou Bridge(你的电脑)  <──Wi-Fi/BLE──>  ESP32-C3 圆屏
                                       │
                                       └── 浏览器:http://localhost:8788/(模拟器 + 审批日志)
```

## 当前能力

- ✅ **Bridge + 浏览器模拟器**(完整协议、4 屏 + 滑动、风险色 question)
- ✅ **审批接管**:Codex `PermissionRequest` hook → Bridge → 设备点同意/拒绝
- ✅ **设备协议 v1**:status / question / reply / usage / session_info / thread_list / follow_thread / ack / ping / pong / error / device_status
- ✅ **双 transport**:Wi-Fi + WebSocket(默认)/ BLE GATT(opt-in)
- ✅ **BLE 配对加密**:NimBLE LE Secure Connections + Just Works + NVS 持久 bond
- ✅ **固件 LVGL UI**:分层 sprite 豆豆 + 4 屏 + 触摸手势 + question 二次确认 + 长按气泡
- ✅ **审计日志 + 查看页**:每次审批落盘 + 浏览器查看 `/log.html`
- ✅ **Wokwi 虚拟跑**:浏览器里跑真 firmware,UI 改动秒级反馈
- ✅ **真机点亮**:ESP32-2424S012(C3 + GC9A01 + CST816D)烧录验证,显示 / 触摸 / Wi-Fi / WS / question UI 全跑通
- ⏳ **V1**:TLS、OTA、电池管理、MITM 抗性配对

## 快速开始

完整教程:**[docs/getting-started.md](docs/getting-started.md)**

```bash
# 1. 安装
pnpm install
pnpm -r build

# 2. 跑 Bridge + 浏览器模拟器(不需要硬件)
node packages/bridge/dist/cli.js
# → 浏览器开 http://localhost:8788/  看模拟器
# → 点头部"审批日志 →"看 /log.html

# 3. 真机烧固件(需要 ESP-IDF v5.3)
. ~/esp/esp-idf/export.sh
cd packages/firmware
cp sdkconfig.local.example sdkconfig.local   # 填入 Wi-Fi 凭证(gitignored)
idf.py build flash monitor                   # 或 idf.py menuconfig 走图形菜单

# 4. 切到 BLE 模式(不用 Wi-Fi)
rm sdkconfig
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.ble" reconfigure
idf.py build flash monitor
pnpm --filter @doudou/bridge add @abandonware/noble
DOUDOU_BLE=1 node packages/bridge/dist/cli.js

# 5. 接 Codex 审批 hook
#    cp scripts/permission_request_hook.py ~/.codex/hooks/
#    编辑 ~/.codex/hooks.json,见 docs/esp32-codex-approval-bridge.md
```

## 工作流建议

| 改什么 | 怎么验 |
|---|---|
| Bridge 逻辑 / 协议 | `pnpm test` —— 154 host 单测 |
| Simulator UI(浏览器内) | `pnpm dev` 起 bridge,改保存即生效 |
| 固件 LVGL UI | **Wokwi**(浏览器跑真 .bin):见 [docs/wokwi-guide.md](docs/wokwi-guide.md) |
| 固件协议解析 | `pnpm firmware:test` —— 43 host 单测,不用 IDF |
| 真 Wi-Fi/BLE/触摸 | **真机** + `idf.py monitor`,没替代品 |

## 文档

**入口**

- [Getting Started](docs/getting-started.md) — 装 → 跑 → 烧的完整流程

**主线**

- [技术方案](docs/technical-plan.md) — 总设计 + 里程碑 + 验收标准(中文)
- [设备协议 v1](docs/device-protocol.md) — Bridge ↔ 设备完整 wire 协议
- [风险策略](docs/risk-policy.md) — Codex 动作 → low/medium/high + 二次确认规则
- [Codex app-server 调研](docs/codex-app-server-research.md) — JSON-RPC 事件 / Bridge 必须处理的字段
- [Codex 审批 hook 接入](docs/esp32-codex-approval-bridge.md) — `PermissionRequest` ↔ 豆豆链路

**工程**

- [Claude 工作指引](CLAUDE.md) — 仓库结构、命令、Transport 切换、约束
- [Wokwi 虚拟环境指南](docs/wokwi-guide.md) — 不用硬件改 UI
- [Flash 分区](docs/firmware/partitions.md) — 4MB 布局(CSV + 说明)
- [ESP32-C3 开发踩坑总结](docs/firmware/esp32-c3-lessons.md) — GC9A01 / CST816D / LVGL / WiFi / mDNS / 分区一线经验

**历史/参考**

- [Pet 美术 spec](docs/pet-art-spec.md) — ⚠ 设计探索过程史,实际实现已偏离(顶部 banner 指向真相)
- [ESP32-2424S012 vendor 资料](docs/1.28inch_ESP32-2424S012/) — 板子原厂 demo + 引脚

## 项目结构

pnpm workspace,所有包在 `packages/`:

| 包 | 作用 |
|---|---|
| `device-protocol/` | zod 协议 schema(Bridge + Simulator + 测试共享) |
| `codex-app-server-protocol/` | Codex JSON-RPC TS 类型(自动生成) |
| `bridge/` | Node.js Bridge:WS 服务 + BLE central + Codex sidecar + 审批 hook 入口 |
| `simulator/` | 浏览器模拟器:4 屏豆豆 + 审批日志页 |
| `firmware/` | ESP-IDF C 工程:LVGL + 触摸 + Wi-Fi/BLE transport + question UI |

## 测试

154 host tests + idf.py build 双 transport 绿:

```
device-protocol      8 tests   (zod schemas)
bridge              103 tests   (risk, registry, approval, audit, codexFeed, bridge HTTP, ble/framing, ble/connection, deviceConnection WS, threadListPoller)
firmware host        43 tests   (protocol_parse pure logic, no IDF)
firmware WS  build   1049 KB    (32% flash free)
firmware BLE build   1037 KB    (32% flash free)
```

跑全套:

```bash
pnpm test
```

## 状态

**MVP-1c 在 ESP32-2424S012 真机上跑起来了。** Bridge / Simulator / 协议 / 审批 hook / 审计 / 固件 build + 单测 / LVGL 渲染 / 触摸手势 / Wi-Fi+WS / question UI 全部 OK。下一步是 BLE pair/bond 真机回归、Wi-Fi 重连 soak、TLS 收尾、OTA。详细 TODO 见 [docs/technical-plan.md §开发里程碑](docs/technical-plan.md#开发里程碑)。

## 硬件清单

实测的开发板:**ESP32-2424S012**(淘宝/AliExpress 常见,~¥40-60)

| 部件 | 型号 | 备注 |
|---|---|---|
| MCU | ESP32-C3(RISC-V 单核,160 MHz,400 KB SRAM,4 MB flash) | 板载 USB-C 直连烧录 |
| LCD | GC9A01,1.28 英寸,240×240 圆屏 | SPI,需 invert + BGR + byte-swap |
| 触摸 | CST816D,I²C 0x15,单指 + 手势识别 | gesture 寄存器会 latch,需软件去重 |
| 供电 | USB-C 5V 或 锂电池接口(V1 计划) | MVP 阶段插 USB 用 |

引脚详见 [`packages/firmware/main/pins.h`](packages/firmware/main/pins.h)。其他板子需要重映射 + 改 `sdkconfig.defaults` 里的 LCD 驱动配置。

## 贡献

欢迎 PR。约定:
- 改协议先动 [`docs/device-protocol.md`](docs/device-protocol.md) 和 [`packages/device-protocol/`](packages/device-protocol/) 的 zod schema,Bridge / 固件 / 模拟器一起跟。
- 改架构 / 里程碑先动 [`docs/technical-plan.md`](docs/technical-plan.md),分歧时以那份为准。
- 固件代码遵循 `docs/firmware/esp32-c3-lessons.md` 里写过的硬件坑,别重新踩。
- 提交前跑:`pnpm test && pnpm firmware:test`。改了固件源还要 `cd packages/firmware && idf.py build` 验证。

不要做(per `docs/technical-plan.md` 明确不做):设备直连 OpenAI/Codex、设备上跑业务逻辑、设备显示长文本/diff、用按键当用户输入。

## License

MIT — 见 [LICENSE](LICENSE)。
