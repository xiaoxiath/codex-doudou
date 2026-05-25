import { mkdir, appendFile, readdir, readFile } from 'node:fs/promises';
import { join } from 'node:path';

import { log } from './log.js';

export interface AuditEntry {
  ts: string;
  device: string;
  thread?: string;
  request_id: string;
  action_type: string;
  risk: 'low' | 'medium' | 'high';
  command?: string;
  choice: string;
  latency_ms?: number;
  codex_response?: string;
}

export class AuditLog {
  private writing = Promise.resolve();

  constructor(private readonly dir: string) {}

  async init(): Promise<void> {
    await mkdir(this.dir, { recursive: true });
  }

  /**
   * Append serialized — never resolves until line is on disk.
   * Errors are logged but never thrown to caller (audit failure must not
   * break the user-facing reply path).
   */
  append(entry: AuditEntry): Promise<void> {
    const line = JSON.stringify(entry) + '\n';
    const date = entry.ts.slice(0, 10);
    const file = join(this.dir, `${date}.jsonl`);
    // serialize writes per-file to avoid interleaved partial lines on macOS
    this.writing = this.writing.then(() =>
      appendFile(file, line, 'utf8').catch((err) => {
        log.error({ err: String(err), file }, 'audit append failed');
      }),
    );
    return this.writing;
  }

  /**
   * Return the most recent N entries across all daily files, newest first.
   * Walks daily files from newest backwards until N is filled. Tolerates a
   * missing audit directory.
   */
  async readRecent(n: number): Promise<AuditEntry[]> {
    let files: string[];
    try {
      files = (await readdir(this.dir)).filter((f) => f.endsWith('.jsonl')).sort().reverse();
    } catch {
      return [];
    }
    const out: AuditEntry[] = [];
    for (const f of files) {
      const lines = (await readFile(join(this.dir, f), 'utf8')).split('\n').filter(Boolean);
      // newest-first within a file = reverse
      for (let i = lines.length - 1; i >= 0 && out.length < n; i--) {
        try {
          out.push(JSON.parse(lines[i] ?? '') as AuditEntry);
        } catch {
          /* skip bad line */
        }
      }
      if (out.length >= n) break;
    }
    return out;
  }
}
