/**
 * Tiny static file server for the simulator UI.
 * Single-purpose — no directory listing, no symlink follow.
 */
import { createReadStream } from 'node:fs';
import { stat } from 'node:fs/promises';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { extname, join, normalize, resolve } from 'node:path';

const MIME: Record<string, string> = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.json': 'application/json; charset=utf-8',
  '.ico': 'image/x-icon',
};

export function makeStaticHandler(root: string) {
  const absRoot = resolve(root);
  return async function handle(
    req: IncomingMessage,
    res: ServerResponse,
    urlPath: string,
  ): Promise<void> {
    // strip query
    const clean = urlPath.split('?')[0] ?? '/';
    // resolve & ensure no path traversal
    const rel = normalize(clean === '/' ? '/index.html' : clean).replace(/^[/\\]+/, '');
    const abs = join(absRoot, rel);
    if (!abs.startsWith(absRoot)) {
      res.statusCode = 403;
      res.end('forbidden');
      return;
    }
    try {
      const st = await stat(abs);
      if (st.isDirectory()) {
        return handle(req, res, urlPath.replace(/\/$/, '') + '/index.html');
      }
      res.statusCode = 200;
      res.setHeader('content-type', MIME[extname(abs)] ?? 'application/octet-stream');
      res.setHeader('content-length', String(st.size));
      res.setHeader('cache-control', 'no-cache');
      createReadStream(abs).pipe(res);
    } catch {
      res.statusCode = 404;
      res.end(`not found: ${urlPath}`);
    }
  };
}
