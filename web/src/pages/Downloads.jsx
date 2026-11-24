import { ps5 } from '../api.js';
import { Bar, Hints, Icon, ICONS, useAutofocus } from '../components.jsx';
import { formatBytes, formatSpeed, formatEta } from '../util.js';

const LABEL = {
  queued: 'Waiting', downloading: 'Downloading', verifying: 'Checking the file', paused: 'Paused',
  done: 'Done', failed: 'Failed', cancelled: 'Cancelled'
};

export function Downloads({ state, refresh, notify }) {
  useAutofocus([]);
  const run = async (fn) => {
    try { await fn(); await refresh(); } catch (e) { notify(e.message, 'error'); }
  };
  const jobs = state.downloads;
  const finished = jobs.filter((j) => j.state === 'done' || j.state === 'cancelled').length;

  return (
    <div class="stack">
      <div class="row between">
        <h1 class="page-title">Downloads</h1>
        {finished > 0 && <button class="btn" onClick={() => run(ps5.clearFinished)}>Clear finished ({finished})</button>}
      </div>
      {state.gameRunning && state.settings.playLimitMBps > 0 && (
        <p class="warn-text">A game is running, so downloads are limited to {state.settings.playLimitMBps} MB/s.</p>
      )}
      {jobs.length === 0 && <div class="panel empty"><p class="muted">Nothing here yet. Pick a game in the Library and press Download.</p></div>}
      {jobs.map((j, i) => {
        const pct = j.total ? (j.done / j.total) * 100 : 0;
        const active = j.state === 'downloading' || j.state === 'verifying';
        return (
          <div class={`panel job ${j.state}`}>
            <div class="row between gap">
              <div class="grow">
                <div class="row gap"><span class="tag dark">{j.platform}</span><b class="clip">{j.title}</b>{j.version && <span class="muted">v{j.version}</span>}</div>
                <div class="muted small">
                  {LABEL[j.state] || j.state}
                  {j.platform === 'PS5' && ` · ${j.drive === 'internal' ? 'Internal SSD' : j.drive.toUpperCase()}`}
                  {j.state !== 'done' && j.platform === 'PS5' && ` · ${formatBytes(j.done)} of ${formatBytes(j.total)}`}
                  {j.state === 'done' && j.platform === 'PS5' && ` · ${formatBytes(j.total)}`}
                  {active && j.speed > 0 && ` · ${formatSpeed(j.speed)} · ${formatEta(j.total - j.done, j.speed)}`}
                  {j.replace && j.state !== 'done' && ' · replaces the older copy when done'}
                </div>
              </div>
              <div class="row gap">
                {(j.state === 'queued' || j.state === 'paused') && i > 0 && <button class="icon-btn" aria-label="Move up" onClick={() => run(() => ps5.action(j.id, 'up'))}><Icon d={ICONS.up} /></button>}
                {(j.state === 'queued' || j.state === 'paused') && i < jobs.length - 1 && <button class="icon-btn" aria-label="Move down" onClick={() => run(() => ps5.action(j.id, 'down'))}><Icon d="M12 5v13M6.5 12.5 12 18l5.5-5.5" /></button>}
                {(active || j.state === 'queued') && j.platform === 'PS5' && <button class="btn" onClick={() => run(() => ps5.action(j.id, 'pause'))}><Icon d={ICONS.pause} />Pause</button>}
                {(j.state === 'paused' || j.state === 'failed') && <button class="btn btn-primary" onClick={() => run(() => ps5.action(j.id, 'resume'))}><Icon d={ICONS.play} />{j.state === 'failed' ? 'Try again' : 'Resume'}</button>}
                {(active || j.state === 'queued' || j.state === 'paused' || j.state === 'failed') && <button class="btn" onClick={() => run(() => ps5.action(j.id, 'cancel'))}><Icon d={ICONS.x} />Cancel</button>}
                {(j.state === 'done' || j.state === 'cancelled' || j.state === 'failed') && <button class="icon-btn" aria-label="Remove from list" onClick={() => run(() => ps5.remove(j.id))}><Icon d={ICONS.trash} /></button>}
              </div>
            </div>
            {j.state !== 'done' && j.state !== 'cancelled' && j.platform === 'PS5' && (
              <Bar label={`${j.title} progress`} parts={[{ pct, color: j.state === 'failed' ? '#F07A7A' : j.state === 'paused' ? '#9AA4B2' : '#5B8CFF' }]} />
            )}
            {j.error && <p class="bad-text small">{j.error}</p>}
            {j.note && <p class="muted small">{j.note}</p>}
          </div>
        );
      })}
      <p class="muted small">Downloads carry on in the background while you use the PS5 or play, and resume after a restart once GameVault is loaded again.</p>
      <Hints items={[['✕', 'Select'], ['L1 / R1', 'Switch section']]} />
    </div>
  );
}
