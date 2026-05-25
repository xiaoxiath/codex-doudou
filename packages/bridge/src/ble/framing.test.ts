import { describe, expect, it } from 'vitest';

import { encodeMessage, FLAG_MORE, FLAG_RESET, Reassembler } from './framing.js';

describe('encodeMessage', () => {
  it('produces a single chunk when body fits in one MTU', () => {
    const chunks = encodeMessage('{"hi":1}', 247, 7);
    expect(chunks).toHaveLength(1);
    expect(chunks[0]![0]!).toBe(7);
    expect(chunks[0]![1]! & FLAG_MORE).toBe(0);
    expect(chunks[0]!.subarray(2).toString()).toBe('{"hi":1}');
  });

  it('splits across multiple chunks when body exceeds payload max', () => {
    // MTU 23 → payload max = 23 - 3 - 2 = 18 bytes per chunk
    const body = 'A'.repeat(50);
    const chunks = encodeMessage(body, 23, 1);
    expect(chunks).toHaveLength(3);
    // First two have MORE, last clears it
    expect(chunks[0]![1]! & FLAG_MORE).toBe(FLAG_MORE);
    expect(chunks[1]![1]! & FLAG_MORE).toBe(FLAG_MORE);
    expect(chunks[2]![1]! & FLAG_MORE).toBe(0);
    // All carry the same seq
    expect(chunks.every((c) => c[0] === 1)).toBe(true);
    // Concat payload reconstructs body
    const recombined = Buffer.concat(chunks.map((c) => c.subarray(2))).toString();
    expect(recombined).toBe(body);
  });

  it('uses a header-only chunk when body is empty', () => {
    const chunks = encodeMessage('', 247, 0);
    expect(chunks).toHaveLength(1);
    expect(chunks[0]!.length).toBe(2);
    expect(chunks[0]![1]! & FLAG_MORE).toBe(0);
  });

  it('clamps payload to at least 1 byte when MTU is degenerate', () => {
    const chunks = encodeMessage('hello', 4, 0); // MTU < hdr+1 → forced 1B per chunk
    expect(chunks.length).toBeGreaterThanOrEqual(5);
  });
});

describe('Reassembler', () => {
  it('returns the body when fed a single complete chunk', () => {
    const r = new Reassembler();
    const chunks = encodeMessage('{"x":42}', 247, 4);
    const got = r.feed(chunks[0]!);
    expect(got).toBe('{"x":42}');
  });

  it('roundtrips a multi-chunk message', () => {
    const r = new Reassembler();
    const body = JSON.stringify({ msg: 'A'.repeat(500) });
    const chunks = encodeMessage(body, 23, 99);
    let result: string | null = null;
    for (const c of chunks) {
      result = r.feed(c);
    }
    expect(result).toBe(body);
  });

  it('returns null for non-final chunks', () => {
    const r = new Reassembler();
    const chunks = encodeMessage('a'.repeat(100), 23, 5);
    expect(chunks.length).toBeGreaterThan(1);
    for (let i = 0; i < chunks.length - 1; i++) {
      expect(r.feed(chunks[i]!)).toBeNull();
    }
    expect(r.feed(chunks[chunks.length - 1]!)).toBe('a'.repeat(100));
  });

  it('resets on FLAG_RESET', () => {
    const r = new Reassembler();
    // Start a multi-chunk message and abandon it mid-flight by sending a RESET frame.
    const partial = encodeMessage('aaaaaaaaaa', 23, 1);
    r.feed(partial[0]!);                                    // still in progress
    const resetFrame = Buffer.from([7, FLAG_RESET, 0x68]);  // seq=7, RESET set, payload 'h'
    expect(r.feed(resetFrame)).toBe('h');
  });

  it('drops in-progress message when seq changes mid-stream', () => {
    const r = new Reassembler();
    const msgA = encodeMessage('A'.repeat(100), 23, 1);
    const msgB = encodeMessage('B'.repeat(100), 23, 2);
    // Half of A
    r.feed(msgA[0]!);
    r.feed(msgA[1]!);
    // Then all of B (different seq) — A should be discarded
    let result: string | null = null;
    for (const c of msgB) result = r.feed(c);
    expect(result).toBe('B'.repeat(100));
  });

  it('throws when accumulator would exceed maxBytes', () => {
    const r = new Reassembler(64);
    const chunks = encodeMessage('Z'.repeat(200), 23, 0);
    expect(() => {
      for (const c of chunks) r.feed(c);
    }).toThrow(/exceeds/);
  });

  it('rejects chunks smaller than the header', () => {
    const r = new Reassembler();
    expect(() => r.feed(Buffer.from([7]))).toThrow(/header/);
  });
});
