export function formatBytes(n) {
  if (n == null || isNaN(n)) return '—';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  let v = Number(n);
  while (v >= 1000 && i < units.length - 1) { v /= 1000; i++; }
  return `${v >= 100 || i === 0 ? Math.round(v) : v.toFixed(1)} ${units[i]}`;
}

export function formatSpeed(bps) {
  return bps > 0 ? `${formatBytes(bps)}/s` : '';
}

export function formatEta(remaining, bps) {
  if (!(bps > 0) || !(remaining > 0)) return '';
  const s = Math.round(remaining / bps);
  if (s < 60) return `${s} s left`;
  if (s < 3600) return `${Math.round(s / 60)} min left`;
  const h = Math.floor(s / 3600);
  return `${h} h ${Math.round((s % 3600) / 60)} min left`;
}

/** Compares versions like "01.010.000" or "1.09" by number. */
export function compareVersions(a, b) {
  const pa = String(a || '').split(/[^0-9]+/).filter(Boolean).map(Number);
  const pb = String(b || '').split(/[^0-9]+/).filter(Boolean).map(Number);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] || 0) - (pb[i] || 0);
    if (d) return d;
  }
  return 0;
}

export const ACTIVE = ['queued', 'downloading', 'verifying', 'paused'];

/**
 * How a GameVault game stands on this PS5:
 * { status: 'download'|'downloaded'|'update'|'queued'|'downloading'|'paused'|'failed',
 *   installed, job }
 */
export function gameStatus(game, state) {
  if (!state) return { status: 'download' };
  const job = (state.downloads || []).find((d) => d.gameId === game.id && ACTIVE.includes(d.state));
  const failed = (state.downloads || []).find((d) => d.gameId === game.id && d.state === 'failed');
  const tid = String(game.titleId || '').toUpperCase();
  const copies = tid ? (state.installed || []).filter((i) => String(i.titleId).toUpperCase() === tid) : [];
  const installed = copies.sort((a, b) => compareVersions(b.version, a.version))[0] || null;
  if (job) {
    const status = job.state === 'paused' ? 'paused' : job.state === 'queued' ? 'queued' : 'downloading';
    return { status, installed, job };
  }
  if (installed) {
    const newer = game.version && installed.version && compareVersions(game.version, installed.version) > 0;
    return { status: newer ? 'update' : 'downloaded', installed, failed };
  }
  return { status: failed ? 'failed' : 'download', failed };
}

export function freeOn(state, driveId) {
  const d = (state && state.drives || []).find((x) => x.id === driveId);
  if (!d) return null;
  const reserved = (state.reserved && state.reserved[driveId]) || 0;
  return { drive: d, free: Math.max(0, d.free - reserved) };
}

export function fwCompare(a, b) {
  const [am, an] = String(a).split('.').map(Number);
  const [bm, bn] = String(b).split('.').map(Number);
  return (am - bm) || ((an || 0) - (bn || 0));
}

export function load(key, fallback) {
  try { const v = localStorage.getItem(`gv.${key}`); return v ? JSON.parse(v) : fallback; } catch (e) { return fallback; }
}

export function save(key, value) {
  try { localStorage.setItem(`gv.${key}`, JSON.stringify(value)); } catch (e) { /* storage unavailable */ }
}
