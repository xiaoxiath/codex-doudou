/** Minimal structured logger. Avoids pino dep for now. */

type Level = 'debug' | 'info' | 'warn' | 'error';

const LEVELS: Record<Level, number> = { debug: 10, info: 20, warn: 30, error: 40 };

const envLevel = (process.env.DOUDOU_LOG_LEVEL ?? 'info') as Level;
const threshold = LEVELS[envLevel] ?? LEVELS.info;

function emit(level: Level, ctx: Record<string, unknown> | string, msg?: string) {
  if (LEVELS[level] < threshold) return;
  const time = new Date().toISOString();
  if (typeof ctx === 'string') {
    process.stderr.write(`${time} ${level.toUpperCase()} ${ctx}\n`);
  } else {
    const line = msg ? `${msg} ${JSON.stringify(ctx)}` : JSON.stringify(ctx);
    process.stderr.write(`${time} ${level.toUpperCase()} ${line}\n`);
  }
}

export const log = {
  debug: (ctx: Record<string, unknown> | string, msg?: string) => emit('debug', ctx, msg),
  info: (ctx: Record<string, unknown> | string, msg?: string) => emit('info', ctx, msg),
  warn: (ctx: Record<string, unknown> | string, msg?: string) => emit('warn', ctx, msg),
  error: (ctx: Record<string, unknown> | string, msg?: string) => emit('error', ctx, msg),
};
