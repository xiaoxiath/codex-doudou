/**
 * Three-screen horizontal slider for the round lens.
 * Handles swipe gestures (pointer events) + dot navigation + programmatic switch.
 */
export type ScreenId = 'info' | 'usage' | 'main' | 'history';

const ORDER: ScreenId[] = ['info', 'usage', 'main', 'history'];
const DEFAULT_INDEX = ORDER.indexOf('main');

export interface ScreenManagerOptions {
  /** Called whenever the active screen changes (including programmatic). */
  onChange?: (screen: ScreenId) => void;
}

export class ScreenManager {
  private track: HTMLElement;
  private lens: HTMLElement;
  private dots: NodeListOf<HTMLElement>;
  private current: ScreenId = 'main';
  private dragStartX = 0;
  private dragStartT = 0;
  private dragging = false;
  private dragDelta = 0;

  constructor(private opts: ScreenManagerOptions = {}) {
    this.track = document.getElementById('track') as HTMLElement;
    this.lens = this.track.parentElement as HTMLElement;
    this.dots = document.querySelectorAll<HTMLElement>('.dot');

    this.lens.addEventListener('pointerdown', (e) => this.onDown(e));
    this.lens.addEventListener('pointermove', (e) => this.onMove(e));
    this.lens.addEventListener('pointerup', (e) => this.onUp(e));
    this.lens.addEventListener('pointercancel', () => this.onCancel());

    this.dots.forEach((d) =>
      d.addEventListener('click', () => {
        const target = d.dataset.screen as ScreenId | undefined;
        if (target) this.setScreen(target);
      }),
    );

    // keyboard arrows for easier desktop demo
    window.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowLeft') this.shift(-1);
      else if (e.key === 'ArrowRight') this.shift(1);
    });

    this.applyTransform(0);
  }

  setScreen(target: ScreenId, animate = true): void {
    if (target === this.current) return;
    this.current = target;
    this.track.classList.toggle('dragging', !animate);
    this.applyTransform(0);
    this.updateDots();
    this.opts.onChange?.(target);
  }

  shift(direction: -1 | 1): void {
    const idx = ORDER.indexOf(this.current);
    const next = ORDER[Math.min(ORDER.length - 1, Math.max(0, idx + direction))];
    if (next) this.setScreen(next);
  }

  get screen(): ScreenId {
    return this.current;
  }

  // ---------- internals ----------

  private indexOfCurrent(): number {
    return ORDER.indexOf(this.current);
  }

  private applyTransform(delta: number): void {
    const slideW = this.lens.clientWidth;
    const offset = -slideW * this.indexOfCurrent() + delta;
    this.track.style.transform = `translateX(${offset}px)`;
  }

  private updateDots(): void {
    this.dots.forEach((d) => {
      d.classList.toggle('active', d.dataset.screen === this.current);
    });
  }

  private onDown(e: PointerEvent): void {
    if (e.button !== undefined && e.button !== 0) return;
    this.dragging = true;
    this.dragStartX = e.clientX;
    this.dragStartT = performance.now();
    this.dragDelta = 0;
    this.track.classList.add('dragging');
    (e.target as Element).setPointerCapture?.(e.pointerId);
  }

  private onMove(e: PointerEvent): void {
    if (!this.dragging) return;
    this.dragDelta = e.clientX - this.dragStartX;
    // resist at edges so swipe past first/last feels rubbery
    const idx = this.indexOfCurrent();
    if ((idx === 0 && this.dragDelta > 0) || (idx === ORDER.length - 1 && this.dragDelta < 0)) {
      this.dragDelta *= 0.35;
    }
    this.applyTransform(this.dragDelta);
  }

  private onUp(_e: PointerEvent): void {
    if (!this.dragging) return;
    this.dragging = false;
    this.track.classList.remove('dragging');
    const slideW = this.lens.clientWidth;
    const ratio = this.dragDelta / slideW;
    const elapsed = performance.now() - this.dragStartT;
    const fast = Math.abs(this.dragDelta) > 30 && elapsed < 250;
    let dir: -1 | 0 | 1 = 0;
    if (ratio > 0.25 || (fast && this.dragDelta > 0)) dir = -1;
    else if (ratio < -0.25 || (fast && this.dragDelta < 0)) dir = 1;
    this.dragDelta = 0;
    if (dir !== 0) this.shift(dir);
    else this.applyTransform(0);
  }

  private onCancel(): void {
    if (!this.dragging) return;
    this.dragging = false;
    this.dragDelta = 0;
    this.track.classList.remove('dragging');
    this.applyTransform(0);
  }
}
