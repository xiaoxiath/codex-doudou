/**
 * Risk classification — translates Codex approval requests into the
 * `risk` and `action_type` fields surfaced on the device.
 *
 * Authoritative rules: docs/risk-policy.md. Keep this file the only place
 * those rules live in code; the policy doc and this module are meant to
 * stay in sync.
 */
import type { ActionType, Risk } from '@doudou/device-protocol';

export interface RiskDecision {
  risk: Risk;
  action_type: ActionType;
  /** Human-readable label for telemetry / audit. */
  reason: string;
}

/** Risk ordering, highest wins on aggregation. */
const RISK_RANK: Record<Risk, number> = { low: 0, medium: 1, high: 2 };

function escalate(a: Risk, b: Risk): Risk {
  return RISK_RANK[a] >= RISK_RANK[b] ? a : b;
}

// ---------- Commands ----------

const READ_ONLY_BINARIES = new Set([
  'ls',
  'pwd',
  'cat',
  'head',
  'tail',
  'wc',
  'file',
  'stat',
  'tree',
  'find',
  'grep',
  'rg',
  'ag',
  'fgrep',
  'egrep',
  'which',
  'whereis',
  'env',
  'printenv',
  'date',
  'whoami',
  'id',
  'hostname',
  'uname',
  'echo',
  'true',
  'false',
]);

const TEST_BINARIES = new Set([
  'npm', // see subcommand check below
  'pnpm',
  'yarn',
  'pytest',
  'cargo',
  'go',
  'gradle',
  'mvn',
  'jest',
  'vitest',
  'mocha',
]);

const NETWORK_BINARIES = new Set([
  'curl',
  'wget',
  'nc',
  'ncat',
  'ssh',
  'scp',
  'rsync',
  'ftp',
  'sftp',
  'telnet',
]);

const DESTRUCTIVE_BINARIES = new Set([
  'rm',
  'rmdir',
  'mv',
  'dd',
  'mkfs',
  'fdisk',
  'parted',
  'shred',
  'chmod',
  'chown',
  'chgrp',
  'kill',
  'pkill',
  'killall',
]);

const INSTALL_SUBCMD = new Set(['install', 'add', 'i', 'remove', 'uninstall', 'rm', 'update', 'upgrade']);
const TEST_SUBCMD = new Set(['test', 'run', 'build', 'tsc', 'lint', 'check', 'fmt', 'format']);

const GIT_HIGH_RISK_SUBCMD = new Set([
  'push',
  'reset',
  'rebase',
  'clean',
  'gc',
  'filter-branch',
  'filter-repo',
  'reflog',
]);

const SENSITIVE_PATH_PATTERNS = [
  /(^|\/)\.env(\.|$)/i,
  /\.(pem|key|p12|pfx|crt)$/i,
  /(^|\/)id_(rsa|ed25519|ecdsa|dsa)(\.pub)?$/,
  /credentials/i,
  /\.aws\//,
  /\.ssh\//,
];

