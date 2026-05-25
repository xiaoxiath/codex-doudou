/**
 * BLE chunked-JSON framing — pure functions, no BLE deps.
 *
 * Wire format (each chunk):
 *   byte 0 = sequence number (0..255, wraps independently per direction)
 *   byte 1 = flags
 *              bit 0 (MORE)  — 1 = more fragments follow, 0 = last fragment
 *              bit 1 (RESET) — 1 = drop receiver's accumulator before this fragment
 *   bytes 2+ = payload (UTF-8 JSON, split across chunks)
 *
 * Must match `packages/firmware/main/ble_transport.c`.
 */

export const FLAG_MORE = 0x01;
export const FLAG_RESET = 0x02;

const HDR_LEN = 2;

/**
 * Split a JSON string into BLE-sized chunks ready for `characteristic.write`.
 *
 * @param body         UTF-8 string (typically `JSON.stringify(...)` output)
 * @param mtu          negotiated ATT MTU (e.g. 247). Subtract 3 bytes ATT
 *                     overhead + 2 bytes for our header → max payload per chunk.
 * @param seq          sequence number to stamp on every chunk in this message
 */
export function encodeMessage(body: string, mtu: number, seq: number): Buffer[] {
  const data = Buffer.from(body, 'utf-8');
  const payloadMax = Math.max(1, mtu - 3 - HDR_LEN);
  const chunks: Buffer[] = [];
  for (let off = 0; off < data.length; off += payloadMax) {
    const slice = data.subarray(off, Math.min(off + payloadMax, data.length));
    const more = off + payloadMax < data.length;
    const chunk = Buffer.allocUnsafe(HDR_LEN + slice.length);
    chunk[0] = seq & 0xff;
    chunk[1] = more ? FLAG_MORE : 0;
    slice.copy(chunk, HDR_LEN);
    chunks.push(chunk);
  }
  // Empty body still gets one (header-only, no MORE) frame so the
  // receiver knows a "message" arrived. Unlikely in practice.
  if (chunks.length === 0) {
    const chunk = Buffer.allocUnsafe(HDR_LEN);
    chunk[0] = seq & 0xff;
    chunk[1] = 0;
    chunks.push(chunk);
  }
  return chunks;
}

/**
 * Accumulates inbound fragments and yields completed messages.
 *
 * The state is intentionally simple — a single buffer + max-len guard.
 * If the sender violates protocol (e.g. interleaves messages with
 * different seqs), we drop and resync on the next frame whose flags
 * include RESET, or on the first frame after we discard an in-progress
 * message because of overflow.
 */
export class Reassembler {
  private buf: Buffer[] = [];
  private size = 0;
  /** Most recent seq we've seen mid-message. */
  private currentSeq: number | null = null;

  constructor(public readonly maxBytes = 16 * 1024) {}

  /**
   * Feed one chunk. Returns the completed JSON string when this chunk
   * carried the final fragment (flags MORE=0); otherwise null.
   * Throws on malformed input.
   */
  feed(chunk: Buffer): string | null {
    if (chunk.length < HDR_LEN) {
      throw new Error('ble framing: chunk shorter than header');
    }
    const seq = chunk[0]!;
    const flags = chunk[1]!;
    const payload = chunk.subarray(HDR_LEN);

    if (flags & FLAG_RESET) this.reset();

    // Different seq mid-message → previous one is abandoned.
    if (this.currentSeq !== null && this.currentSeq !== seq) {
      this.reset();
    }
    this.currentSeq = seq;

    if (this.size + payload.length > this.maxBytes) {
      this.reset();
      throw new Error(`ble framing: message exceeds ${this.maxBytes} bytes`);
    }
    if (payload.length > 0) {
      this.buf.push(payload);
      this.size += payload.length;
    }

    if ((flags & FLAG_MORE) === 0) {
      const joined = Buffer.concat(this.buf, this.size).toString('utf-8');
      this.reset();
      return joined;
    }
    return null;
  }

  reset(): void {
    this.buf = [];
    this.size = 0;
    this.currentSeq = null;
  }
}
