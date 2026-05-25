/**
 * Marquee helper — applies a horizontal-scroll animation to any element
 * whose text overflows its containing box.
 *
 *   applyMarquee(el, "some long text")
 *
 * If the text fits, it stays static. If it overflows, it gets wrapped in
 * a span.marquee that loops smoothly. Idempotent — call repeatedly with
 * the same text and nothing happens.
 */
export function applyMarquee(host: HTMLElement, text: string): void {
  if (host.dataset.marqueeText === text) return;
  host.dataset.marqueeText = text;

  // Reset to plain text first so width measurement is honest.
  host.classList.remove('has-marquee');
  host.textContent = text;

  if (!text) return;

  // Defer measurement until after layout so scrollWidth is accurate.
  requestAnimationFrame(() => {
    if (host.dataset.marqueeText !== text) return; // changed under us
    if (host.scrollWidth <= host.clientWidth + 1) return;

    host.classList.add('has-marquee');
    host.textContent = '';
    const span = document.createElement('span');
    span.className = 'marquee';
    // Two copies separated by a bullet so the loop reads continuously
    // without an obvious gap.
    span.textContent = `${text}   ·   ${text}   ·   `;
    host.appendChild(span);
  });
}

/** Clear any marquee state — content goes back to plain. */
export function clearMarquee(host: HTMLElement): void {
  delete host.dataset.marqueeText;
  host.classList.remove('has-marquee');
  host.textContent = '';
}
