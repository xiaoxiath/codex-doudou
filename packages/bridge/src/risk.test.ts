import { describe, expect, it } from 'vitest';

import { classifyCommand, classifyFileChange, classifyToolCall } from './risk.js';

describe('classifyCommand', () => {
  const cases: Array<{ name: string; cmd: string[]; cwd?: string; expect: { risk: string; action_type: string } }> = [
    { name: 'ls is low', cmd: ['ls', '-la'], expect: { risk: 'low', action_type: 'run_command' } },
    { name: 'git status is low', cmd: ['git', 'status'], expect: { risk: 'low', action_type: 'run_command' } },
    { name: 'git push is high', cmd: ['git', 'push', 'origin', 'main'], expect: { risk: 'high', action_type: 'run_command' } },
    { name: 'git reset --hard is high', cmd: ['git', 'reset', '--hard', 'HEAD~3'], expect: { risk: 'high', action_type: 'run_command' } },
    { name: 'git commit is medium', cmd: ['git', 'commit', '-m', 'wip'], expect: { risk: 'medium', action_type: 'run_command' } },
    { name: 'npm test is medium', cmd: ['npm', 'test'], expect: { risk: 'medium', action_type: 'run_command' } },
    { name: 'npm install is medium', cmd: ['npm', 'install', 'lodash'], expect: { risk: 'medium', action_type: 'run_command' } },
    { name: 'rm -rf is high', cmd: ['rm', '-rf', 'build'], expect: { risk: 'high', action_type: 'run_command' } },
    { name: 'sudo is high', cmd: ['sudo', 'systemctl', 'restart', 'nginx'], expect: { risk: 'high', action_type: 'run_command' } },
    { name: 'curl is high (network)', cmd: ['curl', 'https://example.com'], expect: { risk: 'high', action_type: 'network_access' } },
    { name: 'unknown bin is medium', cmd: ['foo-tool', '--bar'], expect: { risk: 'medium', action_type: 'run_command' } },
    { name: 'shell chain escalates to medium when chars present', cmd: ['cat', 'foo', '|', 'grep', 'bar'], expect: { risk: 'medium', action_type: 'run_command' } },
  ];
  for (const tc of cases) {
    it(tc.name, () => {
      const d = classifyCommand({ command: tc.cmd, cwd: tc.cwd ?? null });
      expect({ risk: d.risk, action_type: d.action_type }).toEqual(tc.expect);
    });
  }

  it('shell chaining inside a single shell-style argument escalates', () => {
    const d = classifyCommand({ command: ['git status && rm -rf /tmp/x'], cwd: null });
    expect(d.risk).toBe('high'); // rm is destructive
  });

  it('cwd outside workspace forces high', () => {
    const d = classifyCommand({
      command: ['ls'],
      cwd: '/etc',
      workspace: '/home/me/proj',
    });
    expect(d.risk).toBe('high');
  });
});

describe('classifyFileChange', () => {
  it('small create is low', () => {
    const d = classifyFileChange([{ path: 'src/foo.ts', kind: 'create' }]);
    expect(d.risk).toBe('low');
  });

  it('small modify is low', () => {
    const d = classifyFileChange([{ path: 'src/foo.ts', kind: 'modify', diffLines: 10 }]);
    expect(d.risk).toBe('low');
  });

  it('large modify is medium', () => {
    const d = classifyFileChange([{ path: 'src/foo.ts', kind: 'modify', diffLines: 500 }]);
    expect(d.risk).toBe('medium');
  });

  it('delete is high', () => {
    const d = classifyFileChange([{ path: 'src/foo.ts', kind: 'delete' }]);
    expect(d.risk).toBe('high');
  });

  it('.env touch is high regardless of kind', () => {
    const d = classifyFileChange([{ path: '.env', kind: 'modify', diffLines: 1 }]);
    expect(d.risk).toBe('high');
  });

  it('.git internals touch is high', () => {
    const d = classifyFileChange([{ path: '.git/config', kind: 'modify', diffLines: 1 }]);
    expect(d.risk).toBe('high');
  });

  it('path escape (..) is high', () => {
    const d = classifyFileChange([{ path: '../../etc/passwd', kind: 'modify' }]);
    expect(d.risk).toBe('high');
  });

  it('mixed bag: one high among lows yields high', () => {
    const d = classifyFileChange([
      { path: 'src/a.ts', kind: 'create' },
      { path: '.ssh/id_rsa', kind: 'modify', diffLines: 1 },
      { path: 'src/b.ts', kind: 'modify', diffLines: 2 },
    ]);
    expect(d.risk).toBe('high');
  });
});

describe('classifyToolCall', () => {
  it('declared no-side-effects is low', () => {
    const d = classifyToolCall({ toolName: 'do_something', sideEffectsHint: false });
    expect(d.risk).toBe('low');
  });
  it('read/search tools are low', () => {
    expect(classifyToolCall({ toolName: 'search_repo' }).risk).toBe('low');
    expect(classifyToolCall({ toolName: 'read_file' }).risk).toBe('low');
    expect(classifyToolCall({ toolName: 'get_user' }).risk).toBe('low');
  });
  it('write tools are medium', () => {
    expect(classifyToolCall({ toolName: 'write_file' }).risk).toBe('medium');
    expect(classifyToolCall({ toolName: 'update_issue' }).risk).toBe('medium');
  });
  it('outbound-pattern tools are high', () => {
    expect(classifyToolCall({ toolName: 'send_email' }).risk).toBe('high');
    expect(classifyToolCall({ toolName: 'deploy_to_prod' }).risk).toBe('high');
    expect(classifyToolCall({ toolName: 'publish_post' }).risk).toBe('high');
  });
  it('unknown is medium', () => {
    expect(classifyToolCall({ toolName: 'mysterious_tool' }).risk).toBe('medium');
  });
});
