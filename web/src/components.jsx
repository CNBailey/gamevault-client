import { useEffect, useState } from 'preact/hooks';
import { formatBytes, formatSpeed, formatEta, ACTIVE } from './util.js';

export function go(path) { location.hash = `#/${path}`; }

/** Focuses the element marked data-autofocus (or the first control) after a page renders. */
export function useAutofocus(deps = []) {
  useEffect(() => {
    // Only on the TV: on a phone or PC a focused field would pop up the keyboard or show a ring.
    if (!document.documentElement.classList.contains('tv')) return undefined;
    const t = setTimeout(() => {
      if (document.activeElement && document.activeElement !== document.body && document.body.contains(document.activeElement)) return;
      const el = document.querySelector('[data-autofocus]') || document.querySelector('main [data-nav], main button:not([disabled]), main a[href]');
      if (el) el.focus({ preventScroll: false });
    }, 30);
    return () => clearTimeout(t);
  }, deps);
}

export function Logo() {
  return (
    <a class="logo" href="#/library" tabIndex={-1} data-nav-skip>
      <svg width="40" height="40" viewBox="0 0 64 64" aria-hidden="true">
        <path d="M7 7 H20 L37 57 H25 Z" fill="#5B8CFF" stroke="#5B8CFF" stroke-width="2" stroke-linejoin="round" />
        <path d="M57 7 H44 L27 57 H39 Z" fill="#A9C2FF" stroke="#0A0C10" stroke-width="3" stroke-linejoin="round" />
      </svg>
      <span>Game<span class="accent">Vault</span></span>
    </a>
  );
}

const PALETTE = ['#3A4A3A', '#6A2D2D', '#3D4A2A', '#5A4420', '#2A3542', '#2F3D5C', '#5C3A22', '#4A2E4F', '#2E4766', '#3B3354'];
export function tint(title) {
  let h = 0;
  for (const c of String(title)) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  return PALETTE[h % PALETTE.length];
}

export function Cover({ game, api, children, large }) {
  const [broken, setBroken] = useState(false);
  const src = !broken && api.media(game.cover);
  return (
    <div class={`cover${large ? ' large' : ''}`} style={{ background: tint(game.title) }}>
      {src ? <img src={src} alt="" loading="lazy" onError={() => setBroken(true)} /> : <span class="cover-title">{game.title}</span>}
      {children}
    </div>
  );
}

const CHECK = 'M5 12.5l4.5 4.5L19 7.5';
const UP = 'M12 19V6M6.5 11.5 12 6l5.5 5.5';
export function Icon({ d, size = 20, stroke = 2.6 }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width={stroke} stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d={d} /></svg>
  );
}
export const ICONS = {
  check: CHECK,
  up: UP,
  down: 'M12 4v11M7 10l5 5 5-5M5 20h14',
  stop: 'M12 3a9 9 0 1 0 0 18a9 9 0 0 0 0-18zM5.6 5.6l12.8 12.8',
  star: 'M12 3l2.7 5.6 6.1.9-4.4 4.3 1 6.1L12 17l-5.4 2.9 1-6.1-4.4-4.3 6.1-.9z',
  pause: 'M8 5v14M16 5v14',
  play: 'M7 5l12 7-12 7z',
  x: 'M6 6l12 12M18 6 6 18',
  trash: 'M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3'
};

/** Corner badge on a library tile. */
export function StatusBadge({ st }) {
  if (st.status === 'downloaded') return <span class="badge good" aria-label="On this PS5"><Icon d={CHECK} /></span>;
  if (st.status === 'update') return <span class="badge warn" aria-label="Update available"><Icon d={UP} /></span>;
  if (st.status === 'downloading' || st.status === 'queued' || st.status === 'paused') {
    const pct = st.job && st.job.total ? Math.floor((st.job.done / st.job.total) * 100) : 0;
    return <span class="badge busy pill" aria-label={`${st.status}, ${pct}%`}>{st.status === 'queued' ? 'Queued' : st.status === 'paused' ? 'Paused' : `${pct}%`}</span>;
  }
  if (st.status === 'failed') return <span class="badge bad" aria-label="Download failed"><Icon d={ICONS.x} /></span>;
  return null;
}

export function Bar({ parts, label }) {
  return (
    <div class="bar" role="progressbar" aria-label={label} aria-valuemin="0" aria-valuemax="100" aria-valuenow={Math.round(parts[0] ? parts[0].pct : 0)}>
      {parts.map((p) => <div style={{ width: `${Math.max(0, Math.min(100, p.pct))}%`, background: p.color }} />)}
    </div>
  );
}

export function StoragePanel({ state }) {
  const active = state.downloads.filter((d) => ACTIVE.includes(d.state));
  const current = active.find((d) => d.state === 'downloading' || d.state === 'verifying') || active[0];
  const rest = active.filter((d) => d !== current);
  return (
    <aside class="side" aria-label="Console storage" data-nav-skip>
      <section class="panel">
        <h2>Console storage</h2>
        {state.drives.map((d) => {
          const used = d.total - d.free;
          return (
            <div class="drive">
              <div class="row between"><span>{d.label}</span><b>{formatBytes(d.free)} free</b></div>
              <Bar label={`${d.label} used`} parts={[{ pct: (used / d.total) * 100, color: d.id === 'internal' ? '#5B8CFF' : '#A9C2FF' }]} />
              <div class="muted small">{formatBytes(used)} of {formatBytes(d.total)} used</div>
            </div>
          );
        })}
      </section>
      <section class="panel">
        <h2>{current ? 'Downloading' : 'Downloads'}</h2>
        {current ? (
          <>
            <div class="row between"><span class="clip">{current.title}</span><b>{current.total ? Math.floor((current.done / current.total) * 100) : 0}%</b></div>
            <Bar label="Download progress" parts={[{ pct: current.total ? (current.done / current.total) * 100 : 0, color: '#5B8CFF' }]} />
            <div class="muted small">
              {current.state === 'paused' ? 'Paused' : current.state === 'queued' ? 'Waiting' : current.state === 'verifying' ? 'Checking the file…'
                : [`${formatBytes(current.done)} of ${formatBytes(current.total)}`, formatSpeed(current.speed), formatEta(current.total - current.done, current.speed)].filter(Boolean).join(' · ')}
            </div>
            {rest.length > 0 && <div class="muted small">{rest.length} more queued · {formatBytes(rest.reduce((n, d) => n + d.total - d.done, 0))}</div>}
          </>
        ) : (
          <>
            <div>Nothing downloading</div>
            <div class="muted small">Downloads keep going while you play{state.settings.playLimitMBps ? `, limited to ${state.settings.playLimitMBps} MB/s` : ''}.</div>
          </>
        )}
      </section>
    </aside>
  );
}

export function Hints({ items }) {
  if (!document.documentElement.classList.contains('tv')) return null;
  const color = { '✕': '#8FB0FF', '○': '#F07A7A', '△': '#3FCF8E', '□': '#E58FD0' };
  return (
    <footer class="hints">
      {items.map(([k, label]) => <span><b style={{ color: color[k] || '#9AA4B2' }}>{k}</b> {label}</span>)}
    </footer>
  );
}
