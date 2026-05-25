# 豆豆设备协议 v1

> 适用范围:Doudou Bridge ↔ ESP32-C3 豆豆设备
> 传输:MVP 用 `ws://<bridge>:8788/device`(LAN 明文),V1 升级到 `wss://`
> 编码:UTF-8 JSON,单条消息 < 1 KB
> 发现:Bridge 通过 mDNS 通告 `_doudou._tcp.local`(TXT 含 `v` / `path` / `sim`),设备无需硬编码 IP

本文档替代 [technical-plan.md "设备协议"](./technical-plan.md) 章节的精简描述。技术方案的硬约束(标题字数、消息大小)在本文档继续生效。

## 1. 设计原则

1. **设备无业务智能**:豆豆只渲染 + 上报点击,Bridge 负责把 Codex 事件归一成本协议。
2. **协议向前兼容**:每条消息必须带 `v`,Bridge/设备遇到未知字段 **忽略** 而非报错。
3. **绝对时间不可信**:豆豆没有 RTC,所有时间字段以 Bridge `welcome` 下发的 `server_time_ms` 为锚,设备本地用 `esp_timer_get_time()` 相对计时。
4. **传输与协议解耦**:网页模拟器和真机走 **完全相同** 的消息;模拟器没有特权消息。

## 2. 帧结构

所有消息共享外层:

```json
{
  "v": 1,
  "type": "<message-type>",
  "seq": 42,
  "ts": 123456,
  ...payload
}
```

| 字段 | 类型 | 必填 | 说明 |
|---|---|:---:|---|
| `v` | int | ✓ | 协议大版本。当前 `1`。不兼容变更才递增。 |
| `type` | string | ✓ | 消息类型枚举,见 §3。 |
| `seq` | int | ✓ | **发送方** 单调递增序号,从 1 开始,重连后重置。 |
| `ts` | int | ✓ | 发送方相对时间戳(ms)。设备侧来自 `esp_timer`,Bridge 侧来自单调时钟。 |

## 3. 消息类型清单

| `type` | 方向 | 用途 |
|---|---|---|
| `welcome` | Bridge → 设备 | 握手响应,下发时间锚和服务能力 |
| `ping` / `pong` | 双向 | 心跳,任一方发都行 |
| `status` | Bridge → 设备 | Codex 任务状态变化 |
| `question` | Bridge → 设备 | 需要用户决策的提问(MVP 阶段 rollout 不带审批,暂不会触发) |
| `reply` | 设备 → Bridge | 用户对 question 的回复 |
| `usage` | Bridge → 设备 | Token 用量 + 多模型限流窗口快照(用于"用量"屏) |
| `session_info` | Bridge → 设备 | 当前会话元数据(标题、模型、cwd、账号、计划等;用于"信息"屏 + 宠物下方标题) |
| `thread_list` | Bridge → 设备 | 最近会话列表(用于"会话列表"屏) |
| `follow_thread` | 设备 → Bridge | 用户在会话列表点击某条 → 切换 Bridge 跟随该 thread 的 rollout |
| `ack` | 接收方 → 发送方 | 确认收到带 `seq` 的关键消息 |
| `error` | Bridge → 设备 | Bridge 侧错误展示 |
| `device_status` | 设备 → Bridge | 设备健康度上报(电量、RSSI、heap 余量) |

## 4. 握手

### 4.1 设备 → Bridge:首帧

WebSocket 连接建立后,设备 **必须** 在 2 秒内发送:

```json
{
  "v": 1,
  "type": "hello",
  "seq": 1,
  "ts": 0,
  "device_id": "doudou-001",
  "fw_version": "0.3.2",
  "pairing_token": "pt_a7c9...",
  "resume_after_seq": 17
}
```

- `pairing_token`:配对时 Bridge 下发,设备存 NVS。鉴权失败 Bridge 立即关闭 socket。
- `resume_after_seq`:**可选**,断线重连时设备告知最后处理到的 Bridge `seq`,Bridge 可补发遗漏的 `question`(仅未过期)。首次连接置 0 或省略。

### 4.2 Bridge → 设备:welcome

