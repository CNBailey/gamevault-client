import { useEffect, useMemo, useState } from 'preact/hooks';
import { Cover, StatusBadge, Hints, Icon, ICONS, useAutofocus } from '../components.jsx';
import { formatBytes, gameStatus, load, save } from '../util.js';

const FILTERS = ['All', 'PS5', 'PS4', 'On this PS5', 'Not on this PS5', 'Updates', 'Favourites'];
const SORTS = {
  title: ['Title A–Z', (a, b) => a.title.localeCompare(b.title)],
  added: ['Date added', (a, b) => String(b.addedAt || '').localeCompare(String(a.addedAt || ''))],
  release: ['Release date', (a, b) => String(b.releaseDate || '').localeCompare(String(a.releaseDate || ''))],
  size: ['Size', (a, b) => (b.sizeBytes || 0) - (a.sizeBytes || 0)]
};

export function Library({ state, games, setGames, api, notify, onKeys, gamesError, loadGames }) {
  const saved = load('library', {});
  const [filter, setFilter] = useState(FILTERS.includes(saved.filter) ? saved.filter : 'All');
  const [sort, setSort] = useState(SORTS[saved.sort] ? saved.sort : 'title');
  const [query, setQuery] = useState('');
  useEffect(() => save('library', { filter, sort }), [filter, sort]);
  useAutofocus([games ? 1 : 0]);

  const rows = useMemo(() => {
    if (!games) return [];
    const q = query.trim().toLowerCase();
    return games
      .map((g) => ({ g, st: gameStatus(g, state) }))
      .filter(({ g, st }) => {
        if (q && !g.title.toLowerCase().includes(q) && !String(g.titleId || '').toLowerCase().includes(q)) return false;
        const on = st.status === 'downloaded' || st.status === 'update';
        switch (filter) {
          case 'PS5': case 'PS4': return g.platform === filter;
          case 'On this PS5': return on;
          case 'Not on this PS5': return !on;
          case 'Updates': return st.status === 'update';
          case 'Favourites': return g.favourite;
          default: return true;
        }
      })
      .sort((a, b) => SORTS[sort][1](a.g, b.g));
  }, [games, state, filter, sort, query]);

  const toggleFavourite = async (id) => {
    const g = games.find((x) => x.id === id);
    if (!g) return;
    try {
      const updated = await api.favourite(id, !g.favourite);
      setGames(games.map((x) => (x.id === id ? { ...x, favourite: updated.favourite } : x)));
      notify(updated.favourite ? `Added ${g.title} to favourites` : `Removed ${g.title} from favourites`);
    } catch (e) { notify(`Couldn't change the favourite: ${e.message}`, 'error'); }
  };

  useEffect(() => {
    onKeys({
      favourite: () => {
        const el = document.activeElement;
        const id = el && el.dataset && Number(el.dataset.gameId);
        if (!id) return false;
        toggleFavourite(id);
      },
      drive: () => setFilter((f) => FILTERS[(FILTERS.indexOf(f) + 1) % FILTERS.length]),
      search: () => { const s = document.getElementById('search'); if (s) s.focus(); }
    });
  });

  const onCount = state.installed.length;
  const updates = games ? games.filter((g) => gameStatus(g, state).status === 'update').length : 0;

  return (
    <div class="stack">
      <div class="row wrap gap">
        <div class="chips" role="group" aria-label="Filter">
          {FILTERS.map((f) => (
            <button class={`chip${filter === f ? ' on' : ''}`} aria-pressed={filter === f} onClick={() => setFilter(f)}>{f}</button>
          ))}
        </div>
        <div class="row gap push">
          <input id="search" class="input search" type="search" placeholder="Search" value={query} onInput={(e) => setQuery(e.target.value)} aria-label="Search games" />
          <select class="select" value={sort} onChange={(e) => setSort(e.target.value)} aria-label="Sort">
            {Object.entries(SORTS).map(([k, [label]]) => <option value={k}>{label}</option>)}
          </select>
        </div>
      </div>

      <div class="legend muted">
        <span><span class="badge good mini"><Icon d={ICONS.check} size={14} stroke={3.4} /></span>On this PS5 ({onCount})</span>
        <span><span class="badge warn mini"><Icon d={ICONS.up} size={14} stroke={3.4} /></span>Update available ({updates})</span>
        <span><span class="badge busy mini"><Icon d={ICONS.down} size={14} stroke={3.4} /></span>Downloading</span>
      </div>

      {gamesError && !games ? (
        <div class="panel empty">
          <h2>Can't reach GameVault</h2>
          <p class="muted">{gamesError}. Check the address in Settings, and that the PS5 and the NAS are on the same network.</p>
          <div class="row gap"><button class="btn" onClick={loadGames}>Try again</button><a class="btn" href="#/settings">Settings</a></div>
        </div>
      ) : !games ? (
        <p class="muted">Loading games…</p>
      ) : rows.length === 0 ? (
        <div class="panel empty"><p class="muted">No games match.</p></div>
      ) : (
        <div class="grid">
          {rows.map(({ g, st }) => (
            <a href={`#/game/${g.id}`} class="tile" data-nav data-game-id={g.id} aria-label={`${g.title}, ${g.platform}`}>
              <Cover game={g} api={api}>
                <span class="plat">{g.platform}</span>
                {g.favourite && <span class="fav" aria-label="Favourite"><Icon d={ICONS.star} size={18} stroke={2} /></span>}
                <StatusBadge st={st} />
              </Cover>
              <span class="tile-title clip">{g.title}</span>
              <span class={`tile-sub clip ${st.status === 'downloaded' ? 'good-text' : st.status === 'update' ? 'warn-text' : ''}`}>
                {formatBytes(g.sizeBytes)}{st.status === 'update' ? ` · update v${g.version}` : st.status === 'downloaded' ? ' · on this PS5' : ''}
              </span>
            </a>
          ))}
        </div>
      )}
      <Hints items={[['✕', 'Open'], ['△', 'Favourite'], ['□', 'Filter'], ['L1 / R1', 'Switch section']]} />
    </div>
  );
}
