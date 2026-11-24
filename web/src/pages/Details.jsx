import { useEffect, useState } from 'preact/hooks';
import { ps5 } from '../api.js';
import { Cover, Hints, Icon, ICONS, Bar, go, useAutofocus } from '../components.jsx';
import { formatBytes, gameStatus, freeOn, fwCompare } from '../util.js';

const CATEGORY = { gd: 'Game', gde: 'Game', gp: 'Update', ac: 'Add-on', image: 'Disk image' };

export function Details({ id, state, api, games, setGames, notify, onKeys, refresh }) {
  const [game, setGame] = useState(null);
  const [files, setFiles] = useState(null);
  const [error, setError] = useState(null);
  const [drive, setDrive] = useState(null);
  const [busy, setBusy] = useState(false);
  const [confirmDelete, setConfirmDelete] = useState(false);
  useAutofocus([game ? 1 : 0]);

  useEffect(() => {
    let live = true;
    setGame(null);
    Promise.all([api.game(id), api.files(id).catch(() => null)])
      .then(([g, f]) => { if (live) { setGame(g); setFiles(f); } })
      .catch((e) => live && setError(e.message));
    return () => { live = false; };
  }, [id, api]);

  const present = state.drives.map((d) => d.id);
  const isPs4 = game && game.platform === 'PS4';
  const chosen = isPs4 ? 'internal' : (drive && present.includes(drive) ? drive : present.includes(state.settings.ps5Drive) ? state.settings.ps5Drive : 'internal');
  const space = freeOn(state, chosen);

  const toggleFavourite = async () => {
    try {
      const u = await api.favourite(game.id, !game.favourite);
      setGame({ ...game, favourite: u.favourite });
      if (games) setGames(games.map((x) => (x.id === game.id ? { ...x, favourite: u.favourite } : x)));
    } catch (e) { notify(e.message, 'error'); }
  };
  const cycleDrive = () => {
    if (isPs4 || present.length < 2) return;
    setDrive(present[(present.indexOf(chosen) + 1) % present.length]);
  };

  useEffect(() => { onKeys({ favourite: () => { if (game) toggleFavourite(); }, drive: cycleDrive }); });

  if (error) return <div class="panel empty"><h2>Couldn't load this game</h2><p class="muted">{error}</p><button class="btn" onClick={() => history.back()} data-autofocus>Back</button></div>;
  if (!game) return <p class="muted">Loading…</p>;

  const st = gameStatus(game, state);
  const needed = game.sizeBytes || (files && files.totalBytes) || 0;
  const tooLarge = space && needed > space.free;
  const fwNeeded = game.requiredFirmware;
  const incompatible = !isPs4 && fwNeeded && state.firmware && fwCompare(fwNeeded, state.firmware) > 0;
  const folderPs4 = files && files.kind === 'ps4-folder';

  const start = async (opts = {}) => {
    setBusy(true);
    try {
      await ps5.download(game.id, { drive: isPs4 ? undefined : chosen, ...opts });
      notify(isPs4 ? `Sending ${game.title} to the PS5's installer` : `Downloading ${game.title}`);
      await refresh();
    } catch (e) { notify(e.message, 'error'); }
    setBusy(false);
  };
  const retry = async (job) => {
    setBusy(true);
    try { await ps5.action(job.id, 'resume'); await refresh(); } catch (e) { notify(e.message, 'error'); }
    setBusy(false);
  };
  const remove = async () => {
    setBusy(true);
    try { await ps5.deleteInstalled(st.installed.path); notify(`Deleted ${game.title} from the PS5`); setConfirmDelete(false); await refresh(); } catch (e) { notify(e.message, 'error'); }
    setBusy(false);
  };

  // The main button, as in the designs.
  let button;
  let note;
  let noteClass = 'muted';
  const pct = st.job && st.job.total ? Math.floor((st.job.done / st.job.total) * 100) : 0;
  if (st.status === 'downloading' || st.status === 'queued') {
    button = <button class="big btn-primary" onClick={() => go('downloads')} data-autofocus><Icon d={ICONS.down} size={28} />{st.status === 'queued' ? 'Queued' : `Downloading · ${pct}%`}</button>;
    note = 'Open Downloads to pause or cancel.';
  } else if (st.status === 'paused') {
    button = <button class="big btn-primary" disabled={busy} onClick={() => retry(st.job)} data-autofocus><Icon d={ICONS.play} size={28} />Resume · {pct}%</button>;
    note = 'Paused. Resuming carries on from where it stopped.';
  } else if (st.status === 'downloaded') {
    button = <button class="big done" disabled><Icon d={ICONS.check} size={28} />Downloaded</button>;
    note = `On this PS5 (${st.installed.drive === 'internal' ? 'Internal SSD' : st.installed.drive.toUpperCase()}) · ${st.installed.version ? `v${st.installed.version}, ` : ''}same version as on the NAS.`;
    noteClass = 'good-text';
  } else if (folderPs4) {
    button = <button class="big blocked" disabled><Icon d={ICONS.stop} size={28} />Can't install</button>;
    note = 'This PS4 game is a folder dump. The PS5 can only install PS4 games from .pkg files.';
    noteClass = 'bad-text';
  } else if (incompatible) {
    button = <button class="big blocked" disabled><Icon d={ICONS.stop} size={28} />Needs firmware {fwNeeded}</button>;
    note = `This version needs firmware ${fwNeeded} or later; this PS5 is on ${state.firmware}.`;
    noteClass = 'bad-text';
  } else if (tooLarge) {
    button = <button class="big blocked" onClick={() => go('storage')} data-autofocus><Icon d={ICONS.stop} size={28} />Too large</button>;
    note = `Needs ${formatBytes(needed)}; ${space.drive.label} has ${formatBytes(space.free)} free. Free up space in Storage${present.length > 1 && !isPs4 ? ', or change the drive with □' : ''}.`;
    noteClass = 'bad-text';
  } else if (st.status === 'update') {
    button = <button class="big btn-warn" disabled={busy} onClick={() => start({ replace: st.installed.kind === 'ps4-app' ? undefined : st.installed.path })} data-autofocus><Icon d={ICONS.up} size={28} />Update · {formatBytes(needed)}</button>;
    note = `On this PS5 as v${st.installed.version}. The NAS has v${game.version}.${st.installed.kind !== 'ps4-app' ? ' The old copy is deleted once the new one is complete.' : ''}`;
    noteClass = 'warn-text';
  } else {
    button = <button class="big btn-primary" disabled={busy} onClick={() => start()} data-autofocus><Icon d={ICONS.down} size={28} />{isPs4 ? 'Install' : 'Download'}</button>;
    note = st.status === 'failed' && st.failed
      ? `The last attempt failed: ${st.failed.error}`
      : isPs4 ? `The PS5 installs PS4 games itself, to the drive set in its Storage settings (Installation location). ${space ? `${formatBytes(space.free)} free on the internal SSD.` : ''}`
        : `Fits on ${space ? space.drive.label : 'the drive'}: ${space ? formatBytes(space.free) : '—'} free, ${formatBytes(needed)} needed.`;
    if (st.status === 'failed') noteClass = 'bad-text';
  }

  const backdrop = api.media(game.backdrop);
  const usedPct = space ? ((space.drive.total - space.drive.free) / space.drive.total) * 100 : 0;
  const needPct = space ? (needed / space.drive.total) * 100 : 0;

  return (
    <div class="details">
      {backdrop && <div class="backdrop" style={{ backgroundImage: `url("${backdrop}")` }} />}
      <div class="crumb muted" data-nav-skip><a href="#/library" tabIndex={-1} onClick={(e) => { e.preventDefault(); history.back(); }}>Library</a> › {game.title}</div>
      <div class="hero">
        <Cover game={game} api={api} large />
        <div class="hero-text">
          <div class="row gap">
            <span class="tag dark">{game.platform}</span>
            {game.genres && game.genres.length > 0 && <span class="tag">{game.genres.slice(0, 3).join(' · ')}</span>}
          </div>
          <h1>{game.title}</h1>
          <p class="meta">{[game.year, game.developer, game.publisher, game.criticScore != null ? `Critic score ${game.criticScore}` : null].filter(Boolean).join(' · ')}</p>
          {game.summary && <p class="summary">{game.summary}</p>}
          <div class="row gap actions">
            {button}
            <button class={`big-icon${game.favourite ? ' on' : ''}`} onClick={toggleFavourite} aria-pressed={game.favourite} aria-label={game.favourite ? 'Remove from favourites' : 'Add to favourites'}>
              <Icon d={ICONS.star} size={30} stroke={2} />
            </button>
            {st.installed && st.installed.kind !== 'ps4-app' && st.status !== 'downloading' && (
              confirmDelete
                ? <button class="big danger" disabled={busy} onClick={remove}><Icon d={ICONS.trash} size={26} />Delete {formatBytes(st.installed.size)}?</button>
                : <button class="big subtle" onClick={() => setConfirmDelete(true)}><Icon d={ICONS.trash} size={26} />Delete from PS5</button>
            )}
          </div>
          <p class={`note ${noteClass}`}>{note}</p>
        </div>
      </div>

      <div class="facts">
        <div class="panel fact"><div class="muted">Download size</div><b>{formatBytes(needed)}</b></div>
        <button class="panel fact" onClick={cycleDrive} disabled={isPs4 || present.length < 2}>
          <div class="muted">Saves to</div>
          <b>{isPs4 ? 'PS5 installer' : space ? space.drive.label : chosen}</b>
          {!isPs4 && present.length > 1 && <div class="muted small">change: □</div>}
        </button>
        <div class="panel fact"><div class="muted">Version on NAS</div><b>{game.version ? `v${game.version}` : '—'}</b><div class="muted small">{st.installed ? `On PS5: v${st.installed.version || '?'}` : 'Not on this PS5'}</div></div>
        <div class="panel fact"><div class="muted">Needs firmware</div><b class={incompatible ? 'bad-text' : ''}>{fwNeeded || '—'}</b><div class="muted small">{isPs4 ? 'PS4 system version' : `This PS5: ${state.firmware || '?'}${fwNeeded && !incompatible ? ' ✓' : ''}`}</div></div>
      </div>

      {space && !isPs4 && (
        <div class="panel">
          <div class="row between"><span>{space.drive.label}</span><b>{formatBytes(space.free)} free</b></div>
          <Bar label="Space" parts={[{ pct: usedPct, color: '#5B8CFF' }, { pct: st.status === 'download' || st.status === 'update' ? needPct : 0, color: tooLarge ? '#F07A7A' : '#A9C2FF' }]} />
          <div class="muted small">{tooLarge ? `Needs ${formatBytes(needed)} · ${formatBytes(needed - space.free)} short` : st.status === 'download' || st.status === 'update' ? `Light blue: this download (${formatBytes(needed)})` : `${formatBytes(space.drive.total - space.drive.free)} of ${formatBytes(space.drive.total)} used`}</div>
        </div>
      )}

      {files && files.files && files.files.length > 1 && (
        <div class="panel">
          <h2>Files</h2>
          <ul class="files">
            {files.files.map((f) => <li class="row between"><span class="clip">{f.path}</span><span class="muted">{[CATEGORY[f.category], f.version ? `v${f.version}` : null, formatBytes(f.size)].filter(Boolean).join(' · ')}</span></li>)}
          </ul>
          {isPs4 && <p class="muted small">The game, its updates and add-ons are sent to the installer in that order.</p>}
        </div>
      )}

      {game.screenshots && game.screenshots.length > 0 && (
        <div class="shots">{game.screenshots.slice(0, 4).map((s) => <img src={api.media(s)} alt="" loading="lazy" />)}</div>
      )}
      <Hints items={[['✕', 'Select'], ['□', 'Change drive'], ['△', 'Favourite'], ['○', 'Back']]} />
    </div>
  );
}