```json
{
  "v": 1,
  "type": "welcome",
  "seq": 1,
  "ts": 0,
  "server_time_ms": 1779345588123,
  "session_id": "sess_xyz",
  "heartbeat_interval_ms": 15000,
  "max_question_choices": 4,
  "features": ["ack", "device_status"]
}
```

- `server_time_ms`:绝对时间锚,设备记录"收到 welcome 时的本地 `esp_timer` 值"与此值的差,用于换算后续消息的 `expires_at`。
- `features`:Bridge 启用的可选能力。设备据此决定是否上报 `device_status`、是否对关键消息发 `ack`。

## 5. 心跳

- 双方 **任一方** 都可以发 `ping`,接收方必须立即回 `pong` 并复用 `seq` 字段为 `pong_for_seq`。
- 默认间隔由 `welcome.heartbeat_interval_ms` 决定(15s)。
- 设备 **连续 2 次心跳超时**(30s 无任何消息) → 主动重连。Bridge 同理。

```json
// ping
{ "v": 1, "type": "ping", "seq": 87, "ts": 412300 }

// pong
{ "v": 1, "type": "pong", "seq": 88, "ts": 412305, "pong_for_seq": 87 }
```

## 6. 关键业务消息

### 6.1 status

Codex 任务状态变化时下发,**幂等覆盖**:设备只保留最新一条,旧的丢弃。

```json
{
  "v": 1,
  "type": "status",
  "seq": 12,
  "ts": 5123,
  "state": "thinking",
  "title": "Codex 正在分析",
  "body": "扫描项目结构",
  "updated_at": 1779345588123
}
```

`state` 枚举:`idle` / `thinking` / `executing` / `waiting_input` / `waiting_approval` / `done` / `error`

### 6.2 question

需要用户决策的提问。**必须 ack**(若 `features` 含 `"ack"`)。

```json
{
  "v": 1,
  "type": "question",
  "seq": 13,
  "ts": 5200,
  "id": "req_123",
  "risk": "high",
  "action_type": "run_command",
  "title": "运行 npm test?",
  "body": "Codex 想在项目根目录执行测试",
  "choices": [
    { "id": "accept", "label": "同意" },
    { "id": "decline", "label": "拒绝" }
  ],
  "expires_at": 1779345600000,
  "require_confirm": true
}
```

字段说明:
- `risk`:`low` / `medium` / `high`,见 [risk-policy.md](./risk-policy.md)。设备据此选页面模板和配色。
- `action_type`:`run_command` / `modify_file` / `network_access` / `user_input` / `tool_call` / `other`。用于在屏幕顶部图标化展示。
- `require_confirm`:`true` 时设备显示二次确认页(选择 → "你确定吗?" → 提交),`false` 直接提交。`risk: "high"` 时 Bridge **必须** 置 `true`。
- `queue_total`(可选,int ≥ 1):Bridge 端 inflight question 总数(含本条)。设备渲染 `+N` 角标提示用户后面还有多少待办。Bridge 不发时设备退化为本地计数。

**注意 — MVP 阶段不会触发**:在 follow 模式下 Bridge 只 tail rollout 文件,审批走 Codex Desktop 内部 IPC,**不写 rollout**。要在豆豆上接管审批需要 OpenAI 提供多客户端审批广播 API,目前 codex 0.131 还没有 — 协议预留好接口,等 API 出现立即接。

### 6.3 reply

```json
{
  "v": 1,
  "type": "reply",
  "seq": 9,
  "ts": 8420,
  "id": "req_123",
  "choice_id": "accept",
  "device_id": "doudou-001"
}
```

- Bridge 用 `id` 做幂等:同一 `id` 二次回复直接忽略,但仍回 `ack` 让设备知道已送达。
- 设备发出 `reply` 后 **5 秒内未收到 `ack`** 应重发(最多 3 次),仍失败显示 `error{code: "reply_dropped"}`。

### 6.4 ack

```json
{ "v": 1, "type": "ack", "seq": 14, "ts": 5210, "ack_for_seq": 13 }
```

仅对 `question` 和 `reply` 必须 ack(其他消息可选)。`status` 不 ack(高频且幂等覆盖)。

### 6.5 error

```json
{
  "v": 1,
  "type": "error",
  "seq": 15,
  "ts": 5300,
  "code": "request_expired",
  "title": "请求已过期",
  "body": "请回到电脑继续处理",
  "related_id": "req_123"
}
```

