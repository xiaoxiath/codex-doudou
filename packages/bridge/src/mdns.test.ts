/**
 * mdns.ts — smoke test that the announcer constructs + stops cleanly.
 * We can't easily verify the actual mDNS packets are on the LAN from a
 * test, but we *can* assert the handle shape + safe teardown so a
 * regression that throws / leaks the bonjour instance gets caught.
 */
import { describe, expect, it } from 'vitest';

import { announceBridge } from './mdns.js';

describe('announceBridge', () => {
  it('returns a handle whose stop() resolves', async () => {
    const h = announceBridge(8788, 1);
    expect(h).toBeTruthy();
    expect(typeof h.stop).toBe('function');
    // Stop should resolve (eventually) without throwing.
    await expect(h.stop()).resolves.toBeUndefined();
  });

  it('stop() called twice does not throw', async () => {
    const h = announceBridge(0, 1);  // port 0 = OS picks
    await h.stop();
    await expect(h.stop()).resolves.toBeUndefined();
  });
});
