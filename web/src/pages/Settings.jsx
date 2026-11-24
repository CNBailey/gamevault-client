import { useEffect, useState } from 'preact/hooks';
import { ps5 } from '../api.js';
import { Hints, useAutofocus } from '../components.jsx';
import { ACTIONS, keyMap, setKey, resetKeys } from '../pad.js';
import { formatBytes } from '../util.js';

const ALL_DRIVES = [['internal', 'Internal SSD']]
  .concat([0, 1, 2, 3, 4, 5, 6, 7].map((i) => [`usb${i}`, `USB drive ${i + 1}`]))
  .concat([0, 1, 2, 3].map((i) => [`ext${i}`, `M.2 / extended storage ${i + 1}`]));

function KeyTest({ onClose }) {
  const [last, setLast] = useState(null);
  const [learning, setLearning] = useState(null);
  const [, bump] = useState(0);
  useEffect(() => {
    window.__gvKeyTest = true;
    const h = (e) => {
      e.preventDefault();
      e.stopPropagation();
      const code = e.keyCode || e.which;
      setLast({ code, key: e.key });
      if (learning) { setKey(learning, code); setLearning(null); bump((n) => n + 1); return; }
      if (code === 27 && !learning) onClose();
    };
    window.addEventListener('keydown', h, true);
    return () => { window.removeEventListener('keydown', h, true); window.__gvKeyTest = false; };
  }, [learning]);
  const keys = keyMap();
  return (
    <div class="panel keytest" role="dialog" aria-label="Controller test">
      <h2>Controller test</h2>
      <p class="muted">Press any button to see the code the PS5 browser sends. To teach GameVault a button, choose the action with the D-pad and ✕, then press the button.</p>
      <div class="keycode">{last ? `Key code ${last.code}${last.key ? ` (${last.key})` : ''}` : 'Press a button…'}</div>
      <div class="list">
        {Object.entries(ACTIONS).map(([a, label]) => (
          <div class="row between">
            <span>{label}</span>
            <span class="row gap">
              <span class="muted">{(keys[a] || []).join(', ') || 'not set'}</span>
              <button class={`btn btn-sm${learning === a ? ' btn-primary' : ''}`} onClick={() => setLearning(a)}>{learning === a ? 'Press the button…' : 'Learn'}</button>
            </span>
          </div>
        ))}
      </div>
      <div class="row gap">
        <button class="btn" onClick={() => { resetKeys(); bump((n) => n + 1); }}>Reset buttons</button>
        <button class="btn btn-primary" onClick={onClose}>Done</button>
      </div>
      <p class="muted small">The arrow keys and ✕ (Enter) always move and select, so the test can be left with the D-pad and ✕.</p>
    </div>
  );
}