`code` 枚举(MVP 全集):

| code | 场景 | 设备页面 |
|---|---|---|
| `request_expired` | question 已超 `expires_at` | "请求已过期"页 |
| `bridge_disconnected` | Bridge 与 Codex 断开 | "电脑端断开"页 |
| `codex_unreachable` | Codex 进程异常 | "Codex 异常"页 |
| `pairing_invalid` | token 失效或被撤销 | "需要重新配对"页(进入配网) |
| `protocol_version_mismatch` | 设备 fw 与 Bridge 协议不兼容 | "请升级 Bridge / 设备"页 |
| `reply_dropped` | 设备侧 reply 重试用尽 | toast 提示,回 idle |
| `rate_limited` | 设备消息过快被 Bridge 限流 | toast 提示 |
| `internal` | 其他未分类 | "出错了"页 + code |

### 6.6 usage

Bridge 周期性透传 Codex 的 token 用量与限流配额,用于设备的"用量"屏。**不需要 ack**(频率不高 + 幂等覆盖)。

```json
{
  "v": 1,
  "type": "usage",
  "seq": 30,
  "ts": 5400,
  "session": {
    "input_tokens": 328610,
    "output_tokens": 1721,
    "cached_tokens": 290816,
    "total_tokens": 330331,
    "current_context_tokens": 42199,
    "model_context_window": 258400
  },
  "limits": [
    { "id": "codex_primary",  "label": "5h", "used_pct": 4, "window_minutes": 300, "resets_at": 1779379342000 },
    { "id": "codex_secondary","label": "7d", "used_pct": 8, "window_minutes": 10080, "resets_at": 1779843610000 },
    { "id": "codex_bengalfox_primary",  "label": "5h", "group_label": "GPT-5.3-Codex-Spark",
      "used_pct": 0, "window_minutes": 300, "resets_at": 1779388180000 },
    { "id": "codex_bengalfox_secondary","label": "7d", "group_label": "GPT-5.3-Codex-Spark",
      "used_pct": 0, "window_minutes": 10080, "resets_at": 1779974980000 }
  ],
  "plan_type": "pro"
}
```

字段说明:
- `session.total_tokens`:**跨多轮累计** 的 token 总量(billable)。和 `model_context_window` 不直接可比。
- `session.current_context_tokens`:**最近一轮** 的 input + output,代表"模型现在持有的上下文大小"。设备据此渲染上下文占用 / 剩余条。
- `session.model_context_window`:模型单次上下文窗口大小(如 258400)。
- `limits[].group_label`:同一计划下不同模型的独立配额(主模型未设置,Spark 等设置)。设备按 `group_label` 分块渲染。

发送时机:
- RolloutWatcher 解析 rollout 的 `event_msg.token_count` → 更新 `session`
- ThreadListPoller 每 30s 调 `account/rateLimits/read` → 更新 `limits`(包含多模型组)
- 任一更新 push;Bridge 重连时也会立即重发缓存 snapshot

字段都是 **可选**:缺失字段设备渲染成 `—`。

### 6.7 session_info

会话元数据快照,合并发送 — Bridge 多个来源会陆续填字段,设备按"上次 + 新字段"叠加缓存。

```json
{
  "v": 1,
  "type": "session_info",
  "seq": 5,
  "ts": 100,
  "session_id": "019e4a99-9219-7603-9166-3fe62854b187",
  "thread_title": "修复 Codex 桌面版启动缓慢",
  "source": "Codex Desktop",
  "model": "gpt-5.5",
  "reasoning_effort": "xhigh",
  "summary_mode": "none",
  "cwd": "~/Documents/example-project",
  "permissions": "Workspace (on-request)",
  "approval_policy": "on-request",
  "sandbox": "workspace-write",
  "collaboration_mode": "default",
  "account_email": "user@example.com",
  "plan_type": "pro",
  "agents_md": true,
  "git_branch": "main",
  "cli_version": "0.131.0"
}
```

