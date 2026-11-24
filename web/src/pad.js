// Controller support. The PS5 browser turns the pad into key presses:
// the D-pad into arrow keys and ✕ into Enter, L1/R1 into F5/F6 (116/117),
// L2/R2 into F7/F8. The other buttons vary, so they can be learnt in
// Settings › Controller test.

import { load, save } from './util.js';

const DEFAULT_KEYS = {
  prev: [116, 81],      // L1, or Q on a keyboard
  next: [117, 69],      // R1, or E
  back: [27, 8],        // ○ is often Escape or Backspace
  favourite: [70],      // △ (learn it), or F
  drive: [68],          // □ (learn it), or D
  search: [191]         // "/" on a keyboard
};

export const ACTIONS = {
  prev: 'Previous section (L1)',
  next: 'Next section (R1)',
  back: 'Back (○)',
  favourite: 'Favourite (△)',
  drive: 'Change drive (□)'
};

let keys = { ...DEFAULT_KEYS, ...load('keys', {}) };

export function keyMap() { return keys; }
export function setKey(action, code) {
  keys = { ...keys, [action]: [code] };
  save('keys', keys);
}
export function resetKeys() {
  keys = { ...DEFAULT_KEYS };
  save('keys', {});
}
export function actionFor(code) {
  for (const a of Object.keys(keys)) if ((keys[a] || []).includes(code)) return a;
  return null;
}

// ---- Spatial focus: arrow keys move to the nearest control in that direction ----

function focusables(root = document) {
  return Array.from(root.querySelectorAll('[data-nav], button:not([disabled]), a[href], input, select, textarea'))
    .filter((el) => {
      if (el.closest('[data-nav-skip]')) return false;
      const r = el.getBoundingClientRect();
      return r.width > 0 && r.height > 0 && getComputedStyle(el).visibility !== 'hidden';
    });
}

function center(r) { return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; }

export function moveFocus(dir) {
  const items = focusables();
  if (!items.length) return false;
  const current = document.activeElement;
  if (!current || current === document.body || !items.includes(current)) {
    items[0].focus();
    return true;
  }
  const from = current.getBoundingClientRect();
  const c = center(from);
  let best = null;
  let bestScore = Infinity;
  for (const el of items) {
    if (el === current) continue;
    const r = el.getBoundingClientRect();
    const p = center(r);
    const dx = p.x - c.x;
    const dy = p.y - c.y;
    let main;
    let cross;
    if (dir === 'left') { if (r.right > from.left + 1) continue; main = -dx; cross = Math.abs(dy); }
    else if (dir === 'right') { if (r.left < from.right - 1) continue; main = dx; cross = Math.abs(dy); }
    else if (dir === 'up') { if (r.bottom > from.top + 1) continue; main = -dy; cross = Math.abs(dx); }
    else { if (r.top < from.bottom - 1) continue; main = dy; cross = Math.abs(dx); }
    // Prefer items in the same row/column, then the nearest.
    const overlap = dir === 'left' || dir === 'right'
      ? Math.min(r.bottom, from.bottom) - Math.max(r.top, from.top)
      : Math.min(r.right, from.right) - Math.max(r.left, from.left);
    const score = main + cross * (overlap > 0 ? 0.3 : 2.5);
    if (score < bestScore) { bestScore = score; best = el; }
  }
  if (best) {
    best.focus({ preventScroll: true });
    best.scrollIntoView({ block: 'nearest', inline: 'nearest', behavior: 'smooth' });
    return true;
  }
  return false;
}

const ARROWS = { 37: 'left', 38: 'up', 39: 'right', 40: 'down' };

/** Installs the key handler; `onAction(name, event)` gets prev/next/back/favourite/drive/search. */
export function installPad(onAction) {
  const handler = (e) => {
    if (window.__gvKeyTest) return; // Controller test is listening
    const tag = (e.target && e.target.tagName) || '';
    const typing = (tag === 'INPUT' && !['checkbox', 'radio', 'range'].includes(e.target.type)) || tag === 'TEXTAREA';
    const code = e.keyCode || e.which;
    if (ARROWS[code] && !(typing && (code === 37 || code === 39))) {
      if (tag === 'SELECT') return;
      if (moveFocus(ARROWS[code])) e.preventDefault();
      return;
    }
    if (typing && code !== 27) return;
    const action = actionFor(code);
    if (action) {
      e.preventDefault();
      onAction(action, e);
    }
  };
  window.addEventListener('keydown', handler, true);
  return () => window.removeEventListener('keydown', handler, true);
}
