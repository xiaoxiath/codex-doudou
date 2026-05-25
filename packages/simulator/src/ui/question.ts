/**
 * Question overlay renderer for the main slide.
 * Handles two-step confirmation (require_confirm) and queue badge.
 */
import type { Question } from '@doudou/device-protocol';

import { applyMarquee } from './marquee.js';

const ACTION_LABEL: Record<string, string> = {
  run_command: '运行命令',
  modify_file: '修改文件',
  network_access: '访问网络',
  user_input: '需要输入',
  tool_call: '工具调用',
  other: '其他',
};

export interface QuestionHandlers {
  /** User chose a choice (after confirmation if required). */
  onCommit: (choiceId: string) => void;
  /** Effective queue size (includes current). */
  queueTotal: number;
}

export function renderQuestion(msg: Question, handlers: QuestionHandlers): void {
  const layer = document.getElementById('question-layer') as HTMLElement;
  const slide = layer.closest('.slide-main') as HTMLElement;
  const device = document.querySelector('.device') as HTMLElement;

  slide.classList.add('has-question');
  device.classList.remove('risk-low', 'risk-medium', 'risk-high');
  device.classList.add(`risk-${msg.risk}`);

  layer.replaceChildren();

  // meta line
  const meta = document.createElement('div');
  meta.className = 'q-meta';
  meta.textContent = `${msg.risk.toUpperCase()} · ${ACTION_LABEL[msg.action_type] ?? msg.action_type}`;
  layer.appendChild(meta);

  // title (marquee if overflow)
  const titleEl = document.createElement('div');
  titleEl.className = 'q-title';
  layer.appendChild(titleEl);
  applyMarquee(titleEl, msg.title);

  if (msg.body) {
    const bodyEl = document.createElement('div');
    bodyEl.className = 'q-body';
    layer.appendChild(bodyEl);
    applyMarquee(bodyEl, msg.body);
  }

  let stage: 'choose' | 'confirm' = 'choose';
  let pendingChoice: string | null = null;

  const render = (): void => {
    // remove existing confirm + choices
    layer.querySelectorAll('.q-confirm, .q-choices, .q-queue').forEach((n) => n.remove());

    if (stage === 'confirm') {
      const chosen = msg.choices.find((c) => c.id === pendingChoice);
      const banner = document.createElement('div');
      banner.className = 'q-confirm';
      banner.textContent = `⚠ 确认要"${chosen?.label ?? '?'}"吗?`;
      layer.appendChild(banner);

      const choices = document.createElement('div');
      choices.className = 'q-choices';
      const confirmBtn = button('确认', 'danger');
      confirmBtn.addEventListener('click', () => {
        if (pendingChoice) handlers.onCommit(pendingChoice);
      });
      const cancelBtn = button('返回', '');
      cancelBtn.addEventListener('click', () => {
        stage = 'choose';
        pendingChoice = null;
        render();
      });
      choices.appendChild(confirmBtn);
      choices.appendChild(cancelBtn);
      layer.appendChild(choices);
    } else {
      const choices = document.createElement('div');
      choices.className = msg.choices.length > 2 ? 'q-choices col' : 'q-choices';
      for (const choice of msg.choices) {
        const variant =
          msg.risk === 'high' && choice.id === 'accept'
            ? 'danger'
            : choice.id === 'accept'
              ? 'primary'
              : '';
        const btn = button(choice.label, variant);
        btn.addEventListener('click', () => {
          if (msg.require_confirm) {
            stage = 'confirm';
            pendingChoice = choice.id;
            render();
          } else {
            handlers.onCommit(choice.id);
          }
        });
        choices.appendChild(btn);
      }
      layer.appendChild(choices);
    }

    if (handlers.queueTotal > 1) {
      const queue = document.createElement('div');
      queue.className = 'q-queue';
      queue.textContent = `+${handlers.queueTotal - 1}`;
      layer.appendChild(queue);
    }
  };
  render();
}

export function hideQuestion(): void {
  const layer = document.getElementById('question-layer') as HTMLElement;
  const slide = layer.closest('.slide-main') as HTMLElement;
  const device = document.querySelector('.device') as HTMLElement;
  layer.replaceChildren();
  slide.classList.remove('has-question');
  device.classList.remove('risk-low', 'risk-medium', 'risk-high');
}

function button(label: string, variant: string): HTMLButtonElement {
  const b = document.createElement('button');
  b.className = `q-btn ${variant}`;
  b.type = 'button';
  b.textContent = label;
  return b;
}
