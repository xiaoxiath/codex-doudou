#!/usr/bin/env node
import { startBridge } from './bridge.js';
import { log } from './log.js';

const port = Number(process.env.DOUDOU_PORT ?? 8788);
const pairingToken = process.env.DOUDOU_PAIRING_TOKEN ?? 'dev-token-change-me';
// Default mode: real Codex + rollout follow. Doudou is a passive observer
// of whatever the user does in Codex Desktop/CLI/VS Code. The `own` and
// `stub` modes remain available via env vars for testing.
const codexMode = (process.env.DOUDOU_CODEX ?? 'real') as 'stub' | 'real';
const auditDir = process.env.DOUDOU_AUDIT_DIR ?? './audit';
const workspace = process.env.DOUDOU_WORKSPACE ?? process.cwd();
const approvalPolicy = (process.env.DOUDOU_APPROVAL ?? 'on-request') as 'untrusted' | 'on-failure' | 'on-request' | 'never';
const sourceMode = (process.env.DOUDOU_SOURCE ?? 'follow') as 'follow' | 'own' | 'auto';
const enableBle = process.env.DOUDOU_BLE === '1';

const bridge = await startBridge({
  port,
  pairingToken,
  codexMode,
  auditDir,
  workspace,
  codexApprovalPolicy: approvalPolicy,
  codexSource: sourceMode,
  ble: enableBle,
});

const shutdown = async (sig: string) => {
  log.info({ sig }, 'shutting down');
  await bridge.stop();
  process.exit(0);
};
process.on('SIGINT', () => void shutdown('SIGINT'));
process.on('SIGTERM', () => void shutdown('SIGTERM'));