const SHELL_METACHARS = /[;&|`$()<>]/;

export interface ClassifyCommandInput {
  command: string[]; // argv-style as provided by Codex
  cwd: string | null;
  /** Optional user workspace root. When supplied, paths escaping it count as cross-project. */
  workspace?: string;
}

export function classifyCommand(input: ClassifyCommandInput): RiskDecision {
  const argv = input.command;
  if (argv.length === 0) {
    return { risk: 'medium', action_type: 'run_command', reason: 'empty command' };
  }

  // If Codex hands us a single shell-style string, split conservatively for binary lookup.
  // We never *execute* — splitting is only for risk lookup.
  let bin = argv[0] ?? '';
  if (argv.length === 1 && /\s/.test(bin)) {
    const first = bin.trim().split(/\s+/)[0] ?? '';
    bin = stripPath(first);
  } else {
    bin = stripPath(bin);
  }

  const sub = argv[1];
  const joined = argv.join(' ');

  // Shell metachars often indicate chained / redirected commands — escalate.
  const hasShellChain = SHELL_METACHARS.test(joined);

  // Build component decisions; aggregate by taking max.
  const decisions: RiskDecision[] = [];

  // network
  if (NETWORK_BINARIES.has(bin)) {
    decisions.push({ risk: 'high', action_type: 'network_access', reason: `network binary ${bin}` });
  }

  // destructive
  if (DESTRUCTIVE_BINARIES.has(bin) || bin === 'sudo') {
    decisions.push({
      risk: 'high',
      action_type: 'run_command',
      reason: bin === 'sudo' ? 'privilege escalation' : `destructive binary ${bin}`,
    });
  }

  // git subcommands
  if (bin === 'git' && sub) {
    if (GIT_HIGH_RISK_SUBCMD.has(sub) || (sub === 'branch' && argv.includes('-D'))) {
      decisions.push({
        risk: 'high',
        action_type: 'run_command',
        reason: `git ${sub} affects history or remote`,
      });
    } else {
      // git commit, git status, git log, git add, etc. — medium / low
      const lowSubs = new Set(['status', 'log', 'diff', 'show', 'blame', 'fetch', 'remote', 'config', 'branch', 'tag']);
      decisions.push({
        risk: lowSubs.has(sub) ? 'low' : 'medium',
        action_type: 'run_command',
        reason: `git ${sub}`,
      });
    }
  }

  // installs / test runs through package managers
  if (TEST_BINARIES.has(bin) && sub) {
    if (INSTALL_SUBCMD.has(sub)) {
      decisions.push({
        risk: 'medium',
        action_type: 'run_command',
        reason: `${bin} ${sub} touches dependencies`,
      });
    } else if (TEST_SUBCMD.has(sub)) {
      decisions.push({
        risk: 'medium',
        action_type: 'run_command',
        reason: `${bin} ${sub} build/test step`,
      });
    }
  }

  // read-only
  if (READ_ONLY_BINARIES.has(bin) && decisions.length === 0) {
    decisions.push({ risk: 'low', action_type: 'run_command', reason: `read-only ${bin}` });
  }

  // cwd outside workspace
  if (input.workspace && input.cwd) {
    const normCwd = stripTrailing(input.cwd);
    const normWs = stripTrailing(input.workspace);
    if (!normCwd.startsWith(normWs)) {
      decisions.push({
        risk: 'high',
        action_type: 'run_command',
        reason: `cwd outside workspace (${normCwd})`,
      });
    }
  }

  // shell chaining — at least medium, and never overrides higher risk
  if (hasShellChain) {
    decisions.push({
      risk: 'medium',
      action_type: 'run_command',
      reason: 'shell chaining / redirection',
    });
    // also scan the joined string for destructive / network tokens hidden
    // inside a chained command line — `git status && rm -rf` should be high.
    const tokens = joined.toLowerCase().split(/[\s|&;]+/);
    for (const tok of tokens) {
      const bare = tok.replace(/^.*[/\\]/, '');
      if (DESTRUCTIVE_BINARIES.has(bare) || bare === 'sudo') {
        decisions.push({
          risk: 'high',
          action_type: 'run_command',
          reason: `destructive token ${bare} inside shell chain`,
        });
      } else if (NETWORK_BINARIES.has(bare)) {
        decisions.push({
          risk: 'high',
          action_type: 'network_access',
          reason: `network token ${bare} inside shell chain`,
        });
      }
    }
  }

  if (decisions.length === 0) {
    return { risk: 'medium', action_type: 'run_command', reason: `unknown binary ${bin}` };
  }

  return decisions.reduce((acc, d) => ({
    risk: escalate(acc.risk, d.risk),
    action_type: d.risk === acc.risk ? acc.action_type : d.action_type,
    // pick the reason from whichever decision had the highest risk
    reason:
      RISK_RANK[d.risk] > RISK_RANK[acc.risk]
        ? d.reason
        : RISK_RANK[d.risk] === RISK_RANK[acc.risk]
          ? `${acc.reason}; ${d.reason}`
          : acc.reason,
  }));
}

// ---------- File changes ----------

export interface FileChange {
  path: string;
  kind: 'create' | 'modify' | 'delete';
  /** Approximate line count of the change, when available. */
  diffLines?: number;
}

export function classifyFileChange(changes: FileChange[]): RiskDecision {
  if (changes.length === 0) {
    return { risk: 'medium', action_type: 'modify_file', reason: 'empty change set' };
  }

  let risk: Risk = 'low';
  const reasons: string[] = [];

  for (const c of changes) {
    // Sensitive paths win immediately.
    if (SENSITIVE_PATH_PATTERNS.some((re) => re.test(c.path))) {
      return {
        risk: 'high',
        action_type: 'modify_file',
        reason: `sensitive file ${c.path}`,
      };
    }
    if (/(^|\/)\.git\//.test(c.path)) {
      return {
        risk: 'high',
        action_type: 'modify_file',
        reason: `.git internals modified (${c.path})`,
      };
    }
    if (c.path.includes('..')) {
      return {
        risk: 'high',
        action_type: 'modify_file',
        reason: `path escapes project (${c.path})`,
      };
    }
    if (c.kind === 'delete') {
      return {
        risk: 'high',
        action_type: 'modify_file',
        reason: `deletion of ${c.path}`,
      };
    }
    if (c.kind === 'modify' && (c.diffLines ?? 0) > 50) {
      risk = escalate(risk, 'medium');
      reasons.push(`large modify ${c.path}`);
    } else {
      // create or small modify
      risk = escalate(risk, 'low');
      reasons.push(`${c.kind} ${c.path}`);
    }
  }

  return {
    risk,
    action_type: 'modify_file',
    reason: reasons.slice(0, 3).join(', ') + (reasons.length > 3 ? ` (+${reasons.length - 3})` : ''),
  };
}

// ---------- Tool calls ----------

export interface ToolCallInput {
  toolName: string;
  /** From MCP tool metadata when known; undefined means we can't tell. */
  sideEffectsHint?: boolean;
}

const HIGH_RISK_TOOL_PATTERNS = [/send/i, /post/i, /publish/i, /pay/i, /deploy/i, /charge/i];
const WRITE_TOOL_PATTERNS = [/write/i, /create/i, /update/i, /delete/i, /move/i, /rename/i, /patch/i];
const READ_TOOL_PATTERNS = [/search/i, /^read/i, /^list/i, /^get/i, /^fetch/i, /lookup/i, /query/i];

export function classifyToolCall(input: ToolCallInput): RiskDecision {
  const name = input.toolName;

  if (HIGH_RISK_TOOL_PATTERNS.some((re) => re.test(name))) {
    return { risk: 'high', action_type: 'tool_call', reason: `tool name matches outbound pattern (${name})` };
  }
  if (input.sideEffectsHint === false) {
    return { risk: 'low', action_type: 'tool_call', reason: `tool declared no side effects (${name})` };
  }
  if (WRITE_TOOL_PATTERNS.some((re) => re.test(name))) {
    return { risk: 'medium', action_type: 'tool_call', reason: `tool name suggests write (${name})` };
  }
  if (READ_TOOL_PATTERNS.some((re) => re.test(name))) {
    return { risk: 'low', action_type: 'tool_call', reason: `tool name suggests read (${name})` };
  }
  return { risk: 'medium', action_type: 'tool_call', reason: `unknown tool ${name}` };
}

// ---------- Helpers ----------

function stripPath(s: string): string {
  const m = s.match(/[^/\\]+$/);
  return (m?.[0] ?? s).toLowerCase();
}
function stripTrailing(s: string): string {
  return s.replace(/[/\\]+$/, '');
}
