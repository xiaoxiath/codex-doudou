/**
 * One-shot probe to fetch the user's account info via codex app-server
 * `account/read` JSON-RPC. Used at Bridge startup so the device info
 * screen knows whose Codex this is.
 *
 * Doesn't keep a long-lived subprocess — spawns, asks, exits. Cheap (~1-2s).
 */
import { JsonRpcStdioClient } from './jsonRpcClient.js';
import { log } from '../log.js';

export interface AccountInfo {
  email?: string;
  planType?: string;
  authMode?: string;
}

interface AccountReadResult {
  account?: {
    type?: string;
    email?: string;
    planType?: string;
  };
}

const PROBE_TIMEOUT_MS = 8_000;

export async function probeAccount(): Promise<AccountInfo | null> {
  const rpc = new JsonRpcStdioClient('codex', ['app-server']);
  try {
    await rpc.start();
    const initPromise = rpc.request('initialize', {
      clientInfo: { name: 'doudou-bridge-probe', version: '0.1.0' },
      capabilities: null,
    });
    await withTimeout(initPromise, PROBE_TIMEOUT_MS, 'initialize');
    rpc.notify('initialized');
    const account = await withTimeout(
      rpc.request<AccountReadResult>('account/read', {}),
      PROBE_TIMEOUT_MS,
      'account/read',
    );
    return {
      ...(account.account?.email ? { email: account.account.email } : {}),
      ...(account.account?.planType ? { planType: account.account.planType } : {}),
      ...(account.account?.type ? { authMode: account.account.type } : {}),
    };
  } catch (err) {
    log.warn({ err: String(err) }, 'account probe failed, info screen will lack email/plan');
    return null;
  } finally {
    await rpc.stop().catch(() => undefined);
  }
}

function withTimeout<T>(p: Promise<T>, ms: number, what: string): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${what} timed out after ${ms}ms`)), ms);
    timer.unref?.();
    p.then(
      (v) => { clearTimeout(timer); resolve(v); },
      (e) => { clearTimeout(timer); reject(e); },
    );
  });
}
