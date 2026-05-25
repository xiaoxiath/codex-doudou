/**
 * scanner.ts — only the "noble not installed" path is testable in CI.
 * The real scan/connect/GATT flow lives behind a dynamic import of
 * @abandonware/noble (native bindings) which we don't ship as a
 * dependency, and would require actual BLE hardware to verify anyway.
 */
import { describe, expect, it } from 'vitest';

import { startBleScanner } from './scanner.js';
import { DeviceRegistry } from '../deviceRegistry.js';

describe('startBleScanner', () => {
  it('returns null when @abandonware/noble is not installed', async () => {
    const handle = await startBleScanner({
      registry: new DeviceRegistry(),
      pairingToken: 'tok-12345',
      onReply: async () => undefined,
    });
    expect(handle).toBeNull();
  });
});
