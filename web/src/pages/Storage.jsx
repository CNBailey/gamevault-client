import { useState } from 'preact/hooks';
import { ps5 } from '../api.js';
import { Bar, Hints, Icon, ICONS, useAutofocus } from '../components.jsx';
import { formatBytes } from '../util.js';

const KIND = { image: 'exFAT image', folder: 'Folder', 'ps4-app': 'Installed PS4 game', 'ps4-pkg': 'PS4 .pkg on a USB drive' };

export function Storage({ state, refresh, notify, games }) {
  const [confirm, setConfirm] = useState(null);
  const [busy, setBusy] = useState(false);
  useAutofocus([]);

  const remove = async (item) => {
    setBusy(true);
    try { await ps5.deleteInstalled(item.path); notify(`Deleted ${item.title}`); setConfirm(null); await refresh(); } catch (e) { notify(e.message, 'error'); }
    setBusy(false);
  };
  const known = (tid) => games && games.some((g) => String(g.titleId).toUpperCase() === String(tid).toUpperCase());

  return (
    <div class="stack">
      <div class="row between">
        <h1 class="page-title">Storage</h1>
        <button class="btn" onClick={async () => { await ps5.rescan(); await refresh(); notify('Looked for games again'); }}>Look again</button>
      </div>
      <div class="drives">
        {state.drives.map((d) => {
          const items = state.installed.filter((i) => i.drive === d.id);
          const used = d.total - d.free;
          const gameBytes = items.reduce((n, i) => n + i.size, 0);
          return (
            <div class="panel">
              <div class="row between"><h2>{d.label}</h2><b>{formatBytes(d.free)} free</b></div>
              <Bar label={`${d.label} used`} parts={[{ pct: ((used - gameBytes) / d.total) * 100, color: "#3A4554" }, { pct: (gameBytes / d.total) * 100, color: '#5B8CFF' }]} />
              <div class="muted small">{formatBytes(used)} of {formatBytes(d.total)} used · blue: {items.length} game{items.length === 1 ? '' : 's'} ({formatBytes(gameBytes)}) · games go in <code>{d.homebrew}</code></div>
            </div>
          );
        })}
      </div>

      <h2>Games on this PS5</h2>
      {state.installed.length === 0 && <div class="panel empty"><p class="muted">No games found in the homebrew folders yet.</p></div>}
      <div class="list">
        {state.installed.slice().sort((a, b) => b.size - a.size).map((i) => (
          <div class="panel row between gap item">
            <div class="grow">
              <b class="clip">{i.title || i.titleId}</b>
              <div class="muted small">{i.titleId}{i.version ? ` · v${i.version}` : ''} · {KIND[i.kind] || i.kind} · {formatBytes(i.size)}{known(i.titleId) ? '' : ' · not in GameVault'}</div>
              <div class="muted small clip"><code>{i.path}</code></div>
            </div>
            {i.kind === 'ps4-app' ? (
              <span class="muted small">Delete in the PS5's Storage settings</span>
            ) : confirm === i.path ? (
              <div class="row gap">
                <button class="btn danger" disabled={busy} onClick={() => remove(i)}><Icon d={ICONS.trash} />Delete {formatBytes(i.size)}</button>
                <button class="btn" onClick={() => setConfirm(null)}>Keep</button>
              </div>
            ) : (
              <button class="btn" onClick={() => setConfirm(i.path)}><Icon d={ICONS.trash} />Delete</button>
            )}
          </div>
        ))}
      </div>
      <p class="muted small">Deleting removes the game file from the PS5 for good. GameVault still has it on the NAS, so you can download it again.</p>
      <Hints items={[['✕', 'Select'], ['L1 / R1', 'Switch section']]} />
    </div>
  );
}
