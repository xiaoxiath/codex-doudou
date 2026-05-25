/**
 * Pet renderer — layered PNG sprites (body + face + accessory).
 *
 * Public API matches the previous emoji version so main.ts doesn't have
 * to change. Internally the pet is composed at runtime from sprites
 * served at `/pet-art/*.png`. Per-state animations (breath / spin /
 * float / shake / blink) ride on top via CSS classes.
 *
 * Body style defaults to "glossy" (semi-3D). To switch to flat-cartoon,
 * set `?body=flat` in the URL.
 */
import type { State } from '@doudou/device-protocol';

import { applyMarquee, clearMarquee } from './marquee.js';

type EyeVariant      = 'normal' | 'blink' | 'sleep' | 'x' | 'happy' | 'surprise';
type MouthVariant    = 'smile' | 'open' | 'grin' | 'wobble' | 'sad' | null;
type AccessoryVariant = 'question' | 'gear' | 'sparkle' | 'zzz' | 'sweat' | 'alert' | null;
type AccAnim         = 'spin' | 'float' | null;

interface Composition {
  eye:   EyeVariant;
  mouth: MouthVariant;
  cheek: boolean;
  acc:   AccessoryVariant;
  accAnim: AccAnim;
  /** Trigger a side-to-side shake on enter (e.g. error). */
  shakeOnEnter?: boolean;
}

const COMPOSITION: Record<State, Composition> = {
  idle:             { eye: 'normal',   mouth: 'smile',  cheek: true,  acc: null,       accAnim: null },
  thinking:         { eye: 'normal',   mouth: null,     cheek: false, acc: 'question', accAnim: 'float' },
  executing:        { eye: 'normal',   mouth: 'open',   cheek: false, acc: 'gear',     accAnim: 'spin'  },
  waiting_input:    { eye: 'surprise', mouth: 'open',   cheek: false, acc: null,       accAnim: null },
  waiting_approval: { eye: 'surprise', mouth: 'open',   cheek: false, acc: null,       accAnim: null },
  done:             { eye: 'happy',    mouth: 'grin',   cheek: true,  acc: 'sparkle',  accAnim: 'float' },
  error:            { eye: 'x',        mouth: 'sad',    cheek: false, acc: 'alert',    accAnim: 'float', shakeOnEnter: true },
};

const STATE_LABEL: Record<State, string> = {
  idle: '待机',
  thinking: '思考中',
  executing: '执行中',
  waiting_input: '等你输入',
  waiting_approval: '等你审批',
  done: '完成',
  error: '出错',
};

/** Per-state edge-glow colour for the lens halo (mirrors pet-art-compare.html). */
const STATE_GLOW: Record<State, string> = {
  idle:             '#fce8ad',
  thinking:         '#a8c8ff',
  executing:        '#ffd47a',
  waiting_input:    '#d8b7ff',
  waiting_approval: '#d8b7ff',
  done:             '#94e9a3',
  error:            '#f47878',
};

const ART = '/pet-art';

function bodySrc(): string {
  const params = new URLSearchParams(window.location.search);
  return params.get('body') === 'flat' ? `${ART}/pet_body.png` : `${ART}/pet_body_glossy.png`;
}

export class Pet {
  private root: HTMLElement;
  private lens: HTMLElement | null;
  private pet: HTMLElement;
  private bodyImg: HTMLImageElement;
  private eyeL: HTMLImageElement;
  private eyeR: HTMLImageElement;
  private mouth: HTMLImageElement;
  private cheekL: HTMLImageElement;
  private cheekR: HTMLImageElement;
  private acc: HTMLImageElement;

  private statusLine: HTMLElement;
  private titleLine: HTMLElement;
  private state: State = 'idle';
  private threadTitle = '';
  private resetTimer: ReturnType<typeof setTimeout> | null = null;
  private longPressTimer: ReturnType<typeof setTimeout> | null = null;
  private blinkTimer: ReturnType<typeof setTimeout> | null = null;
  /** Eye we should restore to after a blink frame. */
  private restingEye: EyeVariant = 'normal';

  constructor() {
    this.root = document.getElementById('pet-layer') as HTMLElement;
    this.lens = this.root.closest('.lens');
    this.statusLine = document.getElementById('status-layer') as HTMLElement;
    this.titleLine  = document.getElementById('title-layer')  as HTMLElement;

    if (this.lens) {
      this.lens.classList.add('glow-breathing');
      this.lens.style.setProperty('--state-glow', STATE_GLOW[this.state]);
    }

    this.pet = document.createElement('div');
    this.pet.className = 'pet';
    this.pet.dataset.state = this.state;

    this.bodyImg = this.mkImg('pet-body',  bodySrc());
    this.cheekL  = this.mkImg('pet-cheek pet-cheek-l', `${ART}/cheek.png`);
    this.cheekR  = this.mkImg('pet-cheek pet-cheek-r', `${ART}/cheek.png`);
    this.eyeL    = this.mkImg('pet-eye pet-eye-l',     `${ART}/eye_normal.png`);
    this.eyeR    = this.mkImg('pet-eye pet-eye-r',     `${ART}/eye_normal.png`);
    this.mouth   = this.mkImg('pet-mouth',             `${ART}/mouth_smile.png`);
    this.acc     = this.mkImg('pet-acc',               '');

    this.pet.append(this.bodyImg, this.cheekL, this.cheekR, this.eyeL, this.eyeR, this.mouth, this.acc);
    this.root.appendChild(this.pet);

    this.statusLine.textContent = STATE_LABEL[this.state];
    clearMarquee(this.titleLine);

    this.pet.addEventListener('click', () => this.wiggle());
    this.pet.addEventListener('pointerdown', () => this.armLongPress());
    this.pet.addEventListener('pointerup',   () => this.clearLongPress());
    this.pet.addEventListener('pointerleave',() => this.clearLongPress());

    this.applyComposition(this.state);
    this.scheduleBlink();
  }