数据来源:
- `session_meta`(rollout 首行)→ id / source / cwd / cli_version / git_branch
- `turn_context`(每轮)→ model / reasoning_effort / summary_mode / approval_policy / sandbox / collaboration_mode / permissions(合成)
- `account/read`(Bridge 启动时一次性 RPC)→ account_email / plan_type
- `thread/list`(ThreadListPoller)→ thread_title(active thread 的)
- 文件系统检查 `<cwd>/AGENTS.md` 存在性 → agents_md

字段全部 **可选** — 设备遇到 `undefined` 字段渲染成"—"或省略行。

### 6.8 thread_list

Codex 持久化的最近会话列表(读自 codex 的 `thread/list` RPC,rollout 不带 AI 生成的标题,只能 RPC 拿)。

```json
{
  "v": 1,
  "type": "thread_list",
  "seq": 6,
  "ts": 200,
  "threads": [
    {
      "id": "019e4a99-9219-7603-9166-3fe62854b187",
      "title": "修复 Codex 桌面版启动缓慢",
      "source": "vscode",
      "active": true,
      "updated_at": 1779368380000
    },
    {
      "id": "019e4a49-f860-72e1-bff2-b1a6d9f143cf",
      "title": "hello",
      "source": "vscode",
      "updated_at": 1779362881000
    }
  ]
}
```

字段:
- `title`:Codex 生成的 AI 标题(`thread/list.name`),未生成时回退到首个用户消息 `preview`。
- `active`:是否是 Bridge 当前跟随的 thread(豆豆主屏标题对应这条)。
- `updated_at`:epoch 毫秒。

刷新频率:10 秒。设备在"会话列表"屏渲染,点击非 active 的项 → 发 `follow_thread`。

### 6.9 follow_thread(device → bridge)

```json
{
  "v": 1,
  "type": "follow_thread",
  "seq": 12,
  "ts": 500,
  "thread_id": "019e4a49-f860-72e1-bff2-b1a6d9f143cf"
}
```

Bridge 收到后:
1. RolloutWatcher 在 `~/.codex/sessions/` 找匹配 thread_id 的 rollout 文件
2. attach 到新文件,旧的 watcher 断开
3. 立即 push 新的 thread_list(active 标志已更新) + 新的 session_info

如果找不到 rollout 文件(thread 太老 / 已归档),静默忽略,**不报错给设备**(避免设备屏抖动)。

### 6.10 device_status

`features` 含 `"device_status"` 时,设备每 60 秒上报一次:

```json
{
  "v": 1,
  "type": "device_status",
  "seq": 200,
  "ts": 100000,
  "battery_pct": 78,
  "charging": false,
  "rssi": -62,
  "free_heap": 78320,
  "uptime_ms": 3600000
}
```

Bridge 用来在本地配置页展示设备健康度,**不下发回设备**(避免 UI 自循环)。

## 6.11 HTTP 旁路:`/api/recent`

设备的"历史"屏走 HTTP 拉取,不占 WebSocket 通道(避免 MVP 阶段引入新消息类型)。

```
GET /api/recent?n=10
→ 200 OK
{
  "entries": [
    {
      "ts": "2026-05-21T12:31:04.512Z",
      "action_type": "run_command",
      "risk": "high",
      "summary": "git push origin main",
      "choice": "accept"
    },
    ...
  ]
}
```

- 真机固件用 `esp_http_client` GET,JSON 解析后渲染。
- Bridge 端从审计 JSONL 文件读最后 N 行,反序输出。
- 设备每 10s 拉一次(在历史屏可见时)。后台不拉,省电。

未来 V1 如果需要 push,会改为独立的 `decision_recorded` 消息类型。

## 7. 配对与首次绑定

> 这一节解决"明文 WebSocket 下 pairing token 怎么安全交换"的核心问题。

### 7.1 出厂状态

- NVS 中无 `pairing_token`、无 Wi-Fi 凭据
- 上电后设备 **不启动** WebSocket client,直接进入 **配网模式**

### 7.2 配网模式

设备同时开启两个通道,二选一:

1. **BLE GATT 服务**(主推荐) — Bridge 端的配置 UI(浏览器 + Web Bluetooth)直接发现并连接设备,通过 BLE 写入 Wi-Fi SSID/密码 和 Bridge URL。
2. **SoftAP fallback** — 设备开启 SSID `doudou-setup-XXXX`(XXXX 为 MAC 后 4 位),用户手机连上后访问 `http://192.168.4.1/setup` 填表。Web Bluetooth 不可用(如 iOS Safari)时使用。

