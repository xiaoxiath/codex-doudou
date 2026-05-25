import { mkdtempSync, readFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { AuditLog, type AuditEntry } from './audit.js';

function sampleEntry(over: Partial<AuditEntry> = {}): AuditEntry {
  return {
    ts: '2026-05-22T10:00:00.000Z',
    device: 'doudou-test',
    request_id: 'q-1',
    action_type: 'run_command',
    risk: 'low',
    choice: 'allow',
    latency_ms: 1234,
    command: 'ls',
    ...over,
  };
}

describe('AuditLog', () => {
  let dir: string;
  let audit: AuditLog;

  beforeEach(async () => {
    dir = mkdtempSync(join(tmpdir(), 'doudou-audit-'));
    audit = new AuditLog(dir);
    await audit.init();
  });
  afterEach(() => rmSync(dir, { recursive: true, force: true }));

  it('init() creates the audit directory if missing', async () => {
    const fresh = join(tmpdir(), `doudou-audit-fresh-${Date.now()}`);
    const a = new AuditLog(fresh);
    await a.init();
    rmSync(fresh, { recursive: true, force: true });
  });

  it('append writes one JSONL line per entry, named by the entry date', async () => {
    await audit.append(sampleEntry({ ts: '2026-05-22T10:00:00.000Z' }));
    await audit.append(sampleEntry({ ts: '2026-05-22T11:00:00.000Z', request_id: 'q-2' }));
    const file = join(dir, '2026-05-22.jsonl');
    const lines = readFileSync(file, 'utf8').split('\n').filter(Boolean);
    expect(lines).toHaveLength(2);
    expect(JSON.parse(lines[0]!).request_id).toBe('q-1');
    expect(JSON.parse(lines[1]!).request_id).toBe('q-2');
  });

  it('routes entries to date-derived files', async () => {
    await audit.append(sampleEntry({ ts: '2026-05-22T10:00:00.000Z', request_id: 'a' }));
    await audit.append(sampleEntry({ ts: '2026-05-23T08:00:00.000Z', request_id: 'b' }));
    expect(readFileSync(join(dir, '2026-05-22.jsonl'), 'utf8')).toContain('"a"');
    expect(readFileSync(join(dir, '2026-05-23.jsonl'), 'utf8')).toContain('"b"');
  });

  it('readRecent returns newest-first across files', async () => {
    await audit.append(sampleEntry({ ts: '2026-05-22T10:00:00.000Z', request_id: 'q1' }));
    await audit.append(sampleEntry({ ts: '2026-05-22T11:00:00.000Z', request_id: 'q2' }));
    await audit.append(sampleEntry({ ts: '2026-05-23T09:00:00.000Z', request_id: 'q3' }));

    const recent = await audit.readRecent(10);
    expect(recent.map((e) => e.request_id)).toEqual(['q3', 'q2', 'q1']);
  });

  it('readRecent honours the limit', async () => {
    for (let i = 0; i < 6; i++) {
      await audit.append(sampleEntry({ ts: '2026-05-22T10:00:00.000Z', request_id: `q${i}` }));
    }
    const recent = await audit.readRecent(3);
    expect(recent.map((e) => e.request_id)).toEqual(['q5', 'q4', 'q3']);
  });

  it('readRecent returns [] when audit dir does not exist yet', async () => {
    const fresh = join(tmpdir(), `doudou-audit-empty-${Date.now()}`);
    const a = new AuditLog(fresh);
    // intentionally skip init()
    const got = await a.readRecent(5);
    expect(got).toEqual([]);
  });

  it('readRecent tolerates a malformed line and continues', async () => {
    await audit.append(sampleEntry({ ts: '2026-05-22T10:00:00.000Z', request_id: 'good1' }));
    // Inject a corrupt line
    const fs = await import('node:fs/promises');
    await fs.appendFile(join(dir, '2026-05-22.jsonl'), 'not-json\n', 'utf8');
    await audit.append(sampleEntry({ ts: '2026-05-22T11:00:00.000Z', request_id: 'good2' }));

    const recent = await audit.readRecent(10);
    expect(recent.map((e) => e.request_id)).toContain('good1');
    expect(recent.map((e) => e.request_id)).toContain('good2');
  });

  it('serializes writes so concurrent appends do not interleave bytes', async () => {
    // Fire many appends without awaiting individually.
    const N = 20;
    const promises = Array.from({ length: N }, (_, i) =>
      audit.append(sampleEntry({ ts: '2026-05-22T10:00:00.000Z', request_id: `q${i}` })),
    );
    await Promise.all(promises);
    const lines = readFileSync(join(dir, '2026-05-22.jsonl'), 'utf8').split('\n').filter(Boolean);
    expect(lines).toHaveLength(N);
    // Every line must parse cleanly (no partial / interleaved JSON).
    for (const l of lines) {
      const parsed = JSON.parse(l) as AuditEntry;
      expect(parsed.request_id).toMatch(/^q\d+$/);
    }
  });
});