  setState(state: State, statusTitle?: string, statusBody?: string): void {
    const stateChanged = state !== this.state;
    this.state = state;
    this.pet.dataset.state = state;

    if (this.lens) {
      this.lens.style.setProperty('--state-glow', STATE_GLOW[state]);
    }

    const text = [statusTitle, statusBody].filter(Boolean).join(' · ');
    this.statusLine.textContent = text || STATE_LABEL[state];

    this.applyComposition(state);

    if (stateChanged && COMPOSITION[state].shakeOnEnter) {
      this.pet.classList.remove('shaking');
      // force reflow so the keyframe restarts
      void this.pet.offsetWidth;
      this.pet.classList.add('shaking');
      setTimeout(() => this.pet.classList.remove('shaking'), 800);
    }

    if (this.resetTimer) clearTimeout(this.resetTimer);
    if (state === 'done' || state === 'error') {
      this.resetTimer = setTimeout(() => this.setState('idle'), 3500);
    }
  }

  setThreadTitle(title: string | undefined): void {
    const t = (title ?? '').trim();
    this.threadTitle = t;
    if (!t) {
      clearMarquee(this.titleLine);
    } else {
      applyMarquee(this.titleLine, t);
    }
  }

  get currentThreadTitle(): string {
    return this.threadTitle;
  }

  wiggle(): void {
    this.pet.classList.remove('wiggle');
    void this.pet.offsetWidth;
    this.pet.classList.add('wiggle');
    setTimeout(() => this.pet.classList.remove('wiggle'), 360);
  }

  /* ---------- internals ---------- */

  private mkImg(cls: string, src: string): HTMLImageElement {
    const img = document.createElement('img');
    img.className = cls;
    img.alt = '';
    img.draggable = false;
    if (src) img.src = src;
    return img;
  }

  private applyComposition(state: State): void {
    const c = COMPOSITION[state];

    this.restingEye = c.eye;
    this.eyeL.src = `${ART}/eye_${c.eye}.png`;
    this.eyeR.src = `${ART}/eye_${c.eye}.png`;

    if (c.mouth) {
      this.mouth.src = `${ART}/mouth_${c.mouth}.png`;
      this.mouth.style.display = '';
    } else {
      this.mouth.style.display = 'none';
    }

    this.cheekL.style.display = c.cheek ? '' : 'none';
    this.cheekR.style.display = c.cheek ? '' : 'none';

    this.acc.classList.remove('spin', 'float');
    if (c.acc) {
      this.acc.src = `${ART}/acc_${c.acc}.png`;
      this.acc.style.display = '';
      if (c.accAnim) this.acc.classList.add(c.accAnim);
    } else {
      this.acc.style.display = 'none';
    }
  }

  private scheduleBlink(): void {
    if (this.blinkTimer) clearTimeout(this.blinkTimer);
    const delay = 3000 + Math.random() * 4000;
    this.blinkTimer = setTimeout(() => {
      this.blink();
      this.scheduleBlink();
    }, delay);
  }

  private blink(): void {
    // Only blink when the resting eye looks like an "open" eye.
    if (this.restingEye !== 'normal' && this.restingEye !== 'surprise') return;
    const oL = this.eyeL.src, oR = this.eyeR.src;
    this.eyeL.src = `${ART}/eye_blink.png`;
    this.eyeR.src = `${ART}/eye_blink.png`;
    setTimeout(() => {
      this.eyeL.src = oL;
      this.eyeR.src = oR;
    }, 130);
  }

  private armLongPress(): void {
    this.clearLongPress();
    this.longPressTimer = setTimeout(() => this.showBubble('我在呢~'), 700);
  }
  private clearLongPress(): void {
    if (this.longPressTimer) {
      clearTimeout(this.longPressTimer);
      this.longPressTimer = null;
    }
  }

  private showBubble(text: string): void {
    const old = this.root.querySelector('.pet-bubble');
    if (old) old.remove();
    const b = document.createElement('div');
    b.className = 'pet-bubble';
    b.textContent = text;
    b.style.transform = 'translateY(-72px)';
    this.root.appendChild(b);
    setTimeout(() => b.remove(), 2100);
  }
}
