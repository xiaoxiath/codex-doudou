import { describe, expect, it } from 'vitest';

import { normalizeRateLimits } from './threadListPoller.js';

describe('normalizeRateLimits', () => {
  it('handles the multi-model rateLimitsByLimitId payload (Pro plan + Spark)', () => {
    const input = {
      rateLimits: {
        limitId: 'codex',
        primary: { usedPercent: 9, windowDurationMins: 300, resetsAt: 1779379343 },
        secondary: { usedPercent: 9, windowDurationMins: 10080, resetsAt: 1779843610 },
        planType: 'pro',
      },
      rateLimitsByLimitId: {
        codex: {
          limitId: 'codex',
          primary: { usedPercent: 9, windowDurationMins: 300, resetsAt: 1779379343 },
          secondary: { usedPercent: 9, windowDurationMins: 10080, resetsAt: 1779843610 },
          planType: 'pro',
        },
        codex_bengalfox: {
          limitId: 'codex_bengalfox',
          limitName: 'GPT-5.3-Codex-Spark',
          primary: { usedPercent: 0, windowDurationMins: 300, resetsAt: 1779388180 },
          secondary: { usedPercent: 0, windowDurationMins: 10080, resetsAt: 1779974980 },
          planType: 'pro',
        },
      },
    };
    const snap = normalizeRateLimits(input);
    expect(snap.planType).toBe('pro');
    expect(snap.groups).toHaveLength(2);
    // codex always sorts first
    expect(snap.groups[0]?.id).toBe('codex');
    expect(snap.groups[0]?.label).toBeUndefined();
    expect(snap.groups[1]?.id).toBe('codex_bengalfox');
    expect(snap.groups[1]?.label).toBe('GPT-5.3-Codex-Spark');
    // epoch seconds → ms conversion
    expect(snap.groups[0]?.primary?.resets_at_ms).toBe(1779379343 * 1000);
    expect(snap.groups[1]?.primary?.used_percent).toBe(0);
  });

  it('falls back to .rateLimits when the per-id map is empty (older codex)', () => {
    const input = {
      rateLimits: {
        limitId: 'codex',
        primary: { usedPercent: 4, windowDurationMins: 300, resetsAt: 100 },
        secondary: { usedPercent: 8, windowDurationMins: 10080, resetsAt: 200 },
        planType: 'pro',
      },
    };
    const snap = normalizeRateLimits(input);
    expect(snap.planType).toBe('pro');
    expect(snap.groups).toHaveLength(1);
    expect(snap.groups[0]?.id).toBe('codex');
    expect(snap.groups[0]?.primary?.used_percent).toBe(4);
    expect(snap.groups[0]?.primary?.resets_at_ms).toBe(100_000);
  });

  it('returns empty groups when neither rateLimits nor byLimitId are usable', () => {
    expect(normalizeRateLimits({}).groups).toEqual([]);
    expect(normalizeRateLimits({ rateLimits: { primary: null, secondary: null } as never }).groups).toEqual([]);
  });

  it('skips groups with no primary and no secondary windows', () => {
    const snap = normalizeRateLimits({
      rateLimitsByLimitId: {
        codex_dead: {
          limitId: 'codex_dead',
          limitName: 'Dead',
          primary: null,
          secondary: null,
          planType: 'pro',
        },
        codex: {
          limitId: 'codex',
          primary: { usedPercent: 1, windowDurationMins: 300, resetsAt: 1 },
          planType: 'pro',
        },
      },
    });
    expect(snap.groups.map((g) => g.id)).toEqual(['codex']);
  });

  it('omits secondary when only primary is present', () => {
    const snap = normalizeRateLimits({
      rateLimitsByLimitId: {
        codex: {
          limitId: 'codex',
          primary: { usedPercent: 50, windowDurationMins: 300, resetsAt: 1 },
        },
      },
    });
    expect(snap.groups[0]?.primary?.used_percent).toBe(50);
    expect(snap.groups[0]?.secondary).toBeUndefined();
  });
});
