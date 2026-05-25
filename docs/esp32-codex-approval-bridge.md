# ESP32 触摸屏接管 Codex 权限审批方案

> 调研时间: 2026-05-22
> 目标: 让 ESP32 触摸屏设备接收 Codex Desktop 的权限申请,用户在设备上选择同意/拒绝后,Codex Desktop 能感知选择并继续或中断流程。

## 结论

可实现。推荐方案是使用 Codex 官方 `PermissionRequest` hook,由本机 Bridge 把审批请求转发到 ESP32 设备,等待设备触摸选择后,Bridge 再通过 hook 的标准输出把 `allow` 或 `deny` 返回给 Codex。

这条路径比直接反向工程 Codex Desktop 内部 IPC 更稳:

- 官方 hooks 文档明确支持 `PermissionRequest` 事件。
- `PermissionRequest` hook 可以返回审批决策。
- `allow` 会让 Codex 继续执行,且跳过原始审批弹窗。
- `deny` 会拒绝该权限请求,并把拒绝信息返回给 Codex。
- 如果 hook 不做决定,Codex 会继续走原本的用户审批流程,可作为降级路径。

因此,ESP32 不需要直接接入 Codex Desktop 内部进程。设备只需要和本机 Bridge 通信。

## 关键证据

### 1. 官方 hook 支持权限申请事件

官方文档将 `PermissionRequest` 定义为 Codex 即将请求权限时触发的 hook:

