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

export function Settings({ state, refresh, notify, loadGames, onKeys }) {
  const [form, setForm] = useState(state.settings);
  const [test, setTest] = useState(null);
  const [saving, setSaving] = useState(false);
  const [keyTest, setKeyTest] = useState(false);
  useAutofocus([]);
  useEffect(() => { onKeys({}); });

  const set = (k, v) => setForm({ ...form, [k]: v });
  const dirty = JSON.stringify(form) !== JSON.stringify(state.settings);
  const present = state.drives.map((d) => d.id);

  const saveAll = async () => {
    setSaving(true);
    try {
      const s = await ps5.saveSettings(form);
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
            {[1, 2, 3, 4, 6, 8].map((n) => <option value={n}>{n}{n === 4 ? ' (recommended)' : ''}</option>)}
          </select>
        </label>
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
        <p class="muted small">This version downloads over HTTP from GameVault, using several connections at once, which is usually as fast as the network allows. NFS isn't built in yet, so Automatic and NFS both use HTTP for now; your NFS share is kept for when it is.</p>
      </section>

      <div class="row gap">
        <button class="btn btn-primary btn-lg" onClick={saveAll} disabled={!dirty || saving}>{saving ? 'Saving…' : 'Save settings'}</button>
        {dirty && <button class="btn btn-lg" onClick={() => setForm(state.settings)}>Undo changes</button>}
      </div>

      <section class="panel stack">
        <h2>Controller and console</h2>
        <div class="row gap wrap">
          <button class="btn" onClick={() => setKeyTest(true)}>Controller test</button>
          <button class="btn" onClick={() => ps5.notifyTest().then(() => notify('Sent a test notification'), (e) => notify(e.message, 'error'))}>Test notification</button>
          <button class="btn" onClick={() => { if (confirm('Stop GameVault on the PS5? Downloads pause until you load it again.')) ps5.quit().then(() => notify('GameVault stopped')); }}>Stop GameVault</button>
        </div>
        <p class="muted small">GameVault {state.version} · PS5 firmware {state.firmware || 'unknown'} · open this page from a phone or PC at <code>http://&lt;PS5 address&gt;:8687</code></p>
      </section>
      <Hints items={[['✕', 'Select'], ['L1 / R1', 'Switch section']]} />
    </div>
  );
}
