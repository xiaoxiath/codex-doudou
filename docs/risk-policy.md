# 风险分级与二次确认策略

> 决定 Bridge 把 Codex 请求映射到豆豆 `question.risk` 字段的哪一级,以及是否触发 `require_confirm: true`。

## 三级定义

| 等级 | 含义 | 设备表现 |
|---|---|---|
| `low` | 只读、可逆、影响范围限于本地 sandbox | 普通 question 页,绿色边框 |
| `medium` | 修改项目文件、安装依赖、运行测试等"可还原但需要注意"的操作 | 黄色边框 + 顶部操作类型图标 |
| `high` | 不可逆 / 跨越项目边界 / 涉及网络外发 / 涉及凭据 | 红色边框 + **强制二次确认页** + 危险按钮不预选 |

## 映射规则

Bridge 收到 Codex 的 server-initiated request 时,按以下顺序判定:

### 1. `item/commandExecution/requestApproval`

| 命令特征 | 等级 | 理由 |
|---|---|---|
| 命令属于 `ls` / `cat` / `pwd` / `git status` / `git log` / `grep` / `rg` 等只读集合 | `low` | 无副作用 |
| `npm test` / `pytest` / `cargo test` / `go test` 等测试命令 | `medium` | 可能写入 build 缓存 |
| `npm install` / `pip install` / `cargo build` / `make` 等构建依赖类 | `medium` | 写入 `node_modules` / `target` |
| `git commit` / `git checkout` (本地分支) | `medium` | 可逆 |
| `git push` / `git reset --hard` / `git branch -D` / `git rebase` | **`high`** | 影响远端或破坏历史 |
| `rm` / `mv` / `dd` / `mkfs` / `chmod` / `chown` / `sudo *` | **`high`** | 不可逆或越权 |
| 包含 `>` / `>>` 重定向到 `/` 之外的绝对路径 | **`high`** | 跨项目边界写入 |
| 包含 `curl` / `wget` / `nc` / `ssh` / `scp` 等网络命令 | **`high`** | 外发 |
| `cwd` 不在用户当前 workspace 下 | **`high`** | 跨项目 |
| 其他未识别命令 | `medium` | 保守默认 |

判定算法:**取最高等级**。例如 `git push origin main && rm -rf /tmp/foo` 因为含 `git push` 直接判 `high`,即使 `rm` 单独看是 medium。

### 2. `item/fileChange/requestApproval`

| 变更特征 | 等级 |
|---|---|
| 全部 `kind: "create"` 且路径在项目内 | `low` |
| `kind: "modify"`,变更行数 ≤ 50 行 | `low` |
| `kind: "modify"`,变更行数 > 50 行 | `medium` |
| 任何 `kind: "delete"` | **`high`** |
| 任何路径在项目外(绝对路径或 `..` 逃出) | **`high`** |
| 任何路径匹配 `.env*` / `*.pem` / `*.key` / `id_rsa*` / `*credentials*` | **`high`** |
| 任何路径在 `.git/` 下 | **`high`** |

### 3. `tool/requestUserInput`

| 工具特征 | 等级 |
|---|---|
| MCP 工具,且工具描述标注 `side_effects: false` | `low` |
| 工具名匹配 `*search*` / `*read*` / `*list*` | `low` |
| 工具名匹配 `*write*` / `*create*` / `*update*` / `*delete*` | `medium` |
| 工具名匹配 `*send*` / `*post*` / `*publish*` / `*pay*` / `*deploy*` | **`high`** |
| 其他未识别 | `medium` |

## 二次确认 (`require_confirm`)

| 等级 | `require_confirm` | UI 行为 |
|---|:---:|---|
| `low` | `false` | 一次点击直接 reply |
| `medium` | `false` | 一次点击直接 reply,但按钮间距加大,默认无预选 |
| `high` | **`true`** | 第一次点击 → "你确定要 [动作描述] 吗?"页面 → 再次点 "确认" 才 reply。返回手势取消 |

## 拒绝默认值

豆豆设备 **超时未回复** 时(`question.expires_at` 到 + 网络抖动 buffer),Bridge 自动:

- `low` → `decline`
- `medium` → `decline`
- `high` → `cancel`(注意 `cancel` 与 `decline` 在 Codex 语义不同:`cancel` 中断整个 turn,`decline` 只是单步拒绝;高风险用 `cancel` 更安全)

## 配置覆盖

用户可在 Bridge 配置页(`http://127.0.0.1:8787/settings/risk`)调整:
- 降级:如把 `npm install` 从 `medium` 改成 `low`(不推荐)
- 升级:如把所有 `command` 都强制 `high`(零信任模式)
- 加白名单:某些命令固定 `low`(如团队内部脚本)

配置存 Bridge 端 JSON 文件,**不下发到设备** — 设备只信 Bridge 已经判定好的 `risk` 字段。

## 演进

- V1 引入"会话内 accept-for-session"语义后,Bridge 收到 `acceptForSession` 应记录到该 thread 的白名单,后续同类命令静默 accept(不再骚扰豆豆),但 **审计日志必须保留**。
- V1 之后考虑引入"组织级策略"(企业版),允许管理员预设不可被用户覆盖的硬性 `high` 规则。