- 文档: [Codex Hooks](https://developers.openai.com/codex/hooks)
- 事件名: `PermissionRequest`
- 触发时机: Codex 准备请求用户授权时
- 典型输入字段: `cwd`、`tool_name`、`tool_input`、`description`

这说明 Codex 有官方入口可以在审批弹窗前拦截权限申请。

### 2. 官方 hook 可以返回 allow / deny

官方文档给出的通过决策格式:

```json
{
  "hookSpecificOutput": {
    "hookEventName": "PermissionRequest",
    "decision": { "behavior": "allow" }
  }
}
```

官方文档给出的拒绝决策格式:

```json
{
  "hookSpecificOutput": {
    "hookEventName": "PermissionRequest",
    "decision": {
      "behavior": "deny",
      "message": "Blocked by ESP32 approval device."
    }
  }
}
```

这说明 hook 不是只能审计或通知,而是能实际影响 Codex 的审批结果。

### 3. 决策优先级符合设备审批需求

官方文档描述的行为可归纳为:

| hook 输出 | Codex 行为 | 对 ESP32 方案的意义 |
|---|---|---|
| `allow` | 继续执行,不再显示原始审批弹窗 | 设备选择同意后,Codex 可继续流程 |
| `deny` | 拒绝请求,并展示 message | 设备选择拒绝后,Codex 可中断该动作 |
| 无决策 | 继续走 Codex 原审批流程 | 设备离线或超时时可降级到桌面弹窗 |

这正好覆盖 ESP32 触摸屏的核心交互:同意、拒绝、超时降级。

### 4. 本机协议侧也能看到审批相关类型

当前本机 Codex CLI 版本:

```bash
codex --version
# codex-cli 0.128.0
```

可复现导出 app-server 协议类型:

```bash
mkdir -p /tmp/codex-appserver-ts /tmp/codex-appserver-schema
codex app-server generate-ts --out /tmp/codex-appserver-ts
codex app-server generate-json-schema --out /tmp/codex-appserver-schema
rg -n "PermissionRequest|requestApproval|ApprovalsReviewer" /tmp/codex-appserver-ts /tmp/codex-appserver-schema
```

本机导出的类型里能看到:

- `HookEventName` 包含 `permissionRequest`
- `CommandExecutionRequestApprovalParams`
- `CommandExecutionRequestApprovalResponse`
- `FileChangeRequestApprovalParams`
- `FileChangeRequestApprovalResponse`
- `PermissionsRequestApprovalParams`
- `PermissionsRequestApprovalResponse`
- `ApprovalsReviewer`

这说明 Codex 当前版本的协议层也有权限请求、命令审批、文件修改审批等概念。对 Codex Doudou 来说,优先使用官方 hook;app-server 协议可作为后续更深度集成的方向。

## 推荐架构

```
Codex Desktop
  -> PermissionRequest hook
    -> 本机 approval bridge
      -> WebSocket / MQTT / HTTP
        -> ESP32 触摸屏
        <- allow / deny / timeout
    <- hook stdout JSON
  -> Codex 继续执行或拒绝请求
```

组件职责:

| 组件 | 职责 |
|---|---|
| Codex Desktop | 原有 Codex 客户端,不改 UI、不改二进制 |
| PermissionRequest hook | 接收 Codex 权限请求,调用本机 Bridge,输出 allow/deny |
| Approval Bridge | 管理设备连接、转发审批、等待触摸选择、做超时和审计 |
| ESP32 设备 | 显示审批内容,提供同意/拒绝按钮,回传用户选择 |

## 与现有 Codex Doudou 文档的关系

`technical-plan.md` 早期写过“MVP 阶段豆豆不接管审批,因为 Codex Desktop 审批通过内部 IPC 且不写 rollout”。这个判断对“只 tail rollout 文件”的 MVP 路线仍然成立,但对完整系统已经可以更新:

- **只 tail rollout 文件**:仍拿不到审批请求。
- **接入 `PermissionRequest` hook**:可以在 Codex Desktop 权限申请发生时同步给 ESP32,并让 Codex 感知结果。
- **接入 app-server**:如果 Bridge 自己启动/管理 Codex 会话,还可以处理 server-initiated approval request,但这更像自建客户端,不是最小改动接管 Desktop。

因此推荐把审批接管作为 Doudou 的 V1/V1.1 能力,而不是依赖 rollout tail 的 MVP 能力。

## Hook 配置

建议新增全局或项目级 hook 配置。

全局路径:

```text
~/.codex/hooks.json
```

项目路径:

```text
.codex/hooks.json
```

示例:

```json
{
  "hooks": {
    "PermissionRequest": [
      {
        "matcher": "*",
        "hooks": [
          {
            "type": "command",
            "command": "~/.codex/esp32-approval/permission_request_hook.py",
            "timeout": 120,
            "statusMessage": "Waiting for ESP32 approval"
          }
        ]
      }
    ]
  }
}
```

说明:

- `matcher: "*"` 表示所有权限请求都交给该 hook。
- `timeout` 要大于设备侧倒计时,例如设备 60 秒超时,hook 可设置 90-120 秒。
- `statusMessage` 会让 Codex UI 侧知道当前正在等待外部审批。

## Hook 脚本行为

`permission_request_hook.py` 的核心流程:

1. 从 stdin 读取 Codex 传入的 JSON。
2. 提取 `cwd`、`tool_name`、`tool_input`、`description`。
3. 生成 `request_id` 和过期时间。
4. 调用本机 Approval Bridge,例如 `POST http://127.0.0.1:8765/approval/request`。
5. 阻塞等待 Bridge 返回 `allow` / `deny` / `timeout`。
6. 向 stdout 输出 Codex hook 所需 JSON。

伪代码:

```python
#!/usr/bin/env python3
import json
import sys
import urllib.request

payload = json.load(sys.stdin)

bridge_payload = {
    "source": "codex",
    "event": "PermissionRequest",
    "cwd": payload.get("cwd"),
    "tool_name": payload.get("tool_name"),
    "tool_input": payload.get("tool_input"),
    "description": payload.get("description"),
}

req = urllib.request.Request(
    "http://127.0.0.1:8765/approval/request",
    data=json.dumps(bridge_payload).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)

with urllib.request.urlopen(req, timeout=120) as resp:
    result = json.loads(resp.read().decode("utf-8"))

if result["decision"] == "allow":
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PermissionRequest",
            "decision": {"behavior": "allow"}
        }
    }))
elif result["decision"] == "deny":
    print(json.dumps({
        "hookSpecificOutput": {
            "hookEventName": "PermissionRequest",
            "decision": {
                "behavior": "deny",
                "message": result.get("message", "Denied by ESP32 approval device.")
            }
        }
    }))
else:
    # 无输出决策,让 Codex 回退到默认审批弹窗。
    print(json.dumps({}))
```

生产实现不要直接照抄以上伪代码,需要补齐鉴权、错误处理、日志和超时策略。

## Bridge 到 ESP32 的设备协议映射

沿用 [device-protocol.md](./device-protocol.md) 的 `question` / `reply` 机制。

Bridge 收到 hook 请求后,下发:

```json
{
  "v": 1,
  "type": "question",
  "seq": 42,
  "ts": 123456,
  "id": "approval_01HX...",
  "risk": "high",
  "action_type": "run_command",
  "title": "允许 Codex 执行命令?",
  "body": "cwd: ~/workspace/my-project\ncmd: aws lambda invoke ...",
  "choices": [
    { "id": "allow", "label": "同意" },
    { "id": "deny", "label": "拒绝" }
  ],
  "expires_at": 1779345600000,
  "require_confirm": true
}
```

ESP32 点击后回传:

```json
{
  "v": 1,
  "type": "reply",
  "seq": 17,
  "ts": 130000,
  "id": "approval_01HX...",
  "choice_id": "allow"
}
```

Bridge 将 `choice_id` 转换为 Codex hook 决策:

| 设备选择 | hook 输出 |
|---|---|
| `allow` | `decision.behavior = "allow"` |
| `deny` | `decision.behavior = "deny"` |
| 超时且配置为降级 | 输出 `{}` 或无决策,交还 Codex Desktop |
| 超时且配置为强制设备审批 | `decision.behavior = "deny"` |

## 安全设计

权限审批是高风险能力,不能让局域网任意设备伪造同意。

最低要求:

- Bridge 只监听 `127.0.0.1` 上的 hook HTTP 入口。
- ESP32 到 Bridge 的连接必须先配对。
- 设备保存 `pairing_token` 到 NVS。
- 每条 `reply` 必须带 `request_id`。
- Bridge 只接受当前 in-flight 且未过期的 `request_id`。
- Bridge 对重复 `reply` 做幂等处理,第一条有效回复生效。

V1 建议:

- Bridge 与 ESP32 使用 WebSocket。
- 首帧 `hello` 携带 `device_id` 和 `pairing_token`。
- Bridge 校验 token 后才发送任何审批内容。
- 每条关键消息加 `nonce` 和 HMAC:

```text
signature = HMAC_SHA256(pairing_secret, canonical_json(message_without_signature))
```

V1.1 建议:

- 局域网走 `wss://`。
- 配对时在桌面显示二维码,ESP32 扫码或通过串口写入一次性 token。
- 审批日志写入本机审计文件,包含时间、命令摘要、设备 ID、结果。

## 超时策略

推荐默认策略: **超时降级到 Codex 原审批弹窗**。

理由:

- ESP32 可能离线、掉电、Wi-Fi 断连。
- 用户可能没看设备。
- 降级到原弹窗不会降低 Codex 原有安全性。

配置建议:

| 场景 | 默认行为 |
|---|---|
| ESP32 在线,用户点同意 | hook 返回 allow |
| ESP32 在线,用户点拒绝 | hook 返回 deny |
| ESP32 离线 | hook 无决策,回退桌面审批 |
| ESP32 超时 | hook 无决策,回退桌面审批 |
| 高安全模式 | 超时返回 deny |

## 风险与限制

| 风险 | 影响 | 缓解 |
|---|---|---|
| Codex hook schema 未来变动 | hook 解析字段可能失效 | Bridge 启动时记录 Codex 版本,保留原始 payload 日志 |
| hook 超时过短 | 用户还没点设备,Codex 已回退或失败 | hook timeout 设为设备 timeout + 30 秒 |
| ESP32 屏幕空间有限 | 命令太长显示不完整 | Bridge 做摘要,长命令只显示程序名、cwd、风险原因 |
| 局域网伪造回复 | 未授权批准高风险动作 | pairing token + request_id + HMAC |
| 多个审批同时出现 | 设备只能展示一个焦点问题 | Bridge 排队,设备显示 `queue_total` |

## MVP 实施步骤

1. 写一个本机 `approval-bridge` 服务:
   - HTTP: `POST /approval/request`
   - WebSocket: `/device`
   - 临时网页调试页: `http://127.0.0.1:8765/approval`

2. 写 `permission_request_hook.py`:
   - stdin 读 Codex payload
   - POST 到 Bridge
   - stdout 输出 allow/deny JSON

3. 配置 `~/.codex/hooks.json`:
   - 注册 `PermissionRequest`
   - matcher 先用 `*`

4. 用浏览器调试页模拟 ESP32:
   - 当前可用 URL: `http://127.0.0.1:8765/approval`
   - 验证同意、拒绝、超时三条路径

5. 接入 ESP32:
   - WebSocket 连接 Bridge
   - 渲染 `question`
   - 触摸发送 `reply`

6. 补强安全:
   - 配对 token
   - HMAC
   - 审计日志
   - 超时策略配置

## 验证用例

| 用例 | 触发方式 | 期望结果 |
|---|---|---|
| 命令审批同意 | 让 Codex 执行需要 escalation 的命令 | ESP32 显示审批,点同意后 Codex 继续 |
| 命令审批拒绝 | 同上 | ESP32 点拒绝后 Codex 收到 deny |
| 设备离线 | 停止 ESP32 或断开 WebSocket | Codex 回退到原审批弹窗 |
| 设备超时 | 不点击设备 | Codex 回退弹窗或强制拒绝,取决于配置 |
| 重复点击 | ESP32 连续发送两次 reply | Bridge 只接受第一次 |
| 过期回复 | 超时后再点同意 | Bridge 拒绝该 reply,不影响 Codex |
| 长命令显示 | 触发长 shell 命令 | 设备显示摘要,详情可在 Desktop 查看 |

## 后续路线

### 路线 A: Hook-first

适合目标:

- 保留 Codex Desktop 原交互。
- 只把权限审批外接到 ESP32。
- 最少改动、最快上线。

这是推荐路线。

### 路线 B: App-server client

适合目标:

- Bridge 自己启动或连接 Codex app-server。
- Bridge 作为完整客户端消费 thread/turn/item 事件。
- ESP32 不仅处理审批,还同步任务状态、计划、输出摘要。

证据:

- 官方 app-server 文档说明它面向 rich clients。
- 当前协议里存在 `item/commandExecution/requestApproval`、`item/fileChange/requestApproval`、`item/permissions/requestApproval` 等 server-initiated request。

缺点:

- 实现复杂度更高。
- 更像自研 Codex 客户端,而不是给现有 Desktop 加外设审批。
- 当前桌面 app 的内部 socket 不应作为稳定公共接口依赖。

### 推荐决策

先实现 Hook-first。等 Doudou Bridge 已经稳定承担设备连接、状态同步、审计日志后,再评估是否把 app-server 作为更完整的事件源。