function HomeScreen({ state, notify }) {
  const t = state.tile || {};
  const [busy, setBusy] = useState(false);
  const update = async () => {
    setBusy(true);
    try {
      const r = await ps5.update();
      notify(r && r.restarting ? `Restarting as GameVault ${t.elfVersion}…` : 'Updated');
      // The page reloads once the new version answers.
      const started = Date.now();
      const poll = setInterval(async () => {
        try {
          const s = await ps5.state();
          if (s.version !== state.version || Date.now() - started > 30000) { clearInterval(poll); location.reload(); }
        } catch (e) { /* still restarting */ }
      }, 1500);
    } catch (e) { notify(e.message, 'error'); setBusy(false); }
  };
  return (
    <section class="panel stack">
      <h2>Home screen and updates</h2>
      {t.installed ? (
        <p>GameVault has put its tile in <code>{t.path}</code>. ShadowMountPlus adds it to the home screen the next time it scans (load ShadowMountPlus again if it doesn't appear). Pressing the tile opens GameVault{t.elfVersion ? ', and starts it first if it isn’t running' : ''}.</p>
      ) : (
        <p class="bad-text">{t.error || 'The tile has not been set up yet.'}</p>
      )}
      {!t.elfVersion && (
        <p class="muted small">To let the tile start GameVault after a restart, the PS5 needs a copy of GameVault from your GameVault server. {t.checkError ? `Not yet: ${t.checkError}.` : 'Getting it…'}</p>
      )}
      <div class="row gap wrap">
        <span>This PS5: GameVault {state.version}{t.latestVersion ? ` · newest: ${t.latestVersion}` : ''}</span>
        {t.updateReady ? (
          <button class="btn btn-primary" disabled={busy} onClick={update}>{busy ? 'Restarting…' : `Update to ${t.elfVersion}`}</button>
        ) : t.updateAvailable ? (
          <span class="muted">Downloading {t.latestVersion}…</span>
        ) : (
          <button class="btn" onClick={() => ps5.updateCheck().then(() => notify('Checking GameVault for a newer version'))}>Check for updates</button>
        )}
      </div>
      {t.updateReady && <p class="muted small">Updating restarts GameVault; a download in progress pauses for a moment and carries on.</p>}
    </section>
  );
}

function SpeedTest({ state, games, notify, onDone }) {
  const [result, setResult] = useState(null);
  const [running, setRunning] = useState(false);
  const candidates = (games || []).filter((g) => g.platform === 'PS5' && g.sizeBytes >= 64 * 1024 * 1024).sort((a, b) => b.sizeBytes - a.sizeBytes);
  const run = async () => {
    if (!candidates.length) { notify('No PS5 game of 64 MB or more to test with', 'error'); return; }
    setRunning(true);
    setResult(null);
    try { setResult(await ps5.speedTest(candidates[0].id)); onDone(); } catch (e) { notify(e.message, 'error'); }
    setRunning(false);
  };
  const s = state.settings;
  return (
    <div class="stack">
      <div class="row gap wrap">
        <button class="btn" onClick={run} disabled={running}>{running ? 'Testing… (about 16 seconds)' : 'Test download speed'}</button>
        {!result && s.measuredConnections > 0 && <span class="muted">Last test: {s.measuredMBps} MB/s with {s.measuredConnections} connection{s.measuredConnections === 1 ? '' : 's'}</span>}
      </div>
      {result && (
        <div class="speed">
          {result.results.map((r) => (
            <div class={`row between${r.connections === result.best ? ' good-text' : ''}`}>
              <span>{r.connections} connection{r.connections === 1 ? '' : 's'}</span>
              <b>{r.mbps.toFixed(0)} MB/s{r.connections === result.best ? ' · used by Automatic' : ''}</b>
            </div>
          ))}
        </div>
      )}
      <p class="muted small">Reads part of your largest PS5 game from GameVault with 1, 2, 4 and 6 connections, without writing anything, and keeps the fewest connections that reach the top speed.</p>
    </div>
  );
}

function Diagnostics() {
  const [text, setText] = useState(null);
  const show = (which) => {
    if (which === 'service') {
      ps5.log().then((lines) => setText(lines.length ? lines.join('\n') : 'Nothing logged yet.'), (e) => setText(`Couldn't read the log: ${e.message}`));
    } else {
      ps5.diagnostics(which).then(setText, (e) => setText(`Couldn't read the log: ${e.message}`));
    }
  };
  return (
    <section class="panel stack">
      <h2>Diagnostics</h2>
      <div class="row gap wrap">
        <button class="btn" onClick={() => show('service')}>Service log</button>
        <button class="btn" onClick={() => show('app')}>Home-screen app log</button>
        <button class="btn" onClick={() => show('smp')}>ShadowMountPlus log</button>
      </div>
      {text != null && <pre class="log">{text}</pre>}
    </section>
  );
}

export function Settings({ state, refresh, notify, loadGames, onKeys, games }) {
  const [form, setForm] = useState(state.settings);
  const [test, setTest] = useState(null);
  const [saving, setSaving] = useState(false);
  const [keyTest, setKeyTest] = useState(false);
  useAutofocus([]);
  useEffect(() => { onKeys({}); });

  const set = (k, v) => setForm({ ...form, [k]: v });
  const EDITABLE = ['server', 'ps5Drive', 'connections', 'playLimitMBps', 'notify', 'checkFiles', 'transfer', 'nfsExport'];
  const dirty = EDITABLE.some((k) => form[k] !== state.settings[k]);
  const present = state.drives.map((d) => d.id);

  const saveAll = async () => {
    setSaving(true);
    try {
      const s = await ps5.saveSettings(Object.fromEntries(EDITABLE.map((k) => [k, form[k]])));
      setForm(s);
      notify('Settings saved');
      await refresh();
      loadGames();
    } catch (e) { notify(e.message, 'error'); }
    setSaving(false);
  };
  const runTest = async () => {
    setTest({ running: true });
    try { setTest(await ps5.testConnection(form.server)); } catch (e) { setTest({ ok: false, error: e.message }); }
  };

  if (keyTest) return <KeyTest onClose={() => setKeyTest(false)} />;

  return (
    <div class="stack settings">
      <h1 class="page-title">Settings</h1>

      <section class="panel stack">
        <h2>GameVault server</h2>
        <label class="field">Address
          <input class="input" type="url" value={form.server} placeholder="http://192.168.0.32:8686" onInput={(e) => set('server', e.target.value)} data-autofocus />
        </label>
        <div class="row gap">
          <button class="btn" onClick={runTest} disabled={!form.server || (test && test.running)}>Test connection</button>
          {test && !test.running && (test.ok
            ? <span class="good-text">Connected · GameVault {test.serverVersion} · {test.games} games · {test.latencyMs} ms</span>
            : <span class="bad-text">{test.error}</span>)}
          {test && test.running && <span class="muted">Testing…</span>}
        </div>
        <p class="muted small">Use the NAS's IP address and GameVault's port (8686). The PS5 and the NAS need to be on the same network.</p>
      </section>

      <section class="panel stack">
        <h2>Download settings</h2>
        <label class="field">PS5 games: save to
          <select class="select" value={form.ps5Drive} onChange={(e) => set('ps5Drive', e.target.value)}>
            {ALL_DRIVES.filter(([id]) => present.includes(id) || id === form.ps5Drive).map(([id, label]) => {
              const d = state.drives.find((x) => x.id === id);
              return <option value={id}>{label}{d ? ` · ${formatBytes(d.free)} free` : ' · not connected'}</option>;
            })}
          </select>
        </label>
        <p class="muted small">PS5 games are saved as files in <code>homebrew</code> on that drive (<code>/data/homebrew</code> on the internal SSD), where ShadowMountPlus finds them. You can still pick another drive for a single game with □ on its page.</p>
        <div class="field">PS4 games: save to
          <div class="readonly">The PS5's own setting</div>
        </div>
        <p class="muted small">PS4 games go through the PS5's installer, which always uses Settings › Storage › Installation location › PS4 games on the console. Change that to install PS4 games to an external drive.</p>
        <label class="field">Connections per download
          <select class="select" value={form.connections} onChange={(e) => set('connections', Number(e.target.value))}>
            <option value={0}>Automatic{state.settings.measuredConnections ? ` (${state.settings.measuredConnections}, from the speed test)` : ' (2 until you run the speed test)'}</option>
            {[1, 2, 3, 4, 6, 8].map((n) => <option value={n}>{n}</option>)}
          </select>
        </label>
        <SpeedTest state={state} games={games} notify={notify} onDone={refresh} />
        <label class="field">While a game is running, limit downloads to
          <select class="select" value={form.playLimitMBps} onChange={(e) => set('playLimitMBps', Number(e.target.value))}>
            {[0, 5, 10, 20, 50].map((n) => <option value={n}>{n ? `${n} MB/s` : 'No limit'}</option>)}
          </select>
        </label>
        <label class="check"><input type="checkbox" checked={form.notify} onChange={(e) => set('notify', e.target.checked)} /> Show a notification when a download finishes or fails</label>
        <label class="check"><input type="checkbox" checked={form.checkFiles} onChange={(e) => set('checkFiles', e.target.checked)} /> Check each image after it downloads</label>
      </section>

      <section class="panel stack">
        <h2>Transfer method</h2>
        <label class="field">Method
          <select class="select" value={form.transfer} onChange={(e) => set('transfer', e.target.value)}>
            <option value="auto">Automatic: fastest available (recommended)</option>
            <option value="http">HTTP</option>
            <option value="nfs">NFS</option>
          </select>
        </label>
        <label class="field">NFS share (for when NFS is supported)
          <input class="input" value={form.nfsExport} placeholder="192.168.0.32:/volume1/Games" onInput={(e) => set('nfsExport', e.target.value)} />
        </label>
        <p class="muted small">This version downloads over HTTP from GameVault, using the number of connections above. NFS isn't built in yet, so Automatic and NFS both use HTTP for now; your NFS share is kept for when it is.</p>
      </section>

      <div class="row gap">
        <button class="btn btn-primary btn-lg" onClick={saveAll} disabled={!dirty || saving}>{saving ? 'Saving…' : 'Save settings'}</button>
        {dirty && <button class="btn btn-lg" onClick={() => setForm(state.settings)}>Undo changes</button>}
      </div>

      <HomeScreen state={state} notify={notify} />

      <section class="panel stack">
        <h2>Controller and console</h2>
        <div class="row gap wrap">
          <button class="btn" onClick={() => setKeyTest(true)}>Controller test</button>
          <button class="btn" onClick={() => ps5.notifyTest().then(() => notify('Sent a test notification'), (e) => notify(e.message, 'error'))}>Test notification</button>
          <button class="btn" onClick={() => { if (confirm('Stop GameVault on the PS5? Downloads pause until you load it again.')) ps5.quit().then(() => notify('GameVault stopped')); }}>Stop GameVault</button>
        </div>
        <p class="muted small">GameVault {state.version} · PS5 firmware {state.firmware || 'unknown'} · open this page from a phone or PC at <code>http://&lt;PS5 address&gt;:8687</code></p>
      </section>
      <Diagnostics />
      <Hints items={[['✕', 'Select'], ['L1 / R1', 'Switch section']]} />
    </div>
  );
}
