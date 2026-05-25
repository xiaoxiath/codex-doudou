# Codex app-server 调研

> 调研时间:2026-05-21
> 资料来源:
> - [App Server – Codex / OpenAI Developers](https://developers.openai.com/codex/app-server)
> - [codex/codex-rs/app-server/README.md](https://github.com/openai/codex/blob/main/codex-rs/app-server/README.md)
> - [Codex Changelog](https://developers.openai.com/codex/changelog)

## 结论

Codex app-server 的事件模型 **完全覆盖** 豆豆方案所需的能力,主要包括:

- 任务状态变化:`turn/started`、`turn/completed`、`thread/status/changed`
- 命令审批:`item/commandExecution/requestApproval` (server-initiated request)
- 文件修改审批:`item/fileChange/requestApproval` (server-initiated request)
- 工具调用用户输入:`tool/requestUserInput`
- 流式输出:`item/started` → `item/agentMessage/delta` → `item/completed`
- 中断:`turn/interrupt` 后服务端发 `turn/completed{status:"interrupted"}`

**方案可行性确认:**Bridge 通过 stdio 启动 `codex app-server`,即可拿到完整事件流,无需自行轮询或反向工程内部状态。

## 协议要点

### 传输层

| 传输 | 命令 | 稳定性 | 推荐用途 |
|---|---|---|---|
| stdio (JSONL) | 默认 / `--listen stdio://` | 稳定 | **Bridge 默认使用** |
| Unix socket | `--listen unix://PATH` | 稳定 | Bridge 二选一(同机调试更易) |
| WebSocket | `--listen ws://IP:PORT` | **实验性、官方标注 unsupported** | 不用 |

**决定**:Bridge 用 stdio 拉起 `codex app-server` 子进程,避开 WS 实验性风险。豆豆设备和 Bridge 之间的 WebSocket 是独立的链路,与 app-server 协议无关。

### 消息格式

JSON-RPC 2.0,但 wire 上省略 `"jsonrpc":"2.0"` 头,按行分隔 (JSONL)。

- **Request**:`{id, method, params}`
- **Response**:`{id, result | error}`
- **Notification**:`{method, params}`(无 `id`)

### 三级模型

```
thread (长生命周期会话,持久化)
  └─ turn (单次用户请求 → 模型响应)
        └─ item (原子事件:userMessage / agentMessage / commandExecution / fileChange / mcpToolCall / ...)
```

- `thread.status`:`notLoaded` / `idle` / `active` / `systemError`
- `turn.status`:`inProgress` / `completed` / `interrupted` / `failed`
- `item.status`:`inProgress` / `completed` / `failed` / `declined`

### 初始化握手

```
client  → initialize { clientInfo: {name, version}, capabilities: {...} }
server  → initialize response
client  → initialized (notification)
```

**版本兼容关键**:
- 默认走 stable surface,**不要传 `capabilities.experimentalApi: true`**(避免 Codex 升级时被未稳定 API 影响)
- 用 `capabilities.optOutNotificationMethods` 显式忽略不关心的 notification(比如 `thread/realtime/*`),降低 Bridge 解析开销

### Bridge 关注的方法和事件

**Bridge → app-server (调用)**:
- `initialize` / `initialized` — 握手
- `thread/start` / `thread/resume` — 在新会话或恢复会话
- `turn/interrupt` — 用户在豆豆按"取消"时调用
- (可选) `thread/list` — 给本地配置页用

**app-server → Bridge (notification,Bridge 监听)**:
- `thread/started`、`thread/status/changed`
- `turn/started`、`turn/completed`、`turn/plan/updated`
- `item/started`、`item/completed`
- `item/agentMessage/delta` — 流式文字(Bridge 不需要逐字推给豆豆,缓冲后摘要)
- `item/commandExecution/outputDelta` — 命令输出(豆豆完全不显示)

**app-server → Bridge (server-initiated request,Bridge 必须 respond)**:
| 请求 | 客户端响应选项 | 豆豆映射 |
|---|---|---|
| `item/commandExecution/requestApproval` | `accept` / `acceptForSession` / `decline` / `cancel` / `acceptWithExecpolicyAmendment` | 高风险 question,二次确认 |
| `item/fileChange/requestApproval` | `accept` / `acceptForSession` / `decline` / `cancel` | 高风险 question,二次确认 |
| `tool/requestUserInput` | `accept` / `decline` / `cancel` | 普通 question |

⚠️ 这些是 **server-initiated request**,不是 notification。Bridge 必须 **在合理时限内** 回复,否则 Codex 会一直挂起。
**豆豆链路超时**(用户没看屏幕)时,Bridge 应主动回 `decline` 或 `cancel`,并在豆豆屏上展示"已自动拒绝"。

### 鉴权

app-server 自身有三种模式:`apikey` / `chatgpt` / `chatgptAuthTokens`。

**Bridge 不需要管理任何凭据** — 直接 inherit 用户本地 `codex` CLI 已登录的状态。Bridge 也 **不应该** 触发 `account/login/start`(避免在用户没看屏幕时弹出 OAuth 流)。

### 错误处理

- `-32001 Server overloaded; retry later.` → Bridge 指数退避重试
- 未发 `initialize` 直接调用 → `"Not initialized"` 错误
- turn 内错误通过 `turn/completed { status: "failed", error: {...} }` 暴露
- Bridge 应统一捕获后 fan-out 到豆豆的 `error` 消息

### 稳定性与版本策略

- 实验性 API 用 `capabilities.experimentalApi` 开关,默认不开
- 协议 schema 可能跨版本变动 — Bridge 需要 **协议适配层**,把 app-server 的事件归一成豆豆设备协议(详见 [device-protocol.md](./device-protocol.md))
- Bridge 启动时记录 app-server 版本(通过 `initialize` 响应中的 `serverInfo`),日志里带上;遇到未识别 method 时 warn 不 crash

## 对豆豆方案的影响

1. **Bridge 实现语言**:Node.js 或 Rust 都可以。Rust 与 Codex 同语言可复用 schema 类型(如果 OpenAI 把 schema crate 公开),Node.js 上线更快。MVP 阶段建议 **Node.js + TypeScript**,体力消耗最小。
2. **设备协议 vs app-server 协议解耦**:豆豆只感知 4-5 种消息类型,绝不直接看见 `item/*`。所有归一化在 Bridge 完成。
3. **Bridge 的"承担智能"列表(原方案)与 app-server 能力对齐良好**:
   - 事件归一化 ✓ (item → status/question/error)
   - 文本摘要 ✓ (Bridge 需要自己实现,不依赖 app-server)
   - 风险分级 ✓ (基于 `item.type` 和 `command`/`changes` 内容,详见 [risk-policy.md](./risk-policy.md))
   - 二次确认 ✓ (Bridge 收到 requestApproval → 推给豆豆 question → 等用户点 → response)
   - 幂等回放 ✓ (Bridge 缓存 in-flight requestApproval 的 `id`,豆豆重复 reply 直接忽略)
4. **MVP 不依赖任何实验性 API**:turn/item/审批/中断都在 stable 表面。

## 风险

| 风险 | 缓解 |
|---|---|
| Codex 升级时 schema 字段重命名或语义变化 | Bridge 加 schema 版本断言,启动时打印 server version |
| `tool/requestUserInput` 的具体 schema 文档未细化 | 第一次拿到真实事件后落盘 sample,补到此文档 |
| stdio 子进程崩溃 | Bridge 监控 exit code,自动重启并重连豆豆 |
| 单 Bridge 进程同时连多 thread 时 turn 事件交错 | 用 `params.threadId` 路由,Bridge 维护 thread → 豆豆会话映射 |
