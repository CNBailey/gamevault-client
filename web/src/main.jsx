import { render } from 'preact';
import { useEffect, useMemo, useState, useCallback, useRef } from 'preact/hooks';
import { ps5, vault } from './api.js';
import { installPad } from './pad.js';
import { ACTIVE } from './util.js';
import { Logo, StoragePanel, go } from './components.jsx';
import { Library } from './pages/Library.jsx';
import { Details } from './pages/Details.jsx';
import { Downloads } from './pages/Downloads.jsx';
import { Storage } from './pages/Storage.jsx';
import { Settings } from './pages/Settings.jsx';
import './styles.css';

const TV = /[?&]tv=1/.test(location.search) || /PlayStation/i.test(navigator.userAgent);
if (TV) document.documentElement.classList.add('tv');

const SECTIONS = [
  { id: 'library', label: 'Library' },
  { id: 'downloads', label: 'Downloads' },
  { id: 'storage', label: 'Storage' },
  { id: 'settings', label: 'Settings' }
];

function parseRoute() {
  const h = location.hash.replace(/^#\/?/, '');
  const [page, arg] = h.split('/');
  if (page === 'game' && arg) return { page: 'game', id: Number(arg) };
  return { page: SECTIONS.some((s) => s.id === page) ? page : 'library' };
}

function App() {
  const [route, setRoute] = useState(parseRoute());
  const [state, setState] = useState(null);
  const [stateError, setStateError] = useState(null);
  const [games, setGames] = useState(null);
  const [gamesError, setGamesError] = useState(null);
  const [toast, setToast] = useState(null);
  const handlers = useRef({});

  useEffect(() => {
    const onHash = () => setRoute(parseRoute());
    window.addEventListener('hashchange', onHash);
    return () => window.removeEventListener('hashchange', onHash);
  }, []);

  const refresh = useCallback(async () => {
    try { setState(await ps5.state()); setStateError(null); } catch (e) { setStateError(e.message); }
  }, []);

  useEffect(() => {
    refresh();
    const t = setInterval(refresh, 2000);
    return () => clearInterval(t);
  }, [refresh]);

  const server = state && state.settings.server;
  const api = useMemo(() => vault(server), [server]);

  const loadGames = useCallback(async () => {
    if (!server) return;
    try { const r = await api.games(); setGames(r.games); setGamesError(null); } catch (e) { setGamesError(e.message); }
  }, [api, server]);

  useEffect(() => {
    loadGames();
    const t = setInterval(loadGames, 60000);
    return () => clearInterval(t);
  }, [loadGames]);

  const notify = useCallback((text, kind = 'info') => {
    setToast({ text, kind, at: Date.now() });
    setTimeout(() => setToast((t) => (t && Date.now() - t.at >= 3900 ? null : t)), 4000);
  }, []);

  // L1 / R1 switch sections; ○ goes back; the page gets the rest.
  useEffect(() => installPad((action, e) => {
    const page = handlers.current[action];
    if (page && page(e) !== false) return;
    const idx = SECTIONS.findIndex((s) => s.id === (route.page === 'game' ? 'library' : route.page));
    if (action === 'prev') go(SECTIONS[(idx + SECTIONS.length - 1) % SECTIONS.length].id);
    else if (action === 'next') go(SECTIONS[(idx + 1) % SECTIONS.length].id);
    else if (action === 'back' && route.page === 'game') history.back();
  }), [route]);

  const onKeys = useCallback((h) => { handlers.current = h || {}; }, []);
  useEffect(() => { handlers.current = {}; }, [route]);

  const activeCount = state ? state.downloads.filter((d) => ACTIVE.includes(d.state)).length : 0;
  const ctx = { state, refresh, games, setGames, loadGames, api, notify, onKeys, tv: TV };
  const needsSetup = state && !state.settings.server;
  const section = route.page === 'game' ? 'library' : route.page;

  return (
    <div class="app">
      <header class="top">
        <Logo />
        <nav class="sections" aria-label="Sections">
          {TV && <span class="key-hint">L1</span>}
          {SECTIONS.map((s) => (
            <a href={`#/${s.id}`} class={`section${section === s.id ? ' current' : ''}`} aria-current={section === s.id ? 'page' : undefined}>
              {s.label}
              {s.id === 'downloads' && activeCount > 0 && <span class="count">{activeCount}</span>}
            </a>
          ))}
          {TV && <span class="key-hint">R1</span>}
        </nav>
        <div class="conn">
          <span class={`dot ${stateError ? 'bad' : !server ? 'warn' : gamesError ? 'bad' : 'good'}`} />
          {stateError ? 'PS5 service not answering'
            : !server ? 'GameVault not set up'
              : gamesError ? 'Cannot reach GameVault'
                : games ? `GameVault connected · ${games.length} games` : 'Connecting…'}
        </div>
      </header>

      <div class="body">
        <main class="main">
          {stateError && !state ? (
            <div class="panel empty"><h2>Can't reach the GameVault service on the PS5</h2><p class="muted">{stateError}. Load gamevault.elf again with your payload loader.</p></div>
          ) : !state ? (
            <div class="panel empty"><p class="muted">Loading…</p></div>
          ) : needsSetup && route.page !== 'settings' ? (
            <div class="panel empty">
              <h2>Welcome to GameVault</h2>
              <p class="muted">Enter your GameVault server's address (for example http://192.168.0.32:8686) in Settings to see your games.</p>
              <a class="btn btn-primary" href="#/settings" data-autofocus>Open Settings</a>
            </div>
          ) : route.page === 'game' ? <Details id={route.id} {...ctx} />
            : route.page === 'downloads' ? <Downloads {...ctx} />
              : route.page === 'storage' ? <Storage {...ctx} />
                : route.page === 'settings' ? <Settings {...ctx} />
                  : <Library {...ctx} gamesError={gamesError} />}
        </main>
        {state && route.page !== 'settings' && <StoragePanel state={state} />}
      </div>

      {toast && <div class={`toast ${toast.kind}`} role="status">{toast.text}</div>}
    </div>
  );
}

render(<App />, document.getElementById('app'));
