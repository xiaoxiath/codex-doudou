import { describe, expect, it } from 'vitest';

import { normalizeEnvelope, normalizeForDevice } from './textNormalize.js';

describe('normalizeForDevice', () => {
  it('passes pure ASCII unchanged (fast path)', () => {
    const s = 'Codex thread 42 done.';
    expect(normalizeForDevice(s)).toBe(s);
  });

  it('passes pure CJK letters unchanged', () => {
    expect(normalizeForDevice('修复启动缓慢')).toBe('修复启动缓慢');
  });

  it('replaces CJK sentence punctuation', () => {
    expect(normalizeForDevice('完成了。')).toBe('完成了.');
    expect(normalizeForDevice('A，B、C；D：E！F？')).toBe('A,B,C;D:E!F?');
  });

  it('flattens curly quotes and CJK brackets', () => {
    expect(normalizeForDevice('「Codex」')).toBe('"Codex"');
    expect(normalizeForDevice('“hi”')).toBe('"hi"');
    expect(normalizeForDevice('《book》')).toBe('<book>');
  });

  it('collapses ellipsis and em/en dashes', () => {
    expect(normalizeForDevice('稍等…')).toBe('稍等...');
    expect(normalizeForDevice('A——B')).toBe('A--B');
    expect(normalizeForDevice('A–B')).toBe('A-B');
  });
});

describe('normalizeEnvelope', () => {
  it('walks nested objects/arrays in place', () => {
    const msg = {
      type: 'question',
      title: '运行命令？',
      choices: [
        { id: 'a', label: '允许' },
        { id: 'b', label: '拒绝（默认）' },
      ],
      meta: { tooltip: '高风险：写盘' },
    };
    normalizeEnvelope(msg);
    expect(msg.title).toBe('运行命令?');
    expect(msg.choices[0]?.label).toBe('允许');
    expect(msg.choices[1]?.label).toBe('拒绝(默认)');
    expect(msg.meta.tooltip).toBe('高风险:写盘');
  });

  it('leaves numbers/booleans alone', () => {
    const msg = { seq: 7, expired: false, label: '完成。' };
    normalizeEnvelope(msg);
    expect(msg).toEqual({ seq: 7, expired: false, label: '完成.' });
  });
});
