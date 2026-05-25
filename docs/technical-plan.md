# Codex Doudou 技术方案

> **命名约定**：本项目对外名称统一为 **Codex Doudou**，中文称 **豆豆**，设备代号 `doudou`。后续文档、代码、commit、UI 文案不再混用 "Project Doudou" 等其他叫法。

## 相关文档

本主方案侧重整体架构和决策。深入细节拆分到以下子文档：

| 文档 | 内容 |
|---|---|
| [codex-app-server-research.md](./codex-app-server-research.md) | Codex app-server JSON-RPC 协议调研、事件清单、Bridge 集成策略 |
| [device-protocol.md](./device-protocol.md) | Bridge ↔ 设备 完整协议 v1（含握手、心跳、ack、错误码、配对流程、审计格式） |
| [risk-policy.md](./risk-policy.md) | Codex 动作到 low/medium/high 的映射规则与二次确认策略 |
| [firmware/partitions.md](./firmware/partitions.md) + [partitions.csv](./firmware/partitions.csv) | 4MB Flash 分区表 + 关键决策 |

## 项目定位

Codex Doudou 是一个基于 1.28 寸圆形 ESP32-C3 触摸屏的 Codex 桌面 **轻外设**。

豆豆是一只活在小圆屏里的"宠物"，**被动观察** 你在 Codex Desktop / CLI / VS Code 插件里的会话：

- **静止时** 通过表情和小动作反映 Codex 当前状态（睡、想、做事、做完、出错），让你"扫一眼桌面就知道 Codex 在干嘛"。
- **被摸时** 会有反应（轻微抖动、眨眼、表情切换），偶尔卖个萌，但不打扰工作。
- **不需要键盘输入** — 设备唯一的人机接口就是触摸。Prompt 输入仍在 Codex 客户端,**但审批**(tool / file / command / network 类权限请求)可以走豆豆——点一下"允许/拒绝"就行,不用回桌面。

一句话定位：

> 豆豆是一只住在小圆屏里的 Codex 宠物。它在你的桌面同步表达 Codex 在干嘛，是桌面上最轻的那一块外设。

### 设计原则