### 7.3 pairing token 交换

避免明文 WS 下 token 泄露的关键设计:**token 在 BLE/AP 通道内生成和交换,不经过 Wi-Fi LAN**。

时序:

```
1. 用户在 Bridge 配置页 (127.0.0.1:8787) 点 "配对新设备"
2. Bridge 生成 256-bit 随机 token,显示 6 位短验证码(如 "8K3M2P")
3. 设备进入配网,扫描到 BLE,Bridge UI 把 SSID/密码/Bridge URL/token 一次性写入 BLE characteristic
4. 设备连 Wi-Fi → 连 Bridge WS → hello 帧带上 token
5. 设备屏幕显示 Bridge 端的同一 6 位验证码(由 token 派生),用户在 Bridge UI 点 "我看到了相同的码 ✓"
6. Bridge 确认后 token 永久生效,设备进入正常状态
7. 任一侧不匹配:设备清 NVS 重回配网,Bridge 撤销 token
```

6 位验证码 = 防 MITM:即使有人在 LAN 上抢先用同样 SSID 仿冒 Bridge,验证码不匹配用户能发现。

### 7.4 解绑 / 重置

- 用户在 Bridge UI 点"解绑设备" → Bridge 撤销 token → 设备下次 hello 收到 `pairing_invalid` 错误 → 自动清 NVS 回配网
- 用户在设备上长按触屏 10s(MVP 用 BOOT 按键也行,因为没有别的物理输入) → 清 NVS 回配网
- 重置后所有审计日志保留在 Bridge 端,不丢

## 8. 时间和过期

设备只能信任 **相对时间**。`question.expires_at` 是 Bridge 时钟下的绝对毫秒时间戳。设备处理流程:

```
on welcome:
    server_time_anchor = msg.server_time_ms
    local_time_anchor  = esp_timer_get_time() / 1000   # ms

on question:
    expires_in_ms = msg.expires_at - server_time_anchor
    expires_at_local = local_time_anchor + expires_in_ms
    # 每帧 UI 检查 esp_timer ms >= expires_at_local
```

时钟漂移 ESP32-C3 大约 ±20 ppm,1 小时偏差 < 100 ms,对几十秒级的审批 expiry **完全可忽略**。

## 9. 审计日志格式

落盘在设备 `audit` 分区(NVS namespace `audit`),仅记录 **用户的确认决策**,不记录 Codex 内容。

```c
// 单条 ~64 bytes
struct audit_entry {
    uint32_t local_ts_ms;        // esp_timer 本地时间
    uint64_t server_ts_ms;       // welcome 锚换算后的绝对时间
    char     request_id[24];     // Bridge 下发的 question.id
    char     choice_id[16];      // 用户选择
    uint8_t  risk_level;         // 0=low / 1=med / 2=high
    uint8_t  action_type;        // 与 action_type 字段对应
};
```

ring buffer 容量:64 KB / 64 B ≈ 1000 条。满后覆盖最老。

Bridge 端 **独立** 维护完整审计(无大小限制),设备端这份是 fail-safe 副本,供"电脑离线情况下证明设备做过什么"使用。

## 10. 设备协议状态机

```
                ┌────────┐
                │  boot  │
                └───┬────┘
                    │
       no pairing ──┴── has pairing
            │              │
       ┌────▼─────┐   ┌────▼─────────┐
       │ pairing  │   │  connecting  │ ←─┐
       └────┬─────┘   └────┬─────────┘   │
            │              │              │ reconnect
            └─────────────►│              │ (token still valid)
                           │              │
                    ┌──────▼─────┐        │
                    │ handshaking│ ──►────┤  pairing_invalid
                    └──────┬─────┘        │
                           │              │
                    ┌──────▼─────┐        │
                    │   ready    │ ───────┘  socket close / timeout
                    └──────┬─────┘
              status / question events
```

`ready` 是稳态。任何 4xx 类错误(`pairing_invalid` / `protocol_version_mismatch`)直接回 `pairing`,5xx 类(`bridge_disconnected` / `codex_unreachable`)展示错误页但不退出 `ready`。
