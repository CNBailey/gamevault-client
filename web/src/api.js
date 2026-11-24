// Talks to two services: this PS5's GameVault payload (same origin, /api/…)
// and the GameVault server on the NAS (its address is in Settings).

async function request(url, options = {}) {
  const res = await fetch(url, {
    ...options,
    headers: options.body ? { 'Content-Type': 'application/json' } : undefined,
    body: options.body ? JSON.stringify(options.body) : undefined
  });
  let data = null;
  try { data = await res.json(); } catch (e) { /* empty body */ }
  if (!res.ok) throw new Error((data && data.error) || `HTTP ${res.status}`);
  return data;
}

// When the interface is opened from a phone or PC, it still talks to the PS5.
const local = (path) => `${location.origin}${path}`;

export const ps5 = {
  state: () => request(local('/api/state')),
  saveSettings: (s) => request(local('/api/settings'), { method: 'PUT', body: s }),
  testConnection: (server) => request(local('/api/test-connection'), { method: 'POST', body: { server } }),
  download: (gameId, opts = {}) => request(local('/api/downloads'), { method: 'POST', body: { gameId, ...opts } }),
  action: (id, action) => request(local(`/api/downloads/${encodeURIComponent(id)}/${action}`), { method: 'POST' }),
  remove: (id) => request(local(`/api/downloads/${encodeURIComponent(id)}`), { method: 'DELETE' }),
  clearFinished: () => request(local('/api/downloads/clear'), { method: 'POST' }),
  deleteInstalled: (path) => request(local(`/api/installed?path=${encodeURIComponent(path)}`), { method: 'DELETE' }),
  rescan: () => request(local('/api/rescan'), { method: 'POST' }),
  notifyTest: () => request(local('/api/notify-test'), { method: 'POST' }),
  quit: () => request(local('/api/quit'), { method: 'POST' }),
  speedTest: (gameId) => request(local('/api/speed-test'), { method: 'POST', body: { gameId } }),
  updateCheck: () => request(local('/api/update-check'), { method: 'POST' }),
  // GameVault restarts as the new version; the request usually ends without an answer.
  update: () => request(local('/api/update'), { method: 'POST' }).catch((e) => {
    if (/HTTP|Failed to fetch|NetworkError|Load failed/i.test(e.message) && !/409/.test(e.message)) return { restarting: true };
    throw e;
  })
};

export function vault(server) {
  const base = String(server || '').replace(/\/+$/, '');
  return {
    base,
    games: () => request(`${base}/api/v1/games?apps=hide&sort=title`),
    game: (id) => request(`${base}/api/v1/games/${id}`),
    files: (id) => request(`${base}/api/v1/games/${id}/files`),
    favourite: (id, favourite) => request(`${base}/api/v1/games/${id}/favourite`, { method: 'PUT', body: { favourite } }),
    media: (url) => (!url ? null : /^https?:/i.test(url) ? url : `${base}${url}`)
  };
}
