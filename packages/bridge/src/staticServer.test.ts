/**
 * staticServer.ts — small file-serving smoke tests.
 *
 * We mount the handler on a real http.Server (ephemeral port), then
 * fetch a few URLs and assert status / content-type. Real `fs` IO so
 * we also catch path-resolution / traversal-guard bugs.
 */
import { createServer, type Server } from 'node:http';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { makeStaticHandler } from './staticServer.js';

let root: string;
let server: Server;
let baseUrl: string;

beforeEach(async () => {
  root = mkdtempSync(join(tmpdir(), 'doudou-static-'));
  writeFileSync(join(root, 'index.html'), '<!doctype html><title>doudou</title>');
  writeFileSync(join(root, 'main.js'),    'console.log("hi");');
  writeFileSync(join(root, 'style.css'),  'body { background: #000; }');
  writeFileSync(join(root, 'favicon.svg'), '<svg></svg>');
  mkdirSync(join(root, 'pet-art'));
  writeFileSync(join(root, 'pet-art', 'body.png'), Buffer.from([0x89, 0x50, 0x4e, 0x47]));

  const handler = makeStaticHandler(root);
  server = createServer(async (req, res) => {
    await handler(req, res, req.url ?? '/');
  });
  await new Promise<void>((r) => server.listen(0, () => r()));
  const addr = server.address();
  if (!addr || typeof addr === 'string') throw new Error('no address');
  baseUrl = `http://localhost:${addr.port}`;
});
afterEach(async () => {
  await new Promise<void>((r) => server.close(() => r()));
  rmSync(root, { recursive: true, force: true });
});

describe('makeStaticHandler', () => {
  it('serves /index.html when requesting /', async () => {
    const r = await fetch(`${baseUrl}/`);
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toMatch(/text\/html/);
    expect(await r.text()).toContain('doudou');
  });

  it('serves a JS file with the correct MIME', async () => {
    const r = await fetch(`${baseUrl}/main.js`);
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toMatch(/javascript/);
  });

  it('serves a CSS file with the correct MIME', async () => {
    const r = await fetch(`${baseUrl}/style.css`);
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toMatch(/text\/css/);
  });

  it('serves an SVG file', async () => {
    const r = await fetch(`${baseUrl}/favicon.svg`);
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toMatch(/image\/svg/);
  });

  it('serves binary files (PNG) with the right MIME', async () => {
    const r = await fetch(`${baseUrl}/pet-art/body.png`);
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toMatch(/image\/png/);
    const buf = Buffer.from(await r.arrayBuffer());
    expect(buf.length).toBe(4);
    expect(buf[0]).toBe(0x89); // PNG header byte
  });

  it('returns 404 for a missing file', async () => {
    const r = await fetch(`${baseUrl}/no-such-file.html`);
    expect(r.status).toBe(404);
  });

  it('blocks path traversal with 403', async () => {
    /* Try to escape the root. The handler normalises path then rejects
     * anything that doesn't stay under root. fetch URL-encodes ".." which
     * the server decodes, then normalizes, then guards via startsWith. */
    const r = await fetch(`${baseUrl}/../../../../etc/passwd`);
    expect([403, 404]).toContain(r.status); // 404 is also fine if normalize landed inside root
    // The important thing: we did NOT leak the file content.
    const body = await r.text();
    expect(body).not.toMatch(/root:.*:0:0/);
  });

  it('strips query string before resolving the path', async () => {
    const r = await fetch(`${baseUrl}/main.js?v=42`);
    expect(r.status).toBe(200);
    expect(r.headers.get('content-type')).toMatch(/javascript/);
  });

  it('serves index.html when requesting a directory path', async () => {
    mkdirSync(join(root, 'subdir'));
    writeFileSync(join(root, 'subdir', 'index.html'), '<p>subdir</p>');
    const r = await fetch(`${baseUrl}/subdir/`);
    expect(r.status).toBe(200);
    expect(await r.text()).toContain('subdir');
  });
});