1. **被动观察 + 主动审批**:豆豆默认通过 tail `~/.codex/sessions/.../rollout-*.jsonl` 被动同步 Codex 状态。**审批通道单独走 Codex 官方 `PermissionRequest` hook → Bridge → 豆豆**;详情见 [docs/esp32-codex-approval-bridge.md](./esp32-codex-approval-bridge.md)。
2. **轻外设,不是终端**:豆豆不是 Codex 的浏览器或 CLI 替代品。凡是需要长输入、看长内容、做多步操作的事都在 Desktop / CLI 里做。
3. **单一焦点屏 + 横滑切换**:见 [UI 设计 §三屏布局](#三屏布局)。
4. **看不下的字会动**:标题/正文超出安全区时,**自动跑马灯**(单方向匀速滚动 + 头尾留白),不截断,不换页。
5. ~~**审批暂不接管**~~ **已接管**:Codex 0.128+ 提供了官方 `PermissionRequest` hook(stdin Codex 给 JSON,stdout hook 给决策)。`scripts/permission_request_hook.py` 把 hook 转发到 Bridge `POST /approval/request`,Bridge 用 `risk.ts` 分类后下发 question,等豆豆点完按钮再把 `allow`/`deny` 写回 stdout。无设备 / 超时 → 输出 `{}` 让 Codex 回退原弹窗。

## 硬件条件

当前目标硬件：

- ESP32-2424S012 系列圆形屏开发板
- ESP32-C3-MINI-1U 主控模块
- 1.28 寸圆形 TFT LCD 显示屏
- 触摸屏，目标型号应选择电容触摸版本
- Wi-Fi
- 蓝牙
- LVGL 图形界面支持

硬件定位：

- 适合显示状态、短文本、少量按钮和简单动画。
- 不适合显示长 diff、完整终端日志、大段源码或复杂列表。
- 主交互方式是触摸点击。
- Wi-Fi 作为主要通信链路。
- 蓝牙用于配网、绑定和近场恢复，不作为主链路。

### 硬件详细信息

根据当前图片资料，目标开发板属于 `ESP32-2424S012` 系列。需要优先采购或确认带触摸的型号，例如 `ESP32-2424S012C-I` 或带外壳的 `ESP32-2424S012C-I-Y`。

| 项目 | 参数 |
| --- | --- |
| 主控模块 | ESP32-C3-MINI-1U |
| CPU | ESP32-C3 单核 MCU，最高 160MHz |
| 无线能力 | Wi-Fi + 蓝牙 |
| SRAM | 400KB |
| ROM | 384KB |
| Flash | 4MB |
| 屏幕尺寸 | 1.28 inch TFT |
| 屏幕分辨率 | 240 x 240 |
| 显示颜色 | 16 bit color，RGB565 |
| 屏幕驱动 | GC9A01 |
| 视角 | IPS |
| 可视区域 | 直径约 32.4mm |
| 模块尺寸 | 约 38.5 x 37.0mm |
| 工作电压 | 5V |
| 标称功耗 | 约 100mA |
| 产品重量 | 约 20g |
| 工作温度 | -20°C 到 70°C |
| 储存温度 | -30°C 到 80°C |
| 开发支持 | Arduino IDE、MicroPython、Mixly、Scratch 3.0 |

板载结构信息：

- USB Type-C 接口，用于供电、下载和调试。
- RESET 按键和 BOOT 按键。
- 侧边电源开关。
- JST1.25 2P 锂电池接口，支持充电和放电，并带过充、过流保护。
- SH1.0 4P 扩展接口，可作为后续外设扩展口。
- 板上包含屏幕、背光控制电路和 IO 接口。

### 验证后的引脚表(vendor 资料抄录)

来自 `docs/1.28inch_ESP32-2424S012/1-Demo/Demo_Arduino/3_1-TFT-LVGL-Benchmark/` 的官方 Arduino sample,与原理图一致:

| 信号 | GPIO | 备注 |
|---|---:|---|
| LCD SCLK | 6 | SPI2_HOST,80MHz |
| LCD MOSI | 7 |  |
| LCD DC | 2 |  |
| LCD CS | 10 |  |
| LCD RST | — | PCB 直连,无 MCU 引脚 |
| LCD BL | 3 | 简单 GPIO 拉高使能,vendor demo 不做 PWM |
| Touch SDA | 4 | **CST816D** @ I2C 0x15 |
| Touch SCL | 5 |  |
| Touch RST | 1 |  |
| Touch INT | 0 |  |

LCD 面板需要 `invert_color = true` + BGR 像素序(rgb_order=false in LovyanGFX 即 BGR)。

仍待向卖家或原理图确认(非阻塞):

- SH1.0 4P 扩展口实际引脚定义
- 板载电池充电管理芯片型号及可读状态引脚
- 是否预留蜂鸣器/震动/用户 LED

### 硬件对方案的影响

- 4MB Flash 足够承载单一主题 UI、少量字体、基础图标和 OTA 分区，但不适合放大量帧动画。
- 400KB SRAM 在开启 Wi-Fi、TLS、JSON 解析和 LVGL 后会比较紧张，不能使用全屏双缓冲。
- 240 x 240 RGB565 单帧缓冲约 112.5KB，建议使用 20 到 40 行 partial buffer。
- 圆形屏幕四角不可见，UI 需要保留圆形安全边距，避免把按钮、状态和关键文字放到角落。
- 当前硬件未明确包含蜂鸣器或震动马达，P0 提醒应以背光亮起、屏幕动画和高对比状态页为主。
- BOOT/RESET 属于下载和复位用途，不建议作为日常用户交互按钮；日常交互应以触摸屏为主。

### 硬件约束下的方案调整

基于 ESP32-C3、400KB SRAM、4MB Flash 和 240 x 240 圆屏，豆豆应定位为极轻量触摸终端，而不是完整 Codex 客户端。

调整后的职责边界：

```text
豆豆设备：状态展示、触摸选择、短消息提醒、回复上报
Doudou Bridge：Codex 集成、事件归一、摘要裁剪、风险判断、审计日志
Codex：真实任务执行、上下文管理、工具调用和审批处理
```

具体调整：

- MVP 阶段使用局域网明文 WebSocket，避免 TLS 带来的 heap 峰值压力。
- 设备端不保存 Codex 凭据、OpenAI API key、完整上下文或长文本历史。
- 设备端只维护当前状态、当前问题和少量配置。
- 所有复杂文本、日志、diff、源码片段都由 Bridge 摘要或裁剪。
- 高风险判断不放在 ESP32 上做，由 Bridge 输出明确的风险等级和二次确认要求。
- OTA 放到 V1，MVP 只保留分区规划和后续升级路径。

## 目标能力

### P0 能力

**布局是 4 屏横滑**:`INFO ← USAGE ← PET → HISTORY`,默认在中间 PET 屏。

1. **PET(主屏):宠物 + 当前会话标题**
   - 表情 / 小动画反映 Codex 当前状态(idle / thinking / executing / waiting / done / error)
   - 触摸有反馈(轻抖、眨眼、长按弹气泡)
   - 宠物下方一行 **当前 Codex 会话标题**(AI 生成的 thread name,过长跑马灯)
   - 底部小字 = 当前 state 描述
   - 数据源:Bridge `RolloutWatcher` 推 `status`,`ThreadListPoller` 推 `session_info.thread_title`

2. **USAGE(左滑):用量 + 多模型配额**
   - **上下文剩余**:`current_context_tokens` / `model_context_window`,绿/黄/红
   - 本会话累计:total / input / output / cached tokens
   - **多模型组**配额剩余(主模型 + Spark 等,每组独立 5h / 7d 进度条 + 重置倒计时)
   - 数据源:rollout `event_msg.token_count`(上下文 + 会话累计)、`ThreadListPoller` 30s 调 `account/rateLimits/read`(`rateLimitsByLimitId` 多组)

3. **INFO(最左滑):会话元数据**
   - 会话标题(跑马灯)+ 邮箱 / PLAN / 协作模式
   - 模型 / reasoning effort / summary 模式
   - 工作目录 + Permissions + AGENTS.md 检测
   - 来源 + session id
   - 数据源:`session_meta` + `turn_context`(rollout)+ `account/read`(Bridge 启动一次)+ 文件存在性检查

4. **HISTORY(右滑):会话列表**
   - 最近 8 条 Codex 会话(标题 / 来源 / 相对时间)
   - 当前跟随的会话用红点 + 加粗
   - **点击其他会话** → 设备发 `follow_thread` → Bridge 切换 RolloutWatcher 到那个 rollout 文件

5. **横滑导航 + 跑马灯**:鼠标拖 / ← → / 点圆点 / 触摸滑;文字超出自动单方向匀速滚动

6. **mDNS 发现**:Bridge 通告 `_doudou._tcp.local`,真机零配置发现

7. **重连恢复**:断网 / Bridge 重启后设备自动重连,DeviceState 持久化未过期 question 跨连接重发(MVP 阶段不会触发,留协议给 V1)

### P0 暂不做(等 codex 升级)

- ~~**审批接管**~~ **已完成**:走 Codex `PermissionRequest` hook 而不是 rollout fan-out。`question` / `reply` / `ack` / `require_confirm` / `risk` 协议字段全部在用,固件 question UI(`pet_ui.c` 的 `doudou_pet_show_question`)支持 1-4 选项 + 风险色 + `require_confirm` 二次确认 + `queue_total` 角标。完整链路文档见 [esp32-codex-approval-bridge.md](./esp32-codex-approval-bridge.md)。
- **从豆豆发 prompt**:需要 codex 多客户端 API,目前没有。MVP 不在 UI 暴露(代码保留 `DOUDOU_SOURCE=own` 作为测试入口)。
- **多设备扇出 / Bridge fan-out**:Bridge 已支持挂多个设备,但首版只面向单台豆豆。
- **基础配置(Wi-Fi 配网 / 亮度 / 休眠)**:MVP-3 阶段,见 [device-protocol.md §7](./device-protocol.md#7-配对与首次绑定)

### P1 能力

- 宠物自定义皮肤
- 多设备绑定(单 Bridge 对多豆豆 fan-out)
- 本地配置页面(`127.0.0.1:8787`)
- 审计导出 / 查询
- 可配置通知规则(按 risk / action_type 过滤)
- OTA 固件升级

### 明确不做

MVP 阶段不做以下能力：

- 在设备上浏览源码、diff 或完整终端输出。
- 在设备上输入长文本。
- 在设备上选择 workspace、模型或复杂 Codex 配置。
- 通过公网直接访问 Codex 或 OpenAI API。
- 在 ESP32 上实现 Codex 事件的复杂业务判断。

## 总体架构

推荐采用本地 Bridge 架构，ESP32 不直接连接 Codex。

```text
Codex CLI (codex app-server, stdio JSON-RPC)
   ↑
   │ 子进程 + JSONL stdio
   ↓
Local Doudou Bridge  (Node.js / TypeScript)
   ↑
   │ ws://<bridge>:8788/device     (LAN 明文，V1 升级到 wss)
   ↓
ESP32-C3 圆形触摸屏  (ESP-IDF + LVGL)
```

设计原则：

- ESP32-C3 只负责展示、触摸输入和轻量消息收发。
- Doudou Bridge 运行在用户电脑上，负责连接 Codex、协议转换、鉴权、超时和日志。
- Codex 凭据、OpenAI API key、完整上下文都不下发到 ESP32。
- 豆豆只接收经过摘要和裁剪的小消息。

### Bridge 部署环境约束

Bridge 与用户电脑生命周期绑定，方案 **不承诺** 在电脑离线时设备仍可用：

- 笔记本休眠 / 合盖 → 豆豆显示 `bridge_disconnected` 错误页。
- 用户切换网络（公司 ↔ 家） → 设备 IP 变化，依赖 mDNS（`doudou-bridge.local`）重新发现 Bridge；mDNS 失败时回到配网模式让用户重输 Bridge URL。
- Bridge 与豆豆 **必须在同一二层网络**。跨子网 / 跨 VLAN 暂不支持，V1 之后考虑通过 Bridge 公网中继。
- 单台 Bridge 同时绑定多设备的上限：MVP **8 台**（受 Bridge Node.js 单进程 WS 句柄数和路由表大小约束）。

### 多 Codex 会话策略

用户可能同时开多个 Codex thread（不同项目）。MVP 阶段：

- Bridge 维护 **所有 active thread 的事件流**，但 **同一时刻只对豆豆展示一个 thread**（防止状态跳变）。
- 选择策略按优先级：
  1. 有 pending `requestApproval` 的 thread（用户最关心）
  2. 最近 30 秒内有 `turn/started` 的 thread（活跃中）
  3. 最近收到事件的 thread
- 多个 thread 同时有审批：豆豆 question 体下方加 "还有 N 个待审批" 角标，用户回复完当前的，自动切到下一个。
- 用户可在 Bridge 配置页临时 "钉住" 某个 thread，阻止自动切换。

V1 阶段在豆豆侧引入会话切换 UI（左右滑动），但 schema 已在 MVP 协议预留 `session_id` 字段。

## Doudou Bridge

Doudou Bridge 是本项目的核心中间层，建议使用 Node.js 或 Rust 实现。

### 职责

1. 启动或连接 `codex app-server`。
2. 监听 Codex 线程、回合、工具调用、审批和用户输入请求。
3. 将 Codex 复杂事件转换成豆豆设备协议。
4. 接收豆豆的触摸回复，并回写给 Codex。
5. 处理设备鉴权、消息去重、超时和错误恢复。
6. 提供本地配置页面，例如 `http://127.0.0.1:8787`。

### Bridge 需要承担的智能

由于 ESP32-C3 资源有限，Bridge 应承担更多决策和转换工作：

- Codex 事件归一化：把 app-server 的 `turn/*`、`item/*`、`*/requestApproval` 等统一成设备协议的 `status` / `question` / `error`（事件清单见 [codex-app-server-research.md](./codex-app-server-research.md)）。
- 文本摘要：把长消息压缩成适合圆屏的标题和正文。
- 风险分级：识别运行命令、修改文件、访问网络等高风险动作，详细规则见 [risk-policy.md](./risk-policy.md)。
- 二次确认策略：对 `risk: high` 的 question 强制 `require_confirm: true`。
- 请求过期：超时未回复时根据风险等级自动 `decline`（low/medium）或 `cancel`（high）整个 turn。
- 回复幂等：同一个 `request_id` 多次回复只处理一次。
- 设备在线状态：记录豆豆在线、离线和最后心跳时间。
- 审计日志：记录设备确认的请求、选项、时间和 Bridge 处理结果。Bridge 端为权威副本，设备端 audit 分区为 fail-safe。

#### 审计日志（Bridge 端）

Bridge 端审计是 **结构化 JSONL**，按日期切分文件，存放在 `$XDG_DATA_HOME/doudou/audit/YYYY-MM-DD.jsonl`：

```json
{"ts":"2026-05-21T10:23:11.412Z","device":"doudou-001","thread":"thr_x","request_id":"req_123","action_type":"run_command","risk":"high","command":"git push origin main","choice":"accept","latency_ms":1820,"codex_response":"acceptForSession"}
```

- 字段稳定，新增字段向后兼容。
- 单文件 > 10 MB 自动 rotate。
- 保留期默认 90 天，可在配置页调整。
- 提供 `doudou-bridge audit query --device doudou-001 --since 2026-05-01` CLI 查询。

### 事件处理

Bridge 重点关注这些事件类型：

- Codex 当前任务状态变化
- Codex 请求用户输入
- Codex 请求命令审批
- Codex 请求文件修改审批
- Codex 回合完成
- Codex 回合失败

Bridge 不应该把完整上下文直接推给设备，而应生成适合 1.28 寸圆屏显示的短消息。

## ESP32 固件

### 推荐技术栈与版本

| 组件 | 版本/选型 | 理由 |
|---|---|---|
| ESP-IDF | v5.3 LTS | C3 支持完备，5.x 系列 IDF Component Registry 成熟 |
| FreeRTOS | 随 IDF 自带 | 不替换 |
| LVGL | **v9.2** | API 较 v8 更稳定，partial buffer 配置更直观；与 IDF v5 集成有官方 component |
| LCD 驱动 | `esp_lcd_panel_gc9a01`（IDF managed component） | 厂商驱动，免手写 SPI |
| Touch 驱动 | **CST816D @ I2C 0x15** | Hynitron 电容触摸,IDF v5.3 `driver/i2c_master.h` 直接驱动,已实现 |
| WebSocket | `esp_websocket_client` v1.3+（managed component） | 官方维护，支持 LWS 不需要 |
| JSON | **cJSON**（IDF 自带） | 内存占用比 ArduinoJson 可控；不用 nlohmann 因 C++ 异常开销 |
| BLE（配网用） | NimBLE | 比 Bluedroid 内存占用减半，C3 推荐 |
| 文件系统 | SPIFFS（assets） + NVS（config + audit） | 见 [partitions.md](./firmware/partitions.md) |
| OTA | `esp_https_ota`（V1 启用） | MVP 不启用但分区已预留 |



### 任务划分

```text
ui_task          LVGL 渲染和触摸处理
network_task     WebSocket/MQTT 连接
message_task     JSON 消息解析和状态机
storage_task     NVS 配置保存
ota_task         固件升级
```

MVP 阶段可以先不启用 `ota_task`，但分区表和固件结构应预留升级空间。

### LVGL 注意事项

ESP32-C3 资源有限，LVGL 需要控制内存使用：

- 使用 partial buffer，不做全屏双缓冲。
- 建议缓冲区从 `240 x 20 x 2` 或 `240 x 40 x 2` 起步调试。
- 字体数量保持克制。
- 动画使用简单矢量、颜色变化或少量帧动画。
- 图片资源使用小尺寸 indexed color 或压缩格式。
- 页面结构保持简单，避免复杂嵌套控件。
- 圆形屏幕需要设置安全边距，关键按钮和文字避开四角区域。
- 如果使用 TLS WebSocket，需要额外评估 heap 峰值，必要时先用局域网明文 WebSocket 完成 MVP。

### 驱动适配

显示驱动优先按 `GC9A01` 适配，数据格式使用 RGB565。触摸驱动需要等供应商示例工程或原理图确认后再定，常见路径是：

1. 从卖家 Arduino 示例中提取 LCD、触摸、背光和电源相关引脚。
2. 在 ESP-IDF 中先跑通 GC9A01 全屏填色、色块和旋转方向。
3. 接入 LVGL display flush。
4. 接入触摸 read callback。
5. 校准触摸坐标和圆屏可点击区域。
6. 加入背光亮度控制和自动息屏。

### 固件 bring-up 顺序

固件开发应先验证硬件闭环，再接入 Codex 业务。

1. 跑通串口日志、供电和下载流程。
2. 跑通 GC9A01 显示驱动，完成全屏填色、色块和旋转方向验证。
3. 接入 LVGL flush，显示静态页面。
4. 接入触摸驱动，完成坐标读取和点击区域验证。
5. 接入背光控制，支持亮屏、息屏和亮度设置。
6. 接入 Wi-Fi STA，完成断线重连。
7. 接入 WebSocket client，完成心跳和简单消息收发。
8. 接入设备协议，显示 `status` 和 `question`。
9. 接入触摸回复，支持确认页和请求过期。
10. 做 heap、栈、水位和长时间运行测试。

## 通信方案

> **协议细节**已迁移到 [device-protocol.md](./device-protocol.md)。本节只保留链路层选型理由。

MVP 推荐使用局域网明文 WebSocket：

```text
ws://<bridge-ip>:8788/device
```

后续正式版本支持：

```text
wss://<bridge-ip>:8788/device
```

采用明文 WebSocket 的原因：

- ESP32-C3 同时运行 LVGL、Wi-Fi、JSON 和 TLS 时内存压力较大。
- MVP 目标是先打通本地闭环，安全边界由局域网、配对 token、请求过期和 Bridge 不暴露公网承担。
- V1 阶段再引入 TLS，并通过 heap 压测确认稳定性。

通信方案优先级：

1. WebSocket over Wi-Fi：开发简单，适合本地 Bridge。
2. MQTT over Wi-Fi：适合后续多设备、多主机或远程中继。
3. BLE：用于配网和绑定，不作为主通信链路。

### 消息大小限制

为降低 ESP32-C3 JSON 解析和 UI 渲染压力，设备协议需要限制 payload：

- 单条消息建议小于 1KB。
- `title` 建议不超过 20 字节到 30 字节，中文约 10 个字。
- `body` 建议不超过 150 字节，中文约 48 个字。
- `choices` 不超过 4 个。
- 单个 `choice.label` 不超过 8 个中文字符。
- 不下发源码、diff、完整日志或长 Markdown。
- 长内容由 Bridge 提供摘要，并提示用户回到电脑查看。

## 设备协议

> 完整协议定义见 [device-protocol.md](./device-protocol.md)（含握手 `hello`/`welcome`、心跳 `ping`/`pong`、`ack` 机制、错误码枚举、配对流程、审计格式）。这里只保留方案级摘要，便于阅读架构时不必跳转。

四类核心消息（v1，外层固定字段 `v`/`type`/`seq`/`ts` 此处省略）：

```json
// Bridge → 设备
{ "type": "status",   "state": "thinking", "title": "Codex 正在处理", "body": "正在分析项目结构" }
{ "type": "question", "id": "req_123", "risk": "high", "action_type": "run_command",
  "title": "运行 npm test?", "body": "Codex 想在项目根目录执行测试",
  "choices": [{"id":"accept","label":"同意"},{"id":"decline","label":"拒绝"}],
  "expires_at": 1779345600000, "require_confirm": true }
{ "type": "error", "code": "request_expired", "title": "请求已过期", "body": "请回到电脑继续处理" }

// 设备 → Bridge
{ "type": "reply", "id": "req_123", "choice_id": "accept", "device_id": "doudou-001" }
```

新增字段（相对原版方案）：
- `v` 协议版本，向前兼容靠未知字段忽略。
- `seq` + `ack` 保证关键消息送达，弱网下不丢 question。
- `risk` + `action_type` + `require_confirm` 支撑风险分级 UI（规则见 [risk-policy.md](./risk-policy.md)）。
- 心跳、设备上行 `device_status`、配对 `hello`、9 种 error code 见 device-protocol.md。

## UI 设计

### 三屏布局

豆豆有 **三屏**,通过横滑切换。**主屏(中)始终是宠物 + 当前状态**,任何时候被 Codex 找(question 来了)都强制回主屏。

```
   ┌─────────┐       ┌─────────┐       ┌─────────┐
   │  USAGE  │ ←滑──│   PET   │──滑→ │ HISTORY │
   │ token   │       │ 状态/   │       │ 最近    │
   │ quota   │       │ question│       │ 决策    │
   └─────────┘       └─────────┘       └─────────┘
       •               ●  •  •           •  •  ●     ← 底部 3 个小点
```

屏间转场:`transform: translateX` 软滑,跟手指走,松手吸附最近屏。圆屏底部留 `• • •` 三个点指示当前位置。

### 主屏:宠物 + 状态 + question

#### 宠物表情态

宠物根据 Codex 状态切换表情和小动作。MVP 用 emoji + 简单动画占位,V1 换矢量绘制 sprite。

| Codex 状态 | 宠物表情 | 动作 |
|---|---|---|
| `idle` | 🫘 闭眼睡 | 轻微浮动呼吸 |
| `thinking` | 🤔 抬头思考 | 转头 + 小问号气泡 |
| `executing` | ⚙️ 干活 | 头顶转动小齿轮 |
| `waiting_input` / `waiting_approval` | 👀 看着你 | 眨眼 + 微微前倾 |
| `done` | ✨ 微笑 | 一次性小欢呼,3 秒后回 idle |
| `error` | 😖 委屈 | 摇头 + 一次,3 秒后回 idle |

#### 触摸反应

宠物自身是可触摸的(全屏 hit area)。**触摸不上报到 Bridge**(避免发明出 Codex 不知道怎么处理的事件),只在本地播放一个小反应:

- 短按:宠物轻微弹一下 + 当前表情眨眼
- 长按 > 1s:展开一句"我在呢"气泡(纯本地)

#### question 弹出

收到 `question` 时,宠物淡出到背景(半透明),前台覆盖审批页:

```
┌─────────────────────────┐
│  HIGH · 运行命令          │   ← 顶部 risk + action_type
│                           │
│  执行 rm -rf?             │   ← 标题(过长 marquee)
│  Codex 想清理 build...   │   ← 正文(过长 marquee)
│                           │
│  +2 more                  │   ← 队列角标(可选)
│                           │
│  ┌──────┐    ┌──────┐    │
│  │ 拒绝 │    │ 同意 │    │
│  └──────┘    └──────┘    │
└─────────────────────────┘
```

回复后(或超时后)审批页退出,宠物淡回,继续显示状态;如果队列里还有问题,**自动弹下一个**(0.5s 衔接)。

### 左屏:用量与配额

```
┌─────────────────────────┐
│      📊 用量              │
│                           │
│  本会话                   │
│   ▍34,431 in / 11 out    │
│   ▍cached: 6,528         │
│                           │
│  5h 窗口   ████░░░ 1%    │
│  7d 窗口   ███████ 7%    │
│  Pro plan · 重置 4h12m   │
└─────────────────────────┘
```

数据来自 Bridge 透传的 `usage` 消息(基于 Codex `tokenUsage` + `rateLimits` notif 聚合)。

### 右屏:决策历史

```
┌─────────────────────────┐
│      📜 最近决策          │
│                           │
│  ● git push origin main   │ ← 红 = high
│    ✓ 同意   12:31         │
│  ● npm install lodash     │ ← 黄 = medium
│    ✓ 同意   12:18         │
│  ● git status             │ ← 绿 = low
│    ✓ 同意   12:15         │
│  ● rm -rf build           │ ← 红
│    ✗ 拒绝   12:10         │
└─────────────────────────┘
```

数据来自 Bridge `/api/recent?n=8`(读取审计 JSONL 的最后 N 行)。设备每 10s 拉一次,或者收到新审计事件时主动 push。

### 圆屏视觉规范

(沿用 240×240 圆屏的硬约束 — 安全边距 ≥ 18px、触摸热区 ≥ 44px、四角不可见。)

- 圆形安全边距 18–24px,任何文字/按钮内嵌于内接圆中
- 三屏共享同一底色,转场时整块滑动,不做窗体
- 高对比色 + emoji/icon 不仅靠颜色传达状态(色盲友好)
- 高风险确认按钮 **不预选**,避免误触
- 待机后亮度自动 ↓ 30%,触摸或新事件唤醒到 100%

### 跑马灯规则

任何文字段超过单行可用宽度(主屏正文 ~180px)时,**自动启用 marquee**:

- 完整渲染一次后,从屏外右侧匀速进,出左侧,头尾各留 24px 空白
- 速度:60 px/s(中文每秒约 2–3 字)
- 鼠标(触摸)悬停时暂停
- 不超过 1 行,**不换行**,避免高度跳动

### 队列行为

设备本地维护待答 question 队列:

- 当前有 question 在显示时,新 question **入队**,不打断当前
- 当前 question 回复 / 超时后,从队列头取下一条 + 自动横滑回主屏(如果在其他屏)
- 队列 > 0 时,审批页右下角显示 `+N` 角标(灰色小字)
- 协议侧可选 `question.queue_total` 由 Bridge 告知"我这边一共多少条",设备据此渲染角标更准确(否则用本地计数)
- 设备无法感知"队列里下一条是低还是高风险" — 完全跟着 Bridge 推过来的顺序走

## 安全策略

1. 豆豆不保存 OpenAI API key。Bridge 也不持有 — 直接复用本机 `codex` CLI 已登录态。
2. Codex app-server 通过 stdio 子进程拉起，**没有任何网络端口**，不存在被外部访问的可能。
3. 豆豆和 Bridge 通过 pairing token 绑定，token 256-bit 随机，存设备 NVS。
4. **首次配对禁止走 LAN 明文** — token 通过 BLE GATT（或 SoftAP 临时通道）交换，再用 6 位短验证码做双向确认防 MITM。完整流程见 [device-protocol.md §7 配对与首次绑定](./device-protocol.md#7-配对与首次绑定)。
5. 每条审批消息必须有 `expires_at`，超时 Bridge 主动 `decline` / `cancel`。
6. 每条回复必须带 `request_id`，Bridge 做幂等处理。
7. 高风险操作必须显示动作类型（`action_type` 字段），分级标准见 [risk-policy.md](./risk-policy.md)。
8. Bridge 记录所有来自豆豆的确认操作，结构化 JSONL 落盘（格式见上节"审计日志"）。
9. MVP 阶段 Bridge 默认 bind `0.0.0.0:8788`（LAN），可在配置页限制到单网卡；**不允许自动暴露到公网**（不会调 UPnP / 不主动开端口映射）。
10. 启用明文 WebSocket 时必须有效配对 token，**且** Bridge 检查 client IP 在 RFC1918 私有段；公网 IP 直接拒接。
11. Pairing token 可在 Bridge 本地配置页重置；重置后所有已绑定设备需重走配对流程。

## 开发里程碑

每个里程碑给出 **可量化验收标准**（Acceptance Criteria, AC）。AC 不全部通过不进入下一阶段。

**状态总览**(2026-05-22):

| Milestone | 软件实现 | 真机验证 |
|---|:---:|:---:|
| MVP-0  Bridge + 模拟器 | ✅ | — (无需硬件) |
| MVP-1a 显示链路 | ✅ build | ⏳ 待真机 |
| MVP-1b 网络链路(WS) | ✅ build | ⏳ 待真机 |
| MVP-1c 触摸输入 | ✅ build | ⏳ 待真机(AC-1c.1~3) |
| MVP-2  触摸回复 + 二次确认 | ✅ | ⏳ 待真机(AC-2.1~4) |
| MVP-3  设备体验 + 配网 | ⚠️ BLE transport 完成,但**配网 UI / SoftAP / 息屏 / NVS 配置**未实现 | ⏳ |
| V1     稳定版 | ❌ | — |

### MVP-0:Bridge + 网页模拟器 ✅

任务：
- 实现 Doudou Bridge 基础服务（Node.js + TypeScript）。
- Bridge 启动 `codex app-server` 子进程，完成 `initialize` 握手。
- 实现设备协议 v1 全部 8 种消息类型（hello/welcome/status/question/reply/ack/error/ping/pong）。
- 网页模拟器（一个浏览器页面）连 `ws://localhost:8788/device`，**走与真机完全相同的协议**，不开任何特权调试端点。
- 实现风险分级映射（[risk-policy.md](./risk-policy.md)）。
- 实现 Bridge 端审计 JSONL 落盘。

验收：
- AC-0.1 能在真实 Codex 会话中触发一次 `git status`（low risk）和一次 `rm -rf` 提案（high risk），网页上分别看到 low/high 两种 question 渲染。
- AC-0.2 高风险 question 出现二次确认页；超时未点击，Codex 端收到 `cancel`。
- AC-0.3 模拟器关闭 5 秒重连，未过期的 in-flight question 自动补发；已过期的展示 `request_expired`。
- AC-0.4 同一 question 连续点 5 次，Codex 只收到一次 response。
- AC-0.5 Bridge 在 mac/Linux/Windows 三平台启动正常。

### MVP-1a:显示链路(不涉及网络) ✅ build,⏳ 真机回归

任务：跑通屏到 LVGL 的全链路。

- 串口日志、供电、下载流程。
- GC9A01 全屏填色、色块、旋转方向。
- LVGL v9 flush 接入，显示静态页面（待机 / 思考 / 等待 / 错误共 4 张静态图）。
- 背光 PWM 控制，亮灭和亮度调节。
- 完成 [partitions.csv](./firmware/partitions.csv) 烧录验证。

验收：
- AC-1a.1 4 张静态页面圆屏正确显示，色彩、字体、安全边距均符合 UI 规范。
- AC-1a.2 背光从 10% 到 100% 五档可调，无可见闪烁。
- AC-1a.3 连续显示 8 小时无重启、无残影、free heap 稳态 ≥ 60 KB。
- AC-1a.4 镜像大小 ≤ 1.3 MB，留 200 KB 给后续 BLE / WS 接入。

### MVP-1b:网络链路 ✅ build,⏳ 真机回归

任务：在 1a 的固件上接入 Wi-Fi 和 WebSocket。

- Wi-Fi STA 接入，断线自动重连，回退超时 30s。
- `esp_websocket_client` 接入，与 Bridge 完成 hello/welcome 握手。
- 心跳 `ping`/`pong`，超时 30s 主动重连。
- 接收 `status` 消息并切换静态页面（无 question 处理）。

验收：
- AC-1b.1 Wi-Fi 拔掉路由器 5 分钟后恢复，设备 30 秒内自动重连并继续收 status。
- AC-1b.2 Bridge 主动 kill 后再启动，设备 60 秒内重连。
- AC-1b.3 连续运行 24 小时，free heap 漂移 ≤ 10 KB，无重启。
- AC-1b.4 Wi-Fi RSSI 在 -75 dBm 弱信号下仍能完成 status 收发（< 5% 重传率）。

### MVP-1c:触摸输入 ✅ build,⏳ 真机回归

任务：接入触摸驱动 — **已完成**(CST816D 在 MVP-1a 步骤 2 一并实现,引脚 vendor 验证)。

- ✅ 触摸坐标读取(I2C 0x15)
- ✅ 圆屏可点击区域:LVGL 自动按 widget 边界过滤,角落区域无 widget
- ✅ LVGL input device 接入(lv_indev_create POINTER + read_cb)
- ✅ 手势识别(单击 / 双击 / 长按 / 四向滑动)由 CST816D 芯片自带

验收:
- AC-1c.1 屏中央 44px×44px 圆形按钮,点击识别率 ≥ 98%(100 次测试)— **待真机验证**
- AC-1c.2 圆屏四角区域点击不触发任何事件 — **待真机验证**
- AC-1c.3 触摸响应延迟 ≤ 80 ms — **待真机验证**(代码侧轮询 20ms + LVGL 5ms tick,理论 ≤ 30ms)

### MVP-2:触摸回复 ✅

任务：把 1c 的触摸接到协议层。

- 接收 `question` 消息，渲染 1/2/3-4 个选项的三种页面模板。
- 触摸选择后构造 `reply` 上报。
- 高风险 question 走二次确认页。
- 请求过期本地计时（基于 welcome 的时间锚）。

验收：
- AC-2.1 question 从 Bridge 发出到屏幕可点击 ≤ 500 ms。
- AC-2.2 高风险 question 必须经过二次确认页才能 reply，误触率 < 1%（100 次随机点击测试）。
- AC-2.3 网络抖动场景下（人为丢包 30%），reply 通过 ack 重试机制最终送达，无重复 reply。
- AC-2.4 question 超时后屏幕展示 `request_expired`，且 5 秒后自动回到 status 页。

### MVP-3:设备体验 + 配网 ⚠️ 部分完成

任务:让设备从开箱到可用,不需要刷固件。

- ⚠️ ~~BLE GATT 配网~~ **改方向**:BLE 直接作为 **主 transport**(替代 Wi-Fi 的数据通道),配网 UI 尚未做。见 [esp32-codex-approval-bridge.md](./esp32-codex-approval-bridge.md) 以及 CLAUDE.md 的 "Transport: WS vs BLE" 一节。
- ⏳ pairing token 6 位验证码双向确认 —— 当前 BLE 走 Just Works(unauthenticated),token 在 hello envelope 里走,未做 6 位 numeric comparison。
- ✅ 待机 / 思考 / 等待 / 完成 / 错误页的表情 —— 分层 sprite 实现,见 `packages/firmware/main/pet_ui.c`,资源 ~146 KB(超出原 5KB/state 预算,但总 flash 余量 ≥ 32% OK)
- ⏳ 自动息屏(默认 60s 无交互 + 无新消息),触摸唤醒
- ⏳ NVS 中保存配置(亮度、息屏时间、Bridge URL)
- ✅ 预留 OTA 分区(已在 1a 完成),OTA 客户端仍未实现

验收：
- AC-3.1 全新设备从上电到能收到第一条 status，用户操作步数 ≤ 5（开机 → BLE 配对 → 输 Wi-Fi → 验证码确认 → 完成）。
- AC-3.2 息屏后任意触摸唤醒延迟 ≤ 200 ms。
- AC-3.3 BLE 配网失败可降级 SoftAP，整体配网耗时 ≤ 3 分钟。
- AC-3.4 解绑后设备自动回配网模式，不需要重新刷固件。
- AC-3.5 电池续航：背光 30% + 每分钟 1 条 status 场景下，1000 mAh 电池续航 ≥ 6 小时（息屏后 ≥ 24 小时）。

### V1:稳定版 ❌ 未启动

- ⏳ 多 Codex 会话切换 UI(设备侧滑动) —— 协议 / Bridge / 模拟器已支持 thread_list + follow_thread,设备端 4 屏支持滑动到 HISTORY 屏点 thread。**单设备多会话切换可用**。
- ⏳ 多设备绑定(同一 Bridge 支持 8 台) —— Bridge `DeviceRegistry` 已经支持挂多个,缺 UX。
- ✅ 审计查询页(浏览器):`/log.html` 已实现(2026-05),~~原 `127.0.0.1:8787` 本地配置页~~ 改方向到现有 simulator 站点 sibling 路径。
- ⏳ 风险策略编辑、token 重置、设备管理 UI
- ⏳ 审批审计导出(CSV / JSONL) —— JSONL 已落盘,缺导出 UI
- ⏳ 可配置通知规则(按 thread / risk / action_type 过滤是否推送到设备)
- ⏳ OTA 固件升级(`esp_https_ota`,资源 OTA 独立通道)
- ⏳ TLS:WebSocket 升级到 `wss://`,证书由 Bridge 自签 + 配对时下发到设备 NVS
- ⏳ BLE MITM 抗性配对(numeric comparison 6 位码 → 需要设备 LCD 显示)

V1 阶段需要单独立项重新走 AC 流程。

## 技术风险

1. Codex app-server 能力和 schema 可能随版本变化,Bridge 需要做协议适配层 —— **当前**:已用 `pnpm regen:codex-bindings` 自动从 `codex app-server generate-ts` 生成 500+ 类型,版本漂移由 codex CLI 自身处理。
2. 1.28 寸屏幕不适合复杂信息,必须坚持"摘要 + 操作" —— **当前**:question/usage/history 均做了字符上限 + marquee 兜底。
3. ESP32-C3 内存有限,LVGL 主题、字体、动画都要克制 —— **当前**:LVGL heap 48KB,固件总 ~1MB / ROM 余 32%。
4. 审批操作存在误触风险,高风险动作必须二次确认 —— **已实现**:`require_confirm` 二阶段在固件 + 模拟器双端齐备。
5. 局域网环境可能不稳定,需要断线重连和请求过期机制 —— **已实现**:WS 心跳 + Bridge 端 inflight 过期 + 重连时 last-status 重发。
6. ~~触摸芯片和引脚表未从图片中明确给出~~ **已解除** —— vendor 资料确认 CST816D + 完整引脚表,固件已实现并编译通过(2026-05-22)。
7. ⏳ **BLE 链路真机未验** —— 软件全套就绪(NimBLE peripheral + 分包协议 + bond NVS + Bridge noble 中心 + 配对加密),但所有 idf.py build 都没烧过真板子。MTU 协商、Just Works 配对、bond 持久化、断连恢复全是未知数。Phase 5 必做。
8. ⏳ **设备开机 1970 年问题** —— `format_local_reset` 依赖系统时间;welcome 帧来之前 USAGE 屏的"今天 X:Y"会显示成 1970 年。已加 `s_clock_synced` 兜底,但首屏体验仍有 < 3s 错位窗口。

## 参考方向

- Codex app-server：用于深度集成 Codex 线程、回合、审批和用户输入请求。
- Codex MCP：适合让 Codex 调用外部工具，但不作为豆豆主集成面。
- ESP-IDF + LVGL：用于 ESP32-C3 圆形屏幕固件开发。
