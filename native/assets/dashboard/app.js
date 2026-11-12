// Rondo dashboard. Plain ES module, no build step, no dependencies. Talks to the API the Rondo
// serves while Settings › Dashboard is open (docs/dev/dashboard-api.md). The Y1 is slow, so: few
// requests, art loaded lazily and a few at a time, status polled only while the tab is visible.

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const icon = (name, cls = 'i') => `<svg class="${cls}" aria-hidden="true"><use href="#i-${name}"/></svg>`;
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const MARK_INNER = '<g class="mark-ring" fill="none" stroke-width="13" stroke-linecap="round"><path d="M57.965 2.024A58 58 0 1 1 12.059-56.733" stroke="#f5f5f7"/><path d="M29-50.229A58 58 0 0 1 55.753-15.987" stroke="#ff3b4f"/></g><path d="M-9-15v30L16 0z" fill="#ff3b4f" stroke="#ff3b4f" stroke-width="3" stroke-linejoin="round"/>';
const spinner = (cls = 'spin') => `<svg class="${cls}" viewBox="-66 -66 132 132" aria-hidden="true">${MARK_INNER}</svg>`;
const markTile = '<svg class="i" aria-hidden="true"><use href="#mark"/></svg>';

const store = {
  get(k) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k, v) { try { v == null ? localStorage.removeItem(k) : localStorage.setItem(k, v); } catch { /* private mode */ } },
};

const S = {
  session: store.get('rondo_session'),
  booted: false,
  online: true,
  route: 'overview',
  status: null, statusAt: 0,
  theme: null,
  library: null, libByPath: new Map(), libKind: 'songs', libSort: store.get('rondo_sort') || 'title', libQuery: '', libRows: [], libShown: 0,
  settings: null, wifi: null, server: null,
  uploads: [],
  pvSel: null,
};

// ---------- formatting ----------
function fmtTime(ms) {
  const s = Math.max(0, Math.floor((ms || 0) / 1000));
  const m = Math.floor(s / 60) % 60, h = Math.floor(s / 3600), ss = String(s % 60).padStart(2, '0');
  return h ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${m}:${ss}`;
}
const fmtDur = (sec) => fmtTime((sec || 0) * 1000);
// whole seconds of length minus whole seconds played, so elapsed and remaining tick together (like the device)
const remainMs = (dur, pos) => Math.max(0, Math.floor(dur / 1000) - Math.floor(pos / 1000)) * 1000;
function fmtBytes(b) {
  if (!Number.isFinite(b)) return '';
  if (b >= 1e9) return `${(b / 1e9).toFixed(1)} GB`;
  if (b >= 1e6) return `${(b / 1e6).toFixed(b >= 1e8 ? 0 : 1)} MB`;
  return `${Math.max(1, Math.round(b / 1e3))} KB`;
}
const plural = (n, one, many = one + 's') => `${n.toLocaleString('en')} ${n === 1 ? one : many}`;
const fileName = (p) => (p || '').split('/').pop();
const fold = (s) => (s || '').toLowerCase().normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/ı/g, 'i').replace(/ə/g, 'e');
const collator = new Intl.Collator(undefined, { sensitivity: 'base', numeric: true });

// ---------- color ----------
function hexToRgb(hex) {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex || '');
  const n = m ? parseInt(m[1], 16) : 0x1c5ccc;
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}
const rgbToHex = (r, g, b) => '#' + [r, g, b].map((v) => Math.round(clamp(v, 0, 255)).toString(16).padStart(2, '0')).join('');
function rgbToHsl(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), l = (max + min) / 2;
  if (max === min) return [0, 0, l * 100];
  const d = max - min, s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
  const h = max === r ? (g - b) / d + (g < b ? 6 : 0) : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return [h * 60, s * 100, l * 100];
}
function hslToHex(h, s, l) {
  h = ((h % 360) + 360) % 360; s = clamp(s, 0, 100) / 100; l = clamp(l, 0, 100) / 100;
  const k = (n) => (n + h / 30) % 12, a = s * Math.min(l, 1 - l);
  const f = (n) => l - a * Math.max(-1, Math.min(k(n) - 3, Math.min(9 - k(n), 1)));
  return rgbToHex(f(0) * 255, f(8) * 255, f(4) * 255);
}
function luminance([r, g, b]) {
  const c = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; };
  return 0.2126 * c(r) + 0.7152 * c(g) + 0.0722 * c(b);
}
// the selection gradient: a lighter top over the accent, like iPod blue #5aa8f6 -> #1c5ccc
function accentTop(hex) {
  const [h, s, l] = rgbToHsl(...hexToRgb(hex));
  return hslToHex(h - 8, s + 12, l + Math.min(20, Math.max(0, 90 - l) * 0.45));
}
function applyAccent(hex) {
  const rgb = hexToRgb(hex);
  const [h, s, l] = rgbToHsl(...rgb);
  const root = document.documentElement.style;
  root.setProperty('--accent', rgbToHex(...rgb));
  root.setProperty('--accent-top', accentTop(hex));
  root.setProperty('--sel-top', hslToHex(h - 4, s + 6, l + Math.min(9, Math.max(0, 80 - l) * 0.3)));
  root.setProperty('--accent-hi', hslToHex(h, s + 6, Math.max(l, 66)));
  root.setProperty('--on-accent', luminance(rgb) > 0.27 ? '#111' : '#fff');
  root.setProperty('--accent-sel', `rgb(${rgb.join(' ')} / .5)`);
}
const darker = (hex, f) => rgbToHex(...hexToRgb(hex).map((v) => v * f));
// ArtColor.dominant: average of a 12x12 sample, saturated, value kept mid
function artColor(img) {
  try {
    const c = document.createElement('canvas');
    c.width = c.height = 12;
    const g = c.getContext('2d', { willReadFrequently: true });
    g.drawImage(img, 0, 0, 12, 12);
    const d = g.getImageData(0, 0, 12, 12).data;
    let r = 0, gr = 0, b = 0;
    for (let i = 0; i < d.length; i += 4) { r += d[i]; gr += d[i + 1]; b += d[i + 2]; }
    const n = d.length / 4;
    r /= n * 255; gr /= n * 255; b /= n * 255;
    const max = Math.max(r, gr, b), min = Math.min(r, gr, b);
    let h = 0;
    if (max !== min) h = (max === r ? (gr - b) / (max - min) + (gr < b ? 6 : 0) : max === gr ? (b - r) / (max - min) + 2 : (r - gr) / (max - min) + 4) * 60;
    const s = max ? Math.min(((max - min) / max) * 1.4, 0.85) : 0;
    const v = clamp(max, 0.35, 0.6);
    const f = (k) => { const kk = (k + h / 60) % 6; return v - v * s * Math.max(0, Math.min(kk, 4 - kk, 1)); };
    return rgbToHex(f(5) * 255, f(3) * 255, f(1) * 255);
  } catch { return null; }
}

// ---------- small SVGs drawn per value ----------
// battery charge color by level, like the device (batteryColor in IconView.kt): green from 50 %, amber from 20 %, red below
const battColor = (level) => (level >= 50 ? '#34c759' : level >= 20 ? '#ff9f0a' : '#ff3b30');
function battSvg(level, cls = 'i', charging = false, colored = true) {
  const w = (16.6 * clamp(level / 100, 0, 1)).toFixed(2);
  const fill = !colored ? 'currentColor' : charging ? '#34c759' : battColor(level);
  return `<svg class="${cls}" viewBox="0 0 24 24" aria-hidden="true"><rect x="0.9" y="6.6" width="19.4" height="10.8" rx="2.2" fill="none" stroke="currentColor" stroke-width="1.5"/><rect x="21.4" y="9.6" width="2.1" height="4.8" rx=".8" fill="currentColor"/><rect x="2.6" y="8.3" width="${w}" height="7.4" rx="1" fill="${fill}"/></svg>`;
}
function sigSvg(level, cls = 'sig') {
  const on = (n) => (level >= n ? '' : ' class="off"');
  return `<svg class="${cls}" viewBox="0 0 24 24" aria-hidden="true"><g fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><path${on(4)} d="M4.22 8.22a11 11 0 0 1 15.56 0"/><path${on(3)} d="M7.05 11.05a7 7 0 0 1 9.9 0"/><path${on(2)} d="M9.88 13.88a3 3 0 0 1 4.24 0"/></g><circle${on(1)} cx="12" cy="18.5" r="1.7" fill="currentColor"/></svg>`;
}
// the official logos (YouTube Music: 176-unit grid; YouTube: 28.57 × 20), as on the device (Widgets.kt).
// Small copies get a thicker ring, like the device, so it doesn't vanish at tile size.
const ytmSvg = (cls, ring) => `<svg class="${cls}" viewBox="0 0 176 176" aria-hidden="true"><circle cx="88" cy="88" r="88" fill="#f00"/><circle cx="88" cy="88" r="44" fill="none" stroke="#fff" stroke-width="${ring}"/><path d="M72 111L111 87L72 65Z" fill="#fff"/></svg>`;
const ytSvg = (cls) => `<svg class="${cls}" viewBox="0 0 28.57 20" aria-hidden="true"><path d="M27.9727 3.12324C27.6435 1.89323 26.6768 .926623 25.4468 .597366C23.2197 0 14.285 0 14.285 0C14.285 0 5.35042 0 3.12323 .597366C1.89323 .926623 .926623 1.89323 .597366 3.12324C0 5.35042 0 10 0 10C0 10 0 14.6496 .597366 16.8768C.926623 18.1068 1.89323 19.0734 3.12323 19.4026C5.35042 20 14.285 20 14.285 20C14.285 20 23.2197 20 25.4468 19.4026C26.6768 19.0734 27.6435 18.1068 27.9727 16.8768C28.5701 14.6496 28.5701 10 28.5701 10C28.5701 10 28.5677 5.35042 27.9727 3.12324Z" fill="#f00"/><path d="M11.4253 14.2854L18.8477 10.0004L11.4253 5.71533Z" fill="#fff"/></svg>`;
const MUSIC_LOGO = ytmSvg('pv-glyph', 4);
const VIDEO_LOGO = ytSvg('pv-glyph wide');
const BRAND_SMALL = { music: ytmSvg('brand-logo', 8), video: ytSvg('brand-logo wide') };
// the white app-icon tile the two YouTube entries use instead of a colored one
const brandTile = (brand, cls = 'tile t-brand') => `<span class="${cls}">${BRAND_SMALL[brand]}</span>`;

// ---------- API ----------
class ApiError extends Error {
  constructor(status, code) { super(code || `HTTP ${status}`); this.status = status; this.code = code; }
}
async function api(method, path, body, { timeout = 15000 } = {}) {
  const headers = {};
  if (S.session) headers['X-Rondo-Session'] = S.session;
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeout);
  let res;
  try {
    res = await fetch(path, { method, headers, body: body === undefined ? undefined : JSON.stringify(body), signal: ctl.signal, credentials: 'same-origin', cache: 'no-store' });
  } catch {
    clearTimeout(timer);
    connectionLost();
    throw new ApiError(0, 'network');
  }
  clearTimeout(timer);
  connectionBack();
  let data = null;
  try { data = await res.json(); } catch { /* empty body */ }
  if (res.status === 401) { needPair(); throw new ApiError(401, 'pair'); }
  if (!res.ok) throw new ApiError(res.status, data?.error);
  return data;
}
const reason = (e) => (e?.status === 0 ? 'Your Rondo didn’t answer.' : e?.code && e.code !== 'pair' ? `It said “${e.code}”.` : 'Something went wrong.');

// ---------- screens ----------
const screens = { splash: $('#splash'), pair: $('#pair'), offline: $('#offline'), app: $('#app') };
function showScreen(name) {
  for (const [k, el] of Object.entries(screens)) el.hidden = k !== name;
}

let offlineTimer;
function connectionLost() {
  if (!S.booted) {
    if (!screens.pair.hidden) return;
    showScreen('offline');
    clearTimeout(offlineTimer);
    offlineTimer = setTimeout(() => { if (!document.hidden) boot(); }, 5000);
    return;
  }
  if (S.online) {
    S.online = false;
    $('#bannerText').textContent = S.joining
      ? `Your Rondo is joining “${S.joining.ssid}”. If this device isn’t on that network, open the new address shown under Settings › Dashboard.`
      : 'Lost the connection to your Rondo. Is Settings › Dashboard still open? Trying again…';
    $('#banner').hidden = false;
  }
}
function connectionBack() {
  if (!S.online) {
    S.online = true;
    $('#banner').hidden = true;
  }
}

// ---------- pairing ----------
const codeInput = $('#codeInput'), codeForm = $('#codeForm'), codeMsg = $('#codeMsg');
let wrongCodes = 0;
function needPair() {
  S.session = null;
  store.set('rondo_session', null);
  stopPolling();
  showScreen('pair');
  codeInput.value = '';
  renderCode();
  setTimeout(() => codeInput.focus(), 60);
}
function renderCode() {
  const v = codeInput.value;
  $$('.code-boxes span').forEach((b, i) => {
    b.textContent = v[i] || '';
    b.classList.toggle('is-filled', !!v[i]);
    b.classList.toggle('is-active', i === Math.min(v.length, 5));
  });
}
codeInput.addEventListener('input', () => {
  codeInput.value = codeInput.value.replace(/\D/g, '').slice(0, 6);
  codeForm.classList.remove('is-error');
  if (codeMsg.textContent && !codeForm.classList.contains('is-busy')) codeMsg.textContent = '';
  renderCode();
  if (codeInput.value.length === 6) submitCode(codeInput.value);
});
codeInput.addEventListener('focus', renderCode);
codeForm.addEventListener('submit', (e) => { e.preventDefault(); if (codeInput.value.length === 6) submitCode(codeInput.value); });
async function submitCode(code) {
  if (codeForm.classList.contains('is-busy')) return;
  codeForm.classList.add('is-busy');
  codeInput.readOnly = true;
  $('#pairMark').classList.add('is-turning');
  codeMsg.className = 'code-msg is-quiet';
  codeMsg.textContent = 'Checking…';
  let res, data = null;
  try {
    res = await fetch('/api/pair', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ code }), credentials: 'same-origin' });
    try { data = await res.json(); } catch { /* none */ }
  } catch { res = null; }
  codeForm.classList.remove('is-busy');
  codeInput.readOnly = false;
  $('#pairMark').classList.remove('is-turning');
  codeMsg.className = 'code-msg';
  if (res?.ok && data?.session) {
    S.session = data.session;
    store.set('rondo_session', data.session);
    codeMsg.textContent = '';
    wrongCodes = 0;
    if (S.booted) {
      // a new pairing usually means the dashboard (or Rondo itself) restarted: drop what was
      // loaded before so the theme, library and settings are fetched again
      S.theme = null; S.library = null; S.settings = null; S.wifi = null; S.server = null;
      showScreen('app');
      startPolling();
      enterView(S.route, true);
      pumpUploads();
    } else boot();
    return;
  }
  codeInput.value = '';
  renderCode();
  codeForm.classList.add('is-error');
  if (!res) codeMsg.textContent = 'Can’t reach your Rondo. Is Settings › Dashboard still open?';
  else if (res.status === 429) {
    wrongCodes = 0;
    codeMsg.textContent = `Too many wrong codes, so your Rondo picked a new one. Wait ${data?.retryAfterSec ?? 30} seconds, then type the code it shows now.`;
  } else if (res.status === 403) {
    wrongCodes++;
    codeMsg.textContent = 'That code didn’t match. Check the code on your Rondo and try again.';
  } else codeMsg.textContent = 'Pairing didn’t work. Try again.';
  codeInput.focus();
}

// ---------- boot ----------
async function boot() {
  clearTimeout(offlineTimer);
  try {
    const status = await api('GET', '/api/status');
    applyStatus(status, false);
  } catch (e) {
    if (e.status && e.status !== 401) {
      showScreen('offline');
      $('#offlineNote').textContent = `Your Rondo answered with an error (${e.code || e.status}). Trying again every few seconds.`;
      offlineTimer = setTimeout(() => { if (!document.hidden) boot(); }, 5000);
    }
    return;
  }
  try {
    S.theme = await api('GET', '/api/theme');
    applyAccent(S.theme.accent);
  } catch { /* the Theme section shows the error */ }
  S.booted = true;
  showScreen('app');
  buildNav();
  renderChrome();
  renderMini();
  route();
  startPolling();
  checkForUpdate();
}
$('#offlineRetry').addEventListener('click', () => {
  $('#offlineNote').textContent = 'Trying…';
  boot();
});

// ---------- navigation ----------
// the sidebar in groups, as on the Rondo: what's on it, how it connects, how it behaves
const NAV = [
  { id: 'overview', label: 'Overview', tile: 'rondo', tab: true },
  { id: 'library', label: 'Library', tile: 'device', glyph: 'library', tab: true },
  { id: 'radio', label: 'FM Radio', short: 'Radio', tile: 'radio', glyph: 'radio', tab: true },
  { group: 'Connections' },
  { id: 'wifi', label: 'Wi-Fi', tile: 'wifi', glyph: 'wifi' },
  { id: 'bluetooth', label: 'Bluetooth', tile: 'bluetooth', glyph: 'bluetooth' },
  { id: 'server', label: 'YouTube', tile: 'server', glyph: 'cloud', tab: true },
  { group: 'Settings' },
  { id: 'sound', label: 'Sound', tile: 'sound', glyph: 'sliders' },
  { id: 'theme', label: 'Theme', tile: 'theme', glyph: 'theme' },
  { id: 'settings', label: 'General', tile: 'settings', glyph: 'gear' },
];
// the phone's fifth tab: the pages without a tab of their own, in the sidebar's groups
const MORE = { id: 'more', label: 'Settings', tile: 'settings', glyph: 'gear' };
const PAGES = [...NAV.filter((n) => n.id), MORE];
const TABS = [...NAV.filter((n) => n.tab), MORE];
const inMore = (id) => !TABS.some((t) => t.id === id && t !== MORE);
const tileHtml = (tile, glyph) => `<span class="tile t-${tile}">${glyph ? icon(glyph) : markTile}</span>`;
function buildNav() {
  $('#nav').innerHTML = NAV.map((n) => n.group ? `<div class="nav-group">${n.group}</div>` : `<a href="#${n.id}" data-id="${n.id}">${tileHtml(n.tile, n.glyph)}<span>${n.label}</span><span class="nav-badge"></span>${icon('chevron', 'chev')}</a>`).join('');
  $('#moreRows').innerHTML = NAV.filter((n) => !n.tab).map((n) => n.group ? `</div><h2 class="gh">${n.group}</h2><div class="group">`
    : `<a class="row more-row" href="#${n.id}">${tileHtml(n.tile, n.glyph)}<div class="row-main"><div class="row-title">${n.label}</div></div>${icon('chevron', 'chev')}</a>`).join('').replace(/^<\/div>/, '') + '</div>';
  $('#tabs').innerHTML = TABS.map((n) => `<a class="tab" href="#${n.id}" data-id="${n.id}"><span class="tab-wrap">${tileHtml(n.tile, n.glyph)}<span class="nav-badge"></span></span><span>${n.short || n.label}</span></a>`).join('');
}
function route() {
  const id = location.hash.slice(1);
  const next = PAGES.some((n) => n.id === id) ? id : 'overview';
  const changed = next !== S.route || $(`#view-${next}`).hidden;
  S.route = next;
  for (const n of PAGES) $(`#view-${n.id}`).hidden = n.id !== next;
  $$('#nav a, #tabs a').forEach((a) => (a.dataset.id === next ? a.setAttribute('aria-current', 'page') : a.removeAttribute('aria-current')));
  if (inMore(next)) $('#tabs a[data-id="more"]')?.setAttribute('aria-current', 'page');  // a page of Settings
  document.title = next === 'overview' ? 'Rondo' : `${PAGES.find((n) => n.id === next).label} · Rondo`;
  if (changed) window.scrollTo(0, 0);
  enterView(next);
}
window.addEventListener('hashchange', () => { if (S.booted) route(); });
function enterView(id, force = false) {
  if (id === 'overview') { renderOverview(); if (performance.now() - S.statusAt > 1800) poll(); }
  if (id === 'library') { if (!S.library || force) loadLibrary(); else renderLibrarySummary(); }
  if (id === 'theme') { if (!S.theme || force) loadTheme(); else renderTheme(); }
  if (id === 'settings') { loadSettings(); loadDateTime(); }
  if (id === 'sound') loadSound();
  if (id === 'wifi') loadWifi();
  if (id === 'bluetooth') loadBluetooth();
  if (id === 'radio') loadRadio();
  if (id === 'server') { loadServer(); loadYtAccount(); loadDownloads(); if (!S.settings) loadSettings(); }
}

// ---------- status polling ----------
let pollTimer = 0, tickTimer = 0;
const pollEvery = () => (!S.online ? 4000 : S.route === 'overview' ? 2000 : 10000);
function startPolling() { schedulePoll(); startTick(); }
function stopPolling() { clearTimeout(pollTimer); clearInterval(tickTimer); tickTimer = 0; }
function schedulePoll(ms = pollEvery()) {
  clearTimeout(pollTimer);
  if (document.hidden || !S.booted) return;
  pollTimer = setTimeout(poll, ms);
}
let polling = false;
async function poll() {
  if (polling) return;
  polling = true;
  try { applyStatus(await api('GET', '/api/status')); } catch { /* banner or pair screen */ }
  polling = false;
  if (S.booted && screens.pair.hidden) schedulePoll();
}
document.addEventListener('visibilitychange', () => {
  if (document.hidden) { clearTimeout(pollTimer); clearInterval(tickTimer); tickTimer = 0; return; }
  if (S.booted && screens.pair.hidden) { poll(); startTick(); }
  else if (!S.booted && !screens.offline.hidden) boot();
});
function startTick() {
  clearInterval(tickTimer);
  tickTimer = setInterval(() => { if (S.status?.player?.playing) renderProgress(); }, 500);
}

function applyStatus(st, render = true) {
  const prevTitle = S.status?.player?.title, prevPlaying = S.status?.player?.playing;
  S.status = st;
  S.statusAt = performance.now();
  if (!render) return;
  renderChrome();
  renderMini();
  if (S.route === 'overview') renderOverview();
  const p = st.player;
  if (p?.title !== prevTitle) markPlayingRows();
  if ((p?.title !== prevTitle || p?.playing !== prevPlaying) && S.route === 'theme') renderPreview();
}
function position() {
  const p = S.status?.player;
  if (!p) return 0;
  const extra = p.playing ? performance.now() - S.statusAt : 0;
  return Math.min(p.positionMs + extra, p.durationMs || Infinity);
}

// ---------- header, sidebar ----------
function renderChrome() {
  const st = S.status;
  if (!st) return;
  const b = st.battery || {};
  const batt = `<span class="stat" title="Battery${b.charging ? ', charging' : ''}">${battSvg(b.level ?? 0, 'i', b.charging)}<span>${b.level ?? '–'}%</span></span>`;
  const wifi = st.wifi
    ? `<span class="stat" title="Wi-Fi: ${esc(st.wifi.ssid)}">${sigSvg(st.wifi.signal ?? 0, 'i')}<span class="side-only">${esc(st.wifi.ssid)}</span></span>`
    : '';
  $('#topStatus').innerHTML = wifi.replace(/<span class="side-only">.*?<\/span>/, '') + batt;
  $('#sideStatus').innerHTML = `${wifi}${batt}`;
}
function renderMini() {
  const p = S.status?.player, mini = $('#mini');
  if (!p) { mini.hidden = true; return; }
  mini.hidden = false;
  const key = `${p.title}|${p.art}`;
  if (mini.dataset.key !== key) {
    mini.dataset.key = key;
    mini.innerHTML = `<span class="art">${icon('note')}${p.art ? `<img alt="" src="${esc(p.art)}" onload="this.classList.add('ok')" onerror="artRetry(this)">` : ''}</span>
      <a class="s-main" href="#overview" style="color:inherit;text-decoration:none"><div class="s-title">${esc(p.title)}</div><div class="s-sub">${esc(p.subtitle || '')}</div></a>
      <button class="ib ib-sm" type="button" data-act="toggle"></button>`;
  }
  const btn = $('button', mini);
  btn.innerHTML = icon(p.playing ? 'pause' : 'play');
  btn.setAttribute('aria-label', p.playing ? 'Pause' : 'Play');
}
$('#mini').addEventListener('click', (e) => { if (e.target.closest('[data-act="toggle"]')) playerAction('toggle'); });

// ---------- overview ----------
const np = $('#np'), npSeek = $('#npSeek'), npVol = $('#npVol');
let seeking = false, volDragging = false, volTimer = 0, volHold = 0;
function setFill(input) {
  const min = +input.min || 0, max = +input.max || 100;
  input.style.setProperty('--p', `${max > min ? ((input.value - min) / (max - min)) * 100 : 0}%`);
}
function renderOverview() {
  renderPlayer();
  renderDevice();
}
function renderPlayer() {
  const p = S.status?.player;
  np.classList.toggle('is-empty', !p);
  if (!p) {
    $('#npSrc').innerHTML = '';
    $('#npTitle').textContent = 'Nothing playing';
    $('#npSub').textContent = 'Choose a song in Library and play it on your Rondo, or start something on the Rondo itself.';
    setNpArt(null);
    return;
  }
  $('#npSrc').innerHTML = p.source === 'youtube' ? `${icon('play')}<span>YouTube</span>` : `${icon('folder')}<span>Library</span>`;
  $('#npTitle').textContent = p.title || 'Untitled';
  $('#npSub').textContent = p.subtitle || '';
  const t = $('#npToggle');
  t.innerHTML = icon(p.playing ? 'pause' : 'play');
  t.setAttribute('aria-label', p.playing ? 'Pause' : 'Play');
  t.classList.toggle('is-paused', !p.playing);
  if (!volDragging && Date.now() > volHold) {
    npVol.max = p.maxVolume || 15;
    npVol.value = p.volume ?? 0;
    $('#npVolOut').textContent = p.volume ?? 0;
    setFill(npVol);
  }
  setNpArt(p.art);
  renderProgress();
}
function renderProgress() {
  const p = S.status?.player;
  if (!p) return;
  const pos = position(), dur = p.durationMs || 0;
  if (!seeking && S.route === 'overview') {
    npSeek.value = dur ? Math.round((pos / dur) * 1000) : 0;
    setFill(npSeek);
    $('#npPos').textContent = fmtTime(pos);
    $('#npRem').textContent = dur ? `−${fmtTime(remainMs(dur, pos))}` : '';
    npSeek.setAttribute('aria-valuetext', `${fmtTime(pos)} of ${fmtTime(dur)}`);
  }
  const bar = $('.pv-prog span');
  if (bar) bar.style.setProperty('--p', `${dur ? (pos / dur) * 100 : 0}%`);
}
let npArt = undefined;
function setNpArt(url) {
  if (url === npArt) return;
  npArt = url;
  let img = $('#npImg');
  const reset = () => {
    img?.remove();
    np.style.setProperty('--np1', '#3a3f4a');
    np.style.setProperty('--np2', darker('#3a3f4a', 0.45));
  };
  if (!url) return reset();
  const probe = new Image();
  probe.onload = () => {
    if (npArt !== url) return;
    if (!img) { img = new Image(); img.id = 'npImg'; img.alt = ''; $('.np-art', np).append(img); }
    img.src = probe.src;  // the address that answered (a retry carries &r=)
    const c = artColor(probe) || '#3a3f4a';
    np.style.setProperty('--np1', c);
    np.style.setProperty('--np2', darker(c, 0.45));
  };
  let tries = 0;
  probe.onerror = () => {
    if (npArt !== url) return;
    if (++tries > ART_RETRY_MS.length) return reset();
    setTimeout(() => { if (npArt === url) probe.src = withTry(url, tries); }, ART_RETRY_MS[tries - 1]);
  };
  probe.src = url;
}
np.addEventListener('click', (e) => {
  const b = e.target.closest('[data-act]');
  if (b) playerAction(b.dataset.act);
});
async function playerAction(action, extra = {}) {
  const p = S.status?.player;
  if (!p) return;
  if (action === 'toggle') { // answer the tap at once
    p.positionMs = position(); S.statusAt = performance.now(); p.playing = !p.playing;
    renderPlayer(); renderMini();
  }
  try {
    const res = await api('POST', '/api/player', { action, ...extra });
    if (S.status) { S.status.player = res; S.statusAt = performance.now(); }
    renderPlayer(); renderMini(); markPlayingRows();
    if (S.route === 'theme') renderPreview();
  } catch (e) {
    if (e.status !== 401) toast(`Couldn’t control playback. ${reason(e)}`, 'err');
    poll();
  }
}
npSeek.addEventListener('input', () => {
  seeking = true;
  setFill(npSeek);
  const dur = S.status?.player?.durationMs || 0;
  const pos = (npSeek.value / 1000) * dur;
  $('#npPos').textContent = fmtTime(pos);
  $('#npRem').textContent = `−${fmtTime(remainMs(dur, pos))}`;
});
npSeek.addEventListener('change', async () => {
  const p = S.status?.player;
  if (!p) { seeking = false; return; }
  const pos = Math.round((npSeek.value / 1000) * p.durationMs);
  p.positionMs = pos; S.statusAt = performance.now();
  await playerAction('seek', { positionMs: pos });
  seeking = false;
});
npVol.addEventListener('pointerdown', () => { volDragging = true; });
npVol.addEventListener('input', () => {
  setFill(npVol);
  $('#npVolOut').textContent = npVol.value;
  clearTimeout(volTimer);
  volTimer = setTimeout(() => sendVolume(+npVol.value), 160);
});
npVol.addEventListener('change', () => { volDragging = false; });
async function sendVolume(v) {
  volHold = Date.now() + 1500;
  try {
    const res = await api('POST', '/api/player', { action: 'volume', volume: v });
    if (S.status) S.status.player = res;
  } catch (e) { if (e.status !== 401) toast(`Couldn’t change the volume. ${reason(e)}`, 'err'); }
}

function renderDevice() {
  const st = S.status;
  if (!st) return;
  const b = st.battery || {}, s = st.storage || {}, w = st.wifi, d = st.device || {}, ab = st.about || {};
  const level = b.level ?? 0;
  const used = (s.totalBytes || 0) - (s.freeBytes || 0);
  const pct = s.totalBytes ? (used / s.totalBytes) * 100 : 0;
  const freePct = 100 - pct;
  $('#deviceRows').innerHTML = `
    <div class="row">
      <span class="tile ${level <= 15 && !b.charging ? 't-low' : 't-battery'}">${b.charging ? icon('bolt') : battSvg(level, 'i', false, false)}</span>
      <div class="row-main"><div class="row-title">Battery</div>${b.charging ? '<div class="row-sub">Charging</div>' : level <= 15 ? '<div class="row-sub">Low. Charge your Rondo soon.</div>' : ''}</div>
      <div class="row-value strong">${level}%</div>
    </div>
    <div class="row">
      <span class="tile t-storage">${icon('folder')}</span>
      <div class="row-main">
        <div class="row-title">Storage</div>
        <div class="meter ${freePct < 3 ? 'full' : freePct < 10 ? 'low' : ''}" role="img" aria-label="${Math.round(pct)}% used"><span style="--p:${pct.toFixed(1)}%"></span></div>
        <div class="row-sub">${fmtBytes(used)} used of ${fmtBytes(s.totalBytes)}</div>
      </div>
      <div class="row-value strong">${fmtBytes(s.freeBytes)} free</div>
    </div>
    <div class="row">
      <span class="tile t-wifi">${icon('wifi')}</span>
      <div class="row-main"><div class="row-title">${w ? esc(w.ssid) : 'Wi-Fi'}</div><div class="row-sub">${w ? `Wi-Fi · ${esc(w.ip || '')}` : 'Not connected'}${ab.mac ? ` · MAC ${esc(ab.mac)}` : ''}</div></div>
      ${w ? `<span class="row-value" aria-label="Signal ${w.signal} of 4">${sigSvg(w.signal ?? 0)}</span>` : ''}
    </div>
    <div class="row">
      <span class="tile t-server">${icon('cloud')}</span>
      <div class="row-main"><div class="row-title">YouTube</div><div class="row-sub">${esc(ab.connectTo || '')}${ab.timeZone ? ` · Time zone ${esc(ab.timeZone)}` : ''}</div></div>
    </div>
    <div class="row">
      <span class="tile t-rondo">${markTile}</span>
      <div class="row-main"><div class="row-title">${esc(d.name || 'Rondo')} ${esc(d.version || '')}</div><div class="row-sub">${esc(d.model || 'Innioasis Y1')}${d.canUpdate ? ` · <button class="link" type="button" data-update="file">Update from a file</button>` : ''}</div></div>
      ${d.power ? `<div class="bt-actions"><button class="btn btn-sm" type="button" data-power="restart">Restart</button><button class="btn btn-sm btn-red-text" type="button" data-power="shutdown">${icon('power')}<span>Shut down</span></button></div>` : ''}
    </div>${newest && isNewer(newest.version, d.version) ? `
    <div class="row update-row">
      <span class="tile t-rondo">${icon('upload')}</span>
      <div class="row-main"><div class="row-title">Rondo ${esc(newest.version)} is out</div><div class="row-sub">${needsFirmware(d) ? `It needs a new firmware: install it with <a href="${UPDATER_GUIDE}" target="_blank" rel="noopener">Innioasis Updater CE</a>.` : `You have ${esc(d.version || '')}.`} <a href="${esc(newest.url)}" target="_blank" rel="noopener">What’s new</a></div></div>
      ${d.canUpdate && !needsFirmware(d) ? `<button class="btn btn-sm btn-primary" type="button" data-update="install">Install</button>` : ''}
    </div>` : ''}`;
}
// ---------- installing an update (SoftwareUpdate.kt on the Rondo) ----------
// The browser downloads rondo.apk from the update page's site (GitHub Pages lets any page fetch its
// files; GitHub release downloads don't) and sends it to the Rondo, which installs it over itself
// and restarts with everything kept. The dashboard stops with it. A release that needs a newer
// firmware than the Rondo's installs only with Innioasis Updater CE (the Rondo refuses it too).
const UPDATE_SITE = 'https://jarvissax.github.io/rondo/';
const UPDATER_GUIDE = 'https://github.com/jarvissax/rondo/blob/main/docs/install.md#updating-rondo';
const needsFirmware = (d) => !!newest && newest.firmware > (d.firmware ?? 1);
$('#deviceRows').addEventListener('click', (e) => {
  const b = e.target.closest('[data-update]');
  if (!b) return;
  if (b.dataset.update === 'file') return $('#updateFile').click();
  installUpdate(b);
});
$('#updateFile').addEventListener('change', (e) => {
  const file = e.target.files?.[0];
  e.target.value = '';
  if (file) sendUpdate(file, null).catch((err) => toast(`Couldn’t update. ${err.message}`, 'err'));
});
async function installUpdate(btn) {
  const label = btn.textContent;
  btn.disabled = true;
  try {
    btn.textContent = 'Downloading…';
    const info = await (await fetch(UPDATE_SITE + 'latest.json', { cache: 'no-store' })).json();
    if (newest && info.version !== newest.version) throw new Error(`The download page still has Rondo ${info.version}. Try again in a few minutes.`);
    if ((Number(info.firmware) || 1) > (S.status?.device?.firmware ?? 1)) throw new Error(`Rondo ${info.version} needs a new firmware: install it with Innioasis Updater CE.`);
    const res = await fetch(UPDATE_SITE + (info.file || 'rondo.apk'), { cache: 'no-store' });
    if (!res.ok) throw new Error(`The download failed (${res.status}).`);
    await sendUpdate(await res.blob(), btn);
  } catch (e) {
    btn.disabled = false;
    btn.textContent = label;
    toast(`Couldn’t update. ${e.message || ''}`, 'err');
  }
}
function sendUpdate(blob, btn) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('PUT', '/api/update');
    if (S.session) xhr.setRequestHeader('X-Rondo-Session', S.session);
    // an APK for 1.0.x, a .rondo bundle for Rondo 2.0
    xhr.setRequestHeader('Content-Type', /\.rondo$/.test(blob.name || '') || (S.status?.device?.firmware ?? 1) >= 2 ? 'application/octet-stream' : 'application/vnd.android.package-archive');
    xhr.upload.onprogress = (e) => { if (btn && e.lengthComputable) btn.textContent = `Sending ${Math.round((e.loaded / e.total) * 100)}%`; };
    xhr.onload = () => {
      let data = null;
      try { data = JSON.parse(xhr.responseText); } catch { /* empty */ }
      if (xhr.status !== 200) return reject(new Error(data?.error || `Your Rondo answered ${xhr.status}.`));
      if (btn) btn.textContent = 'Installing…';
      openSheet(`<h2>Installing Rondo ${esc(data?.version || '')}</h2><p>Your Rondo installs it and restarts in about 15 seconds, with its settings, Wi-Fi, playlists and music as they were. This page loses it then: open <b>Settings › Dashboard</b> on the Y1 to come back.</p><div class="sheet-actions"><button class="btn btn-primary" type="button" data-r="ok">OK</button></div>`);
      $('[data-r="ok"]', sheet).addEventListener('click', () => sheet.close(), { once: true });
      resolve();
    };
    xhr.onerror = () => reject(new Error('Your Rondo didn’t answer.'));
    xhr.send(blob);
  });
}
// ---------- a newer Rondo ----------
// Asked by this browser, not by the Rondo (which talks only to its music server), of the update
// page's site: latest.json names the newest release and the firmware it needs, once its rom.zip is
// out (.github/workflows/release.yml), the same answer the Rondo and the update page get. Nothing
// shows when the site can't be reached from here.
let newest = null; // { version, firmware, url }
async function checkForUpdate() {
  try {
    const r = await fetch(UPDATE_SITE + 'latest.json', { cache: 'no-store' });
    if (!r.ok) return;
    const j = await r.json();
    if (!j.version) return;
    newest = { version: String(j.version), firmware: Number(j.firmware) || 1, url: j.notes || 'https://github.com/jarvissax/rondo/releases' };
    renderDevice();
  } catch { /* offline */ }
}
// "1.0.10" is newer than "1.0.9" (the server's updates.ts compares the same way)
function isNewer(latest, current) {
  if (!latest || !current) return false;
  const a = latest.split(/[.-]/), b = current.split(/[.-]/);
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const x = Number(a[i] ?? 0), y = Number(b[i] ?? 0);
    if (Number.isNaN(x) || Number.isNaN(y)) return latest !== current;
    if (x !== y) return x > y;
  }
  return false;
}

// shut down or restart (firmware builds only: status.device.power)
$('#deviceRows').addEventListener('click', async (e) => {
  const b = e.target.closest('[data-power]');
  if (!b) return;
  const off = b.dataset.power === 'shutdown';
  const ok = await confirmSheet({
    title: off ? 'Shut down your Rondo?' : 'Restart your Rondo?',
    text: off ? 'Music stops and this page disconnects. Press the Power button on the Y1 to turn it on again.' : 'Music stops and this page disconnects. Open Settings › Dashboard on the Y1 again once it’s back.',
    action: off ? 'Shut down' : 'Restart',
  });
  if (!ok) return;
  try {
    await api('POST', '/api/power', { action: b.dataset.power });
    toast(off ? 'Your Rondo is shutting down' : 'Your Rondo is restarting');
  } catch (e2) { if (e2.status !== 401) toast(`Couldn’t do that. ${reason(e2)}`, 'err'); }
});

// ---------- library ----------
// the music Rondo plays (core/library.cpp kAudio): every common format, lossy and lossless
const AUDIO = ['flac', 'mp3', 'm4a', 'm4b', 'aac', 'ogg', 'oga', 'opus', 'wav', 'w64', 'aif', 'aiff', 'aifc', 'wma',
  'ape', 'wv', 'ac3', 'eac3', 'ec3', 'dts', 'mka', 'dsf', 'dff', 'tta', 'tak', 'mpc', 'amr', 'awb', 'mp2', 'caf', 'au'];
const VIDEO = ['mp4', 'm4v', 'mov', 'mkv', '3gp', 'webm'];
const PHOTOS = ['jpg', 'jpeg', 'png', 'webp', 'gif', 'bmp'];
const ALLOWED = [...AUDIO, ...VIDEO, ...PHOTOS, 'lrc'];
// what can go anywhere on the card from the Files tab: pictures, subtitles, playlists too
const ALLOWED_FILES = [...ALLOWED, 'srt', 'ass', 'vtt', 'jpg', 'jpeg', 'png', 'gif', 'webp', 'bmp', 'm3u8', 'm3u'];
const extOf = (n) => (n.split('.').pop() || '').toLowerCase();
const libList = $('#libList'), libEl = $('#lib');

async function loadLibrary(quiet = false) {
  if (!quiet && !S.library) {
    libList.innerHTML = '<div class="lib-skel"></div>'.repeat(6);
    $('#libSummary').textContent = 'Reading the card…';
  }
  try {
    S.library = await api('GET', '/api/library', undefined, { timeout: 45000 });
  } catch (e) {
    if (e.status === 401) return;
    if (!S.library) {
      libList.innerHTML = `<div class="lib-empty"><h3>Couldn’t read the library</h3><p>${esc(reason(e))}</p><button class="btn" type="button" data-act="retry">Try again</button></div>`;
      $('#libSummary').textContent = '';
    }
    return;
  }
  prepLibrary();
  renderLibrary();
  if (S.library.scanning) watchScan();
}
function prepLibrary() {
  S.libByPath = new Map();
  for (const s of S.library.songs) { s._key = fold(`${s.title} ${s.artist} ${s.album} ${s.albumArtist || ''} ${s.genre || ''} ${s.year || ''} ${fileName(s.path)}`); S.libByPath.set(s.path, s); }
  for (const v of S.library.videos) { v._video = true; v._key = fold(`${v.title} ${fileName(v.path)}`); S.libByPath.set(v.path, v); }
}
function renderLibrarySummary() {
  const L = S.library;
  if (!L) return;
  const el = $('#libSummary');
  if (L.scanning) { el.innerHTML = `<span class="scan-note">${spinner()}Scanning the card…</span>`; return; }
  if (S.libKind === 'playlists') return;
  const list = S.libKind === 'songs' ? L.songs : L.videos;
  const bytes = list.reduce((n, x) => n + (x.size || 0), 0);
  el.textContent = list.length
    ? `${plural(list.length, S.libKind === 'songs' ? 'song' : 'video')} · ${fmtBytes(bytes)}. They play without the internet.`
    : 'Music and videos on the card. They play without the internet.';
  $('#rescanBtn').disabled = false;
}
// sorts that only songs have (tags, listening)
const SONG_SORTS = ['artist', 'album', 'albumArtist', 'genre', 'year', 'plays', 'played', 'rating'];
const sortKey = () => (S.libKind === 'videos' && SONG_SORTS.includes(S.libSort) ? 'title' : S.libSort);
const stars = (n) => '★'.repeat(n) + '☆'.repeat(5 - n);
function dateGroup(ms) {
  if (!ms) return 'Earlier';
  const now = new Date(), d = new Date(ms);
  const day = 86400000, start = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
  if (ms >= start) return 'Today';
  if (ms >= start - day) return 'Yesterday';
  if (ms >= start - 6 * day) return 'This week';
  if (ms >= start - 29 * day) return 'This month';
  return d.toLocaleDateString('en', { month: 'long', year: 'numeric' });
}
function buildRows() {
  const L = S.library, kind = S.libKind, sort = sortKey();
  const src = kind === 'songs' ? L.songs : L.videos;
  const q = fold(S.libQuery.trim());
  let items = q ? src.filter((x) => q.split(/\s+/).every((w) => x._key.includes(w))) : src.slice();
  // the listening sorts show only what has been listened to (rated)
  if (sort === 'plays' || sort === 'played') items = items.filter((x) => x.plays > 0);
  if (sort === 'rating') items = items.filter((x) => x.rating > 0);
  const byTitle = (a, b) => collator.compare(a.title || fileName(a.path), b.title || fileName(b.path));
  const emptyLast = (a, b) => (!a) - (!b) || collator.compare(a, b);
  const cmp = {
    title: byTitle,
    artist: (a, b) => emptyLast(a.artist, b.artist) || emptyLast(a.album, b.album) || (a.track || 0) - (b.track || 0) || byTitle(a, b),
    album: (a, b) => emptyLast(a.album, b.album) || (a.disc || 0) - (b.disc || 0) || (a.track || 0) - (b.track || 0) || byTitle(a, b),
    albumArtist: (a, b) => emptyLast(a.albumArtist || a.artist, b.albumArtist || b.artist) || emptyLast(a.album, b.album) || (a.disc || 0) - (b.disc || 0) || (a.track || 0) - (b.track || 0) || byTitle(a, b),
    genre: (a, b) => emptyLast(a.genre, b.genre) || emptyLast(a.artist, b.artist) || byTitle(a, b),
    year: (a, b) => (b.year || 0) - (a.year || 0) || emptyLast(a.album, b.album) || (a.track || 0) - (b.track || 0) || byTitle(a, b),
    added: (a, b) => (b.modified || 0) - (a.modified || 0) || byTitle(a, b),
    plays: (a, b) => b.plays - a.plays || b.lastPlayed - a.lastPlayed || byTitle(a, b),
    played: (a, b) => b.lastPlayed - a.lastPlayed || byTitle(a, b),
    rating: (a, b) => b.rating - a.rating || b.plays - a.plays || byTitle(a, b),
  }[sort];
  items.sort(cmp);
  const rows = [];
  let last = null;
  for (const it of items) {
    let label = null, sub = '';
    if (sort === 'added') label = dateGroup(it.modified);
    else if (kind === 'songs' && sort === 'title') { const c = fold(it.title || fileName(it.path)).charAt(0).toUpperCase(); label = /[A-Z]/.test(c) ? c : '#'; }
    else if (sort === 'artist') label = it.artist || 'Unknown artist';
    else if (sort === 'album') { label = it.album || 'Unknown album'; sub = it.album ? [it.albumArtist || it.artist, it.year || ''].filter(Boolean).join(' · ') : ''; }
    else if (sort === 'albumArtist') label = it.albumArtist || it.artist || 'Unknown artist';
    else if (sort === 'genre') label = it.genre || 'No genre';
    else if (sort === 'year') label = it.year ? String(it.year) : 'No year';
    else if (sort === 'played') label = dateGroup(it.lastPlayed);
    else if (sort === 'rating') label = stars(it.rating);
    if (label !== null && label !== last) { rows.push({ h: label, sub }); last = label; }
    rows.push(it);
  }
  return { rows, count: items.length };
}
function renderLibrary() {
  if (!S.library) return;
  const videos = S.libKind === 'videos';
  libEl.classList.toggle('is-videos', videos);
  showKinds();
  const pls = S.libKind === 'playlists', files = S.libKind === 'files', albumsTab = S.libKind === 'albums', photosTab = S.libKind === 'photos';
  libEl.classList.toggle('is-playlists', pls || files || albumsTab || photosTab);
  if (photosTab) {
    $('#libSortWrap').hidden = true;
    $('#libSearch').placeholder = 'Search photos';
    libList.setAttribute('aria-label', 'Photos');
    $('#view-library').classList.remove('is-empty');
    return renderPhotos();
  }
  if (albumsTab) {
    $('#libSortWrap').hidden = true;
    $('#libSearch').placeholder = 'Search albums';
    libList.setAttribute('aria-label', 'Albums');
    $('#view-library').classList.remove('is-empty');
    return renderAlbums();
  }
  if (files) {
    $('#libSortWrap').hidden = true;
    $('#libSearch').placeholder = 'Search this folder';
    libList.setAttribute('aria-label', 'Files');
    $('#view-library').classList.remove('is-empty');
    return renderFiles();
  }
  $('#libSortWrap').hidden = pls;
  if (pls) {
    $('#libSearch').placeholder = 'Search playlists';
    libList.setAttribute('aria-label', 'Playlists');
    $('#view-library').classList.remove('is-empty');
    return renderPlaylists();
  }
  const sel = $('#libSort');
  for (const o of sel.options) o.hidden = videos && SONG_SORTS.includes(o.value);
  sel.value = sortKey();
  $$('#libHead [data-sort]').forEach((b) => (b.dataset.sort === sortKey() ? b.setAttribute('aria-sort', 'ascending') : b.removeAttribute('aria-sort')));
  libList.setAttribute('aria-label', videos ? 'Videos' : 'Songs');
  $('#libSearch').placeholder = videos ? 'Search videos' : matchMedia('(min-width: 900px)').matches ? 'Search songs, artists, albums' : 'Search songs';
  renderLibrarySummary();

  const { rows, count } = buildRows();
  S.libRows = rows;
  S.libShown = 0;
  artQueue.length = 0;
  libList.innerHTML = '';
  window.scrollTo({ top: Math.min(window.scrollY, libEl.offsetTop - 80) });
  const total = (videos ? S.library.videos : S.library.songs).length;
  $('#view-library').classList.toggle('is-empty', !total);
  // the listening sorts with nothing listened to (or rated) yet
  const LISTENING = { plays: ['Nothing played yet', 'Songs show up here once you’ve played them through on your Rondo.'],
    played: ['Nothing played yet', 'Songs show up here once you’ve played them through on your Rondo.'],
    rating: ['Nothing rated yet', 'Rate a song from its details here, or on your Rondo in Now Playing (hold the center button).'] };
  if (!count && total && !S.libQuery.trim() && LISTENING[sortKey()] && !videos) {
    const [h, p] = LISTENING[sortKey()];
    libList.innerHTML = `<div class="lib-empty"><h3>${h}</h3><p>${p}</p></div>`;
    return;
  }
  if (!count) {
    libList.innerHTML = S.libQuery.trim() && total
      ? `<div class="lib-empty"><h3>No matches</h3><p>Nothing ${videos ? 'among the videos' : 'in the songs'} matches “${esc(S.libQuery.trim())}”.</p></div>`
      : `<div class="lib-empty"><h3>${videos ? 'No videos on the card' : 'No music on the card yet'}</h3><p>${videos ? 'Upload MP4, MKV or WebM files and they’ll show up here.' : 'Drop music files on this page or use Upload. FLAC, MP3, M4A and more.'}</p><button class="btn btn-primary" type="button" data-act="upload">${icon('upload')}<span>Upload files</span></button></div>`;
    return;
  }
  appendRows();
}
const CHUNK = 120;
function appendRows() {
  if (S.libKind !== 'songs' && S.libKind !== 'videos') return;  // the songs' rows stay out of the other tabs
  const end = Math.min(S.libShown + CHUNK, S.libRows.length);
  let html = '';
  for (let i = S.libShown; i < end; i++) html += rowHtml(S.libRows[i]);
  libList.insertAdjacentHTML('beforeend', html);
  S.libShown = end;
  $$('.art[data-art]:not([data-q])', libList).forEach((el) => { el.dataset.q = '1'; artIO.observe(el); });
  requestAnimationFrame(() => { if (S.libShown < S.libRows.length && $('#libSentinel').getBoundingClientRect().top < innerHeight + 900) appendRows(); });
}
new IntersectionObserver((entries) => {
  if (entries[0].isIntersecting && S.libShown < S.libRows.length) appendRows();
}, { rootMargin: '900px 0px' }).observe($('#libSentinel'));

function isPlaying(it) {
  const p = S.status?.player;
  if (!p || p.source !== 'device' || !it) return false;
  if (p.path) return p.path === it.path;
  return p.title === (it.title || fileName(it.path)) && (!it.artist || (p.subtitle || '').startsWith(it.artist));
}
function markPlayingRows() {
  if (!S.library) return;
  $$('.song', libList).forEach((r) => r.classList.toggle('is-playing', isPlaying(S.libByPath.get(r.dataset.path))));
}
function rowHtml(r) {
  if (r.h !== undefined) return `<div class="lhdr" role="heading" aria-level="3">${esc(r.h)}${r.sub ? `<small>${esc(r.sub)}</small>` : ''}</div>`;
  const title = r.title || fileName(r.path);
  // videos play from the device itself: opening its video player would close Settings › Dashboard
  const play = r._video ? '' : `<button class="ib ib-sm" type="button" data-act="play" aria-label="Play ${esc(title)} on your Rondo">${icon('play')}</button>`;
  const acts = `<span class="s-acts">${play}<button class="ib ib-sm" type="button" data-act="more" aria-label="Details for ${esc(title)}">${icon('more')}</button></span>`;
  if (r._video) {
    return `<div class="song is-video${isPlaying(r) ? ' is-playing' : ''}" role="listitem" data-path="${esc(r.path)}">
      <span class="art" data-art="${esc(r.path)}">${icon('film')}</span>
      <span class="s-main"><span class="s-title">${esc(title)}</span><span class="s-sub">${fmtBytes(r.size)}</span></span>
      <span class="s-size">${fmtBytes(r.size)}</span>
      <span class="s-time">${fmtDur(r.durationSec)}</span>${acts}</div>`;
  }
  const sub = [r.artist, r.album].filter(Boolean).join(' · ') || fileName(r.path);
  return `<div class="song${isPlaying(r) ? ' is-playing' : ''}" role="listitem" data-path="${esc(r.path)}">
    <span class="art"${r.art ? ` data-art="${esc(r.path)}"` : ''}>${icon('note')}</span>
    <span class="s-main"><span class="s-title">${esc(title)}</span><span class="s-sub">${esc(sub)}</span></span>
    <span class="s-artist">${esc(r.artist)}</span><span class="s-album">${esc(r.album)}</span>
    <span class="s-time">${fmtDur(r.durationSec)}</span>${acts}</div>`;
}

// album art: only rows near the screen, at most 4 requests at a time. A cover that failed (the Y1 busy,
// Wi-Fi dropping a request, a video's frame still being taken) is asked again after 1.5, 4 and 10 s.
const ART_RETRY_MS = [1500, 4000, 10000];
const artUrl = (path, size, tries = 0) => `/api/art?path=${encodeURIComponent(path)}&size=${size}${tries ? `&r=${tries}` : ''}`;
const withTry = (url, tries) => url + (url.includes('?') ? '&' : '?') + 'r=' + tries;
// an <img> of the player's artwork that failed: the same address again, then nothing
window.artRetry = (img) => {
  const tries = (img._tries || 0) + 1;
  if (tries > ART_RETRY_MS.length) return img.remove();
  img._tries = tries;
  img._src = img._src || img.getAttribute('src');
  setTimeout(() => { if (img.isConnected) img.src = withTry(img._src, tries); }, ART_RETRY_MS[tries - 1]);
};
const artQueue = [];
let artActive = 0;
const artIO = new IntersectionObserver((entries) => {
  for (const e of entries) {
    e.target._vis = e.isIntersecting;
    if (e.isIntersecting && !e.target._queued) { e.target._queued = true; artQueue.push(e.target); }
  }
  pumpArt();
}, { rootMargin: '300px 0px' });
function pumpArt() {
  while (artActive < 4) {
    const i = artQueue.findIndex((el) => el._vis && el.isConnected);
    if (i < 0) break;
    const el = artQueue.splice(i, 1)[0];
    artIO.unobserve(el);
    loadArtInto(el, el.classList.contains('is-video') || el.closest('.is-video') ? 160 : 96, true);
  }
}
function loadArtInto(el, size, counted = false) {
  if (counted) artActive++;
  const img = new Image();
  img.alt = '';
  img.decoding = 'async';
  let settled = false;
  const done = () => { if (settled) return; settled = true; if (counted) { artActive--; pumpArt(); } };
  img.onload = () => {
    done();
    if (!el.isConnected || $('img', el)) return;
    el.append(img);
    requestAnimationFrame(() => img.classList.add('ok'));
  };
  img.onerror = () => {
    done();
    const tries = (el._artTries || 0) + 1;
    if (tries > ART_RETRY_MS.length) return;
    el._artTries = tries;
    setTimeout(() => {
      if (!el.isConnected || $('img', el)) return;
      if (counted) { el._queued = false; artIO.observe(el); } // back in line when it is near the screen
      else loadArtInto(el, size);
    }, ART_RETRY_MS[tries - 1]);
  };
  img.src = artUrl(el.dataset.art, size, el._artTries || 0);
}

libList.addEventListener('click', (e) => {
  if (S.libKind === 'playlists') return onPlaylistClick(e);
  if (e.target.closest('[data-act="retry"]')) return loadLibrary();
  if (e.target.closest('[data-act="upload"]')) return $('#fileInput').click();
  const row = e.target.closest('.song');
  if (!row) return;
  const it = S.libByPath.get(row.dataset.path);
  if (!it) return;
  if (e.target.closest('[data-act="play"]')) playItem(it);
  else openItem(it);
});
const MUSIC_KINDS = ['songs', 'albums', 'playlists'];
const sectionOf = (kind) => (MUSIC_KINDS.includes(kind) ? 'music' : kind);
function showKinds() {
  const sec = sectionOf(S.libKind);
  $$('#libSection button').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.section === sec)));
  $$('#libKind button').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.kind === S.libKind)));
  $('#libKind').hidden = sec !== 'music';
  $('#shuffleBtn').hidden = S.libKind !== 'songs';
}
$('#libSection').addEventListener('click', (e) => {
  const b = e.target.closest('[data-section]');
  if (!b) return;
  const kind = b.dataset.section === 'music' ? (S.musicKind || 'songs') : b.dataset.section;
  if (kind !== S.libKind) setKind(kind);
});
$('#libKind').addEventListener('click', (e) => {
  const b = e.target.closest('[data-kind]');
  if (!b) return;
  S.musicKind = b.dataset.kind;
  if (b.dataset.kind !== S.libKind) setKind(b.dataset.kind);
});
// every song shown (the search and sort as they are), in a random order, on the Rondo
$('#shuffleBtn').addEventListener('click', () => shufflePaths(S.libRows.map((s) => s.path), 'the songs'));
async function shufflePaths(paths, what) {
  const q = paths.filter((p) => S.libByPath.has(p));
  if (!q.length) return toast('No songs to shuffle', 'err');
  for (let i = q.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [q[i], q[j]] = [q[j], q[i]]; }
  try {
    const st = await api('POST', '/api/player/play', { path: q[0], queue: q });
    if (S.status) { S.status.player = st; S.statusAt = performance.now(); }
    renderMini();
    toast(`Shuffling ${what} on your Rondo`);
  } catch (e) { if (e.status !== 401) toast(`Couldn’t play them. ${reason(e)}`, 'err'); }
}
function setKind(kind) {
  S.libKind = kind;
  showKinds();
  S.openPlaylist = null;
  S.openSmart = null;
  if (S.libKind === 'files') return loadFiles(S.filesDir);
  if (S.libKind === 'albums') S.cfAt = S.cfAt || 0;
  if (S.libKind === 'photos') return loadPhotos();
  renderLibrary();
  if (S.libKind === 'playlists') loadPlaylists().then(() => { if (S.libKind === 'playlists') renderLibrary(); });
}
let searchTimer = 0;
$('#libSearch').addEventListener('input', (e) => {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(() => { S.libQuery = e.target.value; renderLibrary(); }, 140);
});
$('#libSearch').addEventListener('keydown', (e) => { if (e.key === 'Escape' && e.target.value) { e.target.value = ''; S.libQuery = ''; renderLibrary(); } });
function setSort(v) { S.libSort = v; store.set('rondo_sort', v); renderLibrary(); }
$('#libSort').addEventListener('change', (e) => setSort(e.target.value));
$('#libHead').addEventListener('click', (e) => { const b = e.target.closest('[data-sort]'); if (b) setSort(b.dataset.sort); });

async function playItem(it) {
  try {
    const p = await api('POST', '/api/player/play', { path: it.path });
    if (S.status) { S.status.player = p; S.statusAt = performance.now(); }
    renderMini(); markPlayingRows();
    toast(`Playing “${it.title || fileName(it.path)}” on your Rondo`);
    return true;
  } catch (e) {
    if (e.status !== 401) toast(`Couldn’t play it. ${reason(e)}`, 'err');
    return false;
  }
}

// ---------- sheet (details and confirmations) ----------
const sheet = $('#sheet');
sheet.addEventListener('click', (e) => { if (e.target === sheet) sheet.close(); });
function openSheet(html) {
  sheet.innerHTML = `<div class="sheet-body"><div class="sheet-grab" aria-hidden="true"></div>${html}</div>`;
  if (!sheet.open) sheet.showModal();
}
function confirmSheet({ title, text, action, danger = true }) {
  return new Promise((resolve) => {
    openSheet(`<h2>${esc(title)}</h2><p>${text}</p><div class="sheet-actions"><button class="btn" type="button" data-r="0">Cancel</button><button class="btn ${danger ? 'btn-danger' : 'btn-primary'}" type="button" data-r="1">${esc(action)}</button></div>`);
    $('[data-r="1"]', sheet).focus();
    const onClick = (e) => { const b = e.target.closest('[data-r]'); if (b) finish(b.dataset.r === '1'); };
    const finish = (v) => { sheet.removeEventListener('click', onClick); sheet.removeEventListener('close', onClose); if (sheet.open) sheet.close(); resolve(v); };
    const onClose = () => finish(false);
    sheet.addEventListener('click', onClick);
    sheet.addEventListener('close', onClose);
  });
}
function openItem(it) {
  const video = !!it._video, title = it.title || fileName(it.path);
  const facts = video
    ? [['Length', fmtDur(it.durationSec)], ['Size', fmtBytes(it.size)], ['File', it.path, 'path']]
    : [['Artist', it.artist], ['Album artist', it.albumArtist && it.albumArtist !== it.artist ? it.albumArtist : ''], ['Album', it.album],
      ['Disc', it.disc || ''], ['Track', it.track || ''], ['Year', it.year || ''], ['Genre', it.genre || ''], ['Length', fmtDur(it.durationSec)],
      ['Played', it.plays ? `${plural(it.plays, 'time')}, last ${new Date(it.lastPlayed).toLocaleDateString(undefined, { dateStyle: 'medium' })}` : 'Never'],
      ['Size', fmtBytes(it.size)], ['File', it.path, 'path']];
  const detail = () => {
    openSheet(`<div class="song-sheet${video ? ' is-video' : ''}"><span class="art"${video || it.art ? ` data-art="${esc(it.path)}"` : ''}>${icon(video ? 'film' : 'note')}</span>
      <div><h2>${esc(title)}</h2><p>${esc(video ? 'Video on the card' : [it.artist, it.album].filter(Boolean).join(' · ') || 'On the card')}</p></div></div>
      ${video ? '' : `<div class="rate" role="radiogroup" aria-label="Rating">${[1, 2, 3, 4, 5].map((n) => `<button type="button" role="radio" aria-checked="${n === it.rating}" aria-label="${plural(n, 'star')}" data-rate="${n}" class="${n <= it.rating ? 'on' : ''}">★</button>`).join('')}</div>`}
      <dl class="facts">${facts.filter((f) => f[1] !== '' && f[1] != null).map(([k, v, c]) => `<dt>${k}</dt><dd${c ? ` class="${c}"` : ''}>${esc(v)}</dd>`).join('')}</dl>
      <div class="sheet-actions"><button class="btn btn-red-text" type="button" data-act="delete">${icon('trash')}<span>Delete</span></button>${video ? '' : `<button class="btn" type="button" data-act="playlist">${icon('playlist')}<span>Add to playlist</span></button>`}${video ? '' : `<button class="btn btn-primary" type="button" data-act="play">${icon('play')}<span>Play on Rondo</span></button>`}</div>`);
    const art = $('.art[data-art]', sheet);
    if (art) loadArtInto(art, 240);
    ($('[data-act="play"]', sheet) || $('[data-act="delete"]', sheet)).focus();
  };
  const confirm = (err = '') => {
    openSheet(`<h2>Delete this ${video ? 'video' : 'song'}?</h2><p>“${esc(title)}” will be removed from the card. This can’t be undone.</p>
      ${err ? `<div class="note err" style="margin-top:14px">${icon('alert')}<span>${esc(err)}</span></div>` : ''}
      <div class="sheet-actions"><button class="btn" type="button" data-act="back">Cancel</button><button class="btn btn-danger" type="button" data-act="really">Delete</button></div>`);
    $('[data-act="back"]', sheet).focus();
  };
  const onClick = async (e) => {
    const star = e.target.closest('[data-rate]');
    if (star) {  // the same star again takes the rating away
      const n = +star.dataset.rate === it.rating ? 0 : +star.dataset.rate;
      try {
        const t = await api('PUT', '/api/library/rating', { path: it.path, rating: n });
        it.rating = t.rating ?? n;
        $$('[data-rate]', sheet).forEach((x) => { x.classList.toggle('on', +x.dataset.rate <= it.rating); x.setAttribute('aria-checked', String(+x.dataset.rate === it.rating)); });
        if (sortKey() === 'rating') renderLibrary();
      } catch (err) { if (err.status !== 401) toast(`Not saved. ${reason(err)}`, 'err'); }
      return;
    }
    const b = e.target.closest('[data-act]');
    if (!b) return;
    const act = b.dataset.act;
    if (act === 'play') { b.disabled = true; if (await playItem(it)) sheet.close(); else b.disabled = false; }
    if (act === 'playlist') { sheet.removeEventListener('click', onClick); return addToPlaylistSheet(it); }
    if (act === 'delete') confirm();
    if (act === 'back') detail();
    if (act === 'really') {
      b.disabled = true;
      b.textContent = 'Deleting…';
      try {
        await api('DELETE', `/api/library?path=${encodeURIComponent(it.path)}`);
        removeFromLibrary(it);
        sheet.close();
        toast(`Deleted “${title}”`);
      } catch (e2) {
        if (e2.status !== 401) confirm(`Couldn’t delete it. ${reason(e2)}`);
      }
    }
  };
  sheet.addEventListener('click', onClick);
  sheet.addEventListener('close', () => sheet.removeEventListener('click', onClick), { once: true });
  detail();
}
function removeFromLibrary(it) {
  const L = S.library;
  S.playlists = null; // the Rondo took it out of every playlist too
  L.songs = L.songs.filter((s) => s !== it);
  L.videos = L.videos.filter((v) => v !== it);
  S.libByPath.delete(it.path);
  const idx = S.libRows.indexOf(it);
  if (idx >= 0) {
    S.libRows.splice(idx, 1);
    if (idx < S.libShown) S.libShown--;
  }
  const row = $$('.song', libList).find((r) => r.dataset.path === it.path);
  if (row) {
    const prev = row.previousElementSibling, next = row.nextElementSibling;
    row.remove();
    // drop a group header left with nothing under it
    if (prev?.classList.contains('lhdr') && (!next || next.classList.contains('lhdr'))) {
      prev.remove();
      const h = S.libRows.findIndex((r) => r.h === prev.textContent.replace(prev.querySelector('small')?.textContent || '', ''));
      if (h >= 0) { S.libRows.splice(h, 1); if (h < S.libShown) S.libShown--; }
    }
  }
  renderLibrarySummary();
  if (!(S.libKind === 'songs' ? L.songs : L.videos).length) renderLibrary();
}

// ---------- playlists (.m3u8 files in the card's Playlists folder; local/Playlists.kt) ----------
const plTime = (sec) => (sec >= 3600 ? `${Math.floor(sec / 3600)} h ${Math.round((sec % 3600) / 60)} min` : `${Math.round(sec / 60)} min`);
const plSub = (p) => [plural(p.paths.length - p.missing, 'song'), p.durationSec ? plTime(p.durationSec) : '', p.missing ? `${p.missing} not on the card` : ''].filter(Boolean).join(' · ');
async function loadPlaylists() {
  try {
    const [pl, sm] = await Promise.all([api('GET', '/api/playlists'), api('GET', '/api/smart').catch(() => ({ playlists: [] }))]);
    S.playlists = pl.playlists;
    S.smart = sm.playlists || [];
  } catch (e) {
    if (e.status !== 401) toast(`Couldn’t read the playlists. ${reason(e)}`, 'err');
    S.playlists = S.playlists || [];
  }
  return S.playlists;
}
function keepPlaylist(p, oldName = p.name) {
  const i = S.playlists.findIndex((x) => x.name === oldName);
  if (i >= 0) S.playlists[i] = p; else S.playlists.push(p);
  S.playlists.sort((a, b) => collator.compare(a.name, b.name));
}
async function savePlaylist(name, change) {
  try {
    const p = await api('PUT', `/api/playlists?name=${encodeURIComponent(name)}`, change);
    keepPlaylist(p, name);
    return p;
  } catch (e) {
    if (e.status !== 401) toast(`Couldn’t save it. ${reason(e)}`, 'err');
    return null;
  }
}
// a one-line question in the sheet; resolves to the text, or null on Cancel
function promptSheet({ title, text = '', value = '', action, placeholder = '' }) {
  return new Promise((resolve) => {
    openSheet(`<h2>${esc(title)}</h2>${text ? `<p>${text}</p>` : ''}<form class="sheet-form"><input type="text" maxlength="60" value="${esc(value)}" placeholder="${esc(placeholder)}" aria-label="${esc(title)}" autocomplete="off"><div class="sheet-actions"><button class="btn" type="button" data-r="0">Cancel</button><button class="btn btn-primary" type="submit">${esc(action)}</button></div></form>`);
    const input = $('input', sheet), form = $('form', sheet);
    input.focus();
    input.select();
    const finish = (v) => { sheet.removeEventListener('close', onClose); form.removeEventListener('submit', onSubmit); if (sheet.open) sheet.close(); resolve(v); };
    const onSubmit = (e) => { e.preventDefault(); if (input.value.trim()) finish(input.value.trim()); else input.classList.add('is-invalid'); };
    const onClose = () => finish(null);
    form.addEventListener('submit', onSubmit);
    $('[data-r="0"]', sheet).addEventListener('click', () => finish(null));
    sheet.addEventListener('close', onClose);
  });
}
async function newPlaylist(paths = []) {
  const name = await promptSheet({ title: 'New playlist', action: 'Create', placeholder: 'Name' });
  if (!name) return null;
  try {
    const p = await api('POST', '/api/playlists', { name, paths });
    keepPlaylist(p);
    toast(paths.length ? `Added to “${p.name}”` : `Made “${p.name}”`);
    return p;
  } catch (e) {
    if (e.status !== 401) toast(`Couldn’t make it. ${reason(e)}`, 'err');
    return null;
  }
}
function renderPlaylists() {
  const all = S.playlists;
  if (!all) { libList.innerHTML = '<div class="lib-skel"></div>'.repeat(3); return; }
  if (S.openSmart) return renderSmart();
  if (S.openPlaylist) return renderPlaylist();
  const q = fold(S.libQuery.trim());
  const shown = q ? all.filter((p) => q.split(/\s+/).every((w) => fold(p.name).includes(w))) : all;
  $('#libSummary').textContent = all.length ? `${plural(all.length, 'playlist')}, made here or on your Rondo. They’re saved on the card.` : 'Playlists of songs on the card. They’re saved on the card, so they survive a reflash.';
  libList.innerHTML = `<div class="song pl-row pl-new" role="listitem" data-act="pl-new"><span class="art art-add">${icon('plus')}</span><span class="s-main"><span class="s-title">New playlist</span></span></div>` +
    (shown.length ? shown.map((p) => `<div class="song pl-row" role="listitem" data-pl="${esc(p.name)}"><span class="art">${icon('playlist')}</span>
      <span class="s-main"><span class="s-title">${esc(p.name)}</span><span class="s-sub">${esc(plSub(p))}</span></span>
      <span class="s-acts">${p.paths.length > p.missing ? `<button class="ib ib-sm" type="button" data-act="pl-play" aria-label="Play ${esc(p.name)} on your Rondo">${icon('play')}</button>` : ''}<button class="ib ib-sm" type="button" data-act="pl-open" aria-label="Open ${esc(p.name)}">${icon('chevron')}</button></span></div>`).join('')
      : all.length ? `<div class="lib-empty"><h3>No matches</h3><p>No playlist is called anything like “${esc(S.libQuery.trim())}”.</p></div>` : '') +
    smartListHtml(q);
}
function renderPlaylist() {
  const p = S.playlists.find((x) => x.name === S.openPlaylist);
  if (!p) { S.openPlaylist = null; return renderPlaylists(); }
  $('#libSummary').textContent = 'A playlist on the card.';
  const rows = p.paths.map((path, i) => {
    const s = S.libByPath.get(path);
    const up = `<button class="ib ib-sm" type="button" data-act="pl-up" data-i="${i}" aria-label="Move up"${i ? '' : ' disabled'}>${icon('up')}</button>`;
    const down = `<button class="ib ib-sm" type="button" data-act="pl-down" data-i="${i}" aria-label="Move down"${i < p.paths.length - 1 ? '' : ' disabled'}>${icon('down')}</button>`;
    const rm = `<button class="ib ib-sm" type="button" data-act="pl-remove" data-i="${i}" aria-label="Remove from the playlist">${icon('close')}</button>`;
    if (!s) return `<div class="song pl-song is-missing" role="listitem"><span class="art">${icon('note')}</span><span class="s-main"><span class="s-title">${esc(fileName(path))}</span><span class="s-sub">Not on the card any more</span></span><span class="s-acts">${up}${down}${rm}</span></div>`;
    return `<div class="song pl-song${isPlaying(s) ? ' is-playing' : ''}" role="listitem" data-path="${esc(path)}" data-i="${i}">
      <span class="art"${s.art ? ` data-art="${esc(path)}"` : ''}>${icon('note')}</span>
      <span class="s-main"><span class="s-title">${esc(s.title || fileName(path))}</span><span class="s-sub">${esc([s.artist, s.album].filter(Boolean).join(' · '))}</span></span>
      <span class="s-time">${fmtDur(s.durationSec)}</span><span class="s-acts">${up}${down}${rm}</span></div>`;
  }).join('');
  const playable = p.paths.length > p.missing;
  libList.innerHTML = `<div class="pl-head">
      <button class="btn btn-sm btn-quiet" type="button" data-act="pl-back">${icon('chevron', 'i flip')}<span>Playlists</span></button>
      <h2>${esc(p.name)}</h2><p>${esc(plSub(p))}</p>
      <div class="pl-acts">
        <button class="btn btn-primary btn-sm" type="button" data-act="pl-play"${playable ? '' : ' disabled'}>${icon('play')}<span>Play on Rondo</span></button>
        <button class="btn btn-sm" type="button" data-act="pl-shuffle"${playable ? '' : ' disabled'}>${icon('shuffle')}<span>Shuffle</span></button>
        <button class="btn btn-sm" type="button" data-act="pl-add">${icon('plus')}<span>Add songs</span></button>
        <button class="btn btn-sm" type="button" data-act="pl-rename">Rename</button>
        <button class="btn btn-sm btn-red-text" type="button" data-act="pl-delete">${icon('trash')}<span>Delete</span></button>
      </div></div>` + (rows || '<div class="lib-empty"><h3>No songs yet</h3><p>Use Add songs, or Add to playlist in a song’s details.</p></div>');
  $$('.art[data-art]:not([data-q])', libList).forEach((el) => { el.dataset.q = '1'; artIO.observe(el); });
}
async function playPlaylist(p, path) {
  const onCard = p.paths.filter((x) => S.libByPath.has(x));
  if (!onCard.length) return;
  try {
    const st = await api('POST', '/api/player/play', { path: path || onCard[0], queue: onCard });
    if (S.status) { S.status.player = st; S.statusAt = performance.now(); }
    renderMini();
    toast(`Playing “${p.name}” on your Rondo`);
  } catch (e) { if (e.status !== 401) toast(`Couldn’t play it. ${reason(e)}`, 'err'); }
}
async function onPlaylistClick(e) {
  if (await onSmartClick(e)) return;
  const b = e.target.closest('[data-act]'), row = e.target.closest('.song');
  const act = b?.dataset.act || (row?.dataset.pl ? 'pl-open' : row?.dataset.path ? 'pl-song' : '');
  const name = row?.dataset.pl || S.openPlaylist;
  const p = S.playlists?.find((x) => x.name === name);
  if (act === 'pl-new') { const made = await newPlaylist(); if (made) { S.openPlaylist = made.name; renderPlaylists(); } return; }
  if (act === 'pl-back') { S.openPlaylist = null; return renderPlaylists(); }
  if (!p) return;
  if (act === 'pl-open') { S.openPlaylist = p.name; window.scrollTo({ top: Math.min(window.scrollY, libEl.offsetTop - 80) }); return renderPlaylists(); }
  if (act === 'pl-play') return playPlaylist(p);
  if (act === 'pl-shuffle') return shufflePaths(p.paths, `“${p.name}”`);
  if (act === 'pl-song') return playPlaylist(p, row.dataset.path);
  if (act === 'pl-add') return addSongsSheet(p.name);
  if (act === 'pl-rename') {
    const next = await promptSheet({ title: 'Rename playlist', value: p.name, action: 'Rename' });
    if (!next || next === p.name) return;
    const saved = await savePlaylist(p.name, { name: next });
    if (saved) { S.openPlaylist = saved.name; renderPlaylists(); }
    return;
  }
  if (act === 'pl-delete') {
    if (!(await confirmSheet({ title: `Delete “${p.name}”?`, text: 'The playlist goes; its songs stay on the card.', action: 'Delete' }))) return;
    try {
      await api('DELETE', `/api/playlists?name=${encodeURIComponent(p.name)}`);
      S.playlists = S.playlists.filter((x) => x !== p);
      S.openPlaylist = null;
      renderPlaylists();
      toast(`Deleted “${p.name}”`);
    } catch (e2) { if (e2.status !== 401) toast(`Couldn’t delete it. ${reason(e2)}`, 'err'); }
    return;
  }
  const i = Number(b?.dataset.i);
  if (!Number.isInteger(i)) return;
  const paths = p.paths.slice();
  if (act === 'pl-remove') paths.splice(i, 1);
  else if (act === 'pl-up' && i > 0) [paths[i - 1], paths[i]] = [paths[i], paths[i - 1]];
  else if (act === 'pl-down' && i < paths.length - 1) [paths[i + 1], paths[i]] = [paths[i], paths[i + 1]];
  else return;
  if (await savePlaylist(p.name, { paths })) renderPlaylists();
}
// pick songs for a playlist: search the card's songs, Add puts one at the end
function addSongsSheet(name) {
  let q = '';
  const draw = () => {
    const p = S.playlists.find((x) => x.name === name);
    if (!p) return sheet.close();
    const words = fold(q.trim()).split(/\s+/).filter(Boolean);
    const songs = (S.library?.songs || []).filter((s) => words.every((w) => s._key.includes(w))).slice(0, 60);
    $('.pick-list', sheet).innerHTML = songs.length ? songs.map((s) => {
      const has = p.paths.includes(s.path);
      return `<div class="pick" data-path="${esc(s.path)}"><span class="s-main"><span class="s-title">${esc(s.title || fileName(s.path))}</span><span class="s-sub">${esc([s.artist, s.album].filter(Boolean).join(' · '))}</span></span>
        <button class="btn btn-sm${has ? '' : ' btn-primary'}" type="button" data-act="add"${has ? ' disabled' : ''}>${has ? 'Added' : 'Add'}</button></div>`;
    }).join('') : `<p class="pick-none">${q.trim() ? 'No songs match.' : 'No songs on the card.'}</p>`;
  };
  openSheet(`<h2>Add to “${esc(name)}”</h2><label class="search pick-search">${icon('search')}<input type="search" placeholder="Search songs, artists, albums" autocomplete="off" aria-label="Search songs"></label><div class="pick-list"></div><div class="sheet-actions"><button class="btn" type="button" data-act="done">Done</button></div>`);
  const input = $('input', sheet);
  input.addEventListener('input', () => { q = input.value; draw(); });
  input.focus();
  const onClick = async (e) => {
    const b = e.target.closest('[data-act]');
    if (!b) return;
    if (b.dataset.act === 'done') return sheet.close();
    const path = b.closest('.pick')?.dataset.path;
    const p = S.playlists.find((x) => x.name === name);
    if (!path || !p) return;
    b.disabled = true;
    b.textContent = 'Adding…';
    if (await savePlaylist(name, { paths: [...p.paths, path] })) draw(); else { b.disabled = false; b.textContent = 'Add'; }
  };
  sheet.addEventListener('click', onClick);
  sheet.addEventListener('close', () => { sheet.removeEventListener('click', onClick); if (S.libKind === 'playlists') renderPlaylists(); }, { once: true });
  draw();
}
// from a song's details: pick a playlist, or start a new one with it
async function addToPlaylistSheet(it) {
  const all = await loadPlaylists();
  openSheet(`<h2>Add to a playlist</h2><p>“${esc(it.title || fileName(it.path))}”</p><div class="pick-list">${all.map((p) => {
    const has = p.paths.includes(it.path);
    return `<div class="pick" data-pl="${esc(p.name)}"><span class="s-main"><span class="s-title">${esc(p.name)}</span><span class="s-sub">${esc(plSub(p))}</span></span><button class="btn btn-sm${has ? '' : ' btn-primary'}" type="button" data-act="add"${has ? ' disabled' : ''}>${has ? 'Added' : 'Add'}</button></div>`;
  }).join('')}</div><div class="sheet-actions"><button class="btn" type="button" data-act="done">Done</button><button class="btn btn-primary" type="button" data-act="new">${icon('plus')}<span>New playlist</span></button></div>`);
  const onClick = async (e) => {
    const b = e.target.closest('[data-act]');
    if (!b) return;
    if (b.dataset.act === 'done') return sheet.close();
    if (b.dataset.act === 'new') { sheet.removeEventListener('click', onClick); await newPlaylist([it.path]); return; }
    const p = S.playlists.find((x) => x.name === b.closest('.pick')?.dataset.pl);
    if (!p) return;
    b.disabled = true;
    const saved = await savePlaylist(p.name, { paths: [...p.paths, it.path] });
    if (saved) {
      b.textContent = 'Added';
      b.classList.remove('btn-primary');
      $('.s-sub', b.closest('.pick')).textContent = plSub(saved);
      toast(`Added to “${p.name}”`);
    } else b.disabled = false;
  };
  sheet.addEventListener('click', onClick);
  sheet.addEventListener('close', () => sheet.removeEventListener('click', onClick), { once: true });
}

// ---------- rescan ----------
let scanTimer = 0;
$('#rescanBtn').addEventListener('click', rescan);
async function rescan() {
  const btn = $('#rescanBtn');
  btn.classList.add('is-busy');
  btn.disabled = true;
  try {
    await api('POST', '/api/library/rescan');
    if (S.library) S.library.scanning = true;
    renderLibrarySummary();
    watchScan();
  } catch (e) {
    if (e.status !== 401) toast(`Couldn’t start a scan. ${reason(e)}`, 'err');
    btn.disabled = false;
  }
  btn.classList.remove('is-busy');
}
function watchScan(onDone) {
  clearTimeout(scanTimer);
  $('#rescanBtn').disabled = true;
  const before = S.library ? S.library.songs.length + S.library.videos.length : 0;
  const check = async () => {
    if (document.hidden) { scanTimer = setTimeout(check, 2000); return; }
    try {
      const L = await api('GET', '/api/library', undefined, { timeout: 45000 });
      if (L.scanning) { scanTimer = setTimeout(check, 2000); return; }
      S.library = L;
      prepLibrary();
      if (S.route === 'library') renderLibrary(); else renderLibrarySummary();
      if (S.uploads.length) renderUploads();
      const added = L.songs.length + L.videos.length - before;
      toast(added > 0 ? `Library updated: ${plural(added, 'new file')}` : 'Library is up to date');
      onDone?.();
    } catch (e) {
      if (e.status !== 401) scanTimer = setTimeout(check, 4000);
    }
  };
  scanTimer = setTimeout(check, 1500);
}

// ---------- uploads ----------
let upId = 0;
const fileInput = $('#fileInput');
$('#uploadBtn').addEventListener('click', () => fileInput.click());
$('#dropPick').addEventListener('click', () => fileInput.click());
fileInput.addEventListener('change', () => { enqueue([...fileInput.files]); fileInput.value = ''; });

// where an upload goes by its kind, as Jellyfin keeps them: Music, Movies (else an existing Videos
// folder), Pictures (else an existing Photos or DCIM folder), each made on the card when missing
function folderFor(ext) {
  const folders = S.library?.folders || [];
  const root = (folders[0] || '/storage/sdcard0/Music').split('/').slice(0, 3).join('/');
  const top = (re) => folders.find((f) => f.split('/').length === 4 && re.test(f.split('/')[3]));
  if (VIDEO.includes(ext)) return top(/^movies$/i) || top(/^videos?$/i) || `${root}/Movies`;
  if (PHOTOS.includes(ext)) return top(/^pictures$/i) || top(/^photos$/i) || top(/^dcim$/i) || `${root}/Pictures`;
  return top(/^music$/i) || `${root}/Music`;
}
// folder: where they go (the Files tab's folder), else Music or a video folder by their kind
function enqueue(files, folder = null) {
  const bad = [];
  let added = 0;
  for (const f of files) {
    const ext = extOf(f.name);
    if (!(folder ? ALLOWED_FILES : ALLOWED).includes(ext) || !f.size) { bad.push(f.name); continue; }
    S.uploads.push({ id: ++upId, file: f, name: f.name, size: f.size, loaded: 0, status: 'queued', folder: folder || folderFor(ext) });
    added++;
  }
  if (bad.length) toast(bad.length === 1 ? `Skipped “${bad[0]}”: Rondo can’t play that kind of file` : `Skipped ${bad.length} files Rondo can’t play`, 'err');
  if (!added) return;
  renderUploads();
  pumpUploads();
}
const uploading = () => S.uploads.find((u) => u.status === 'uploading');
function pumpUploads() {
  if (uploading() || !S.session && !document.cookie.includes('rondo_session')) return;
  const next = S.uploads.find((u) => u.status === 'queued');
  if (next) return startUpload(next);
  const fresh = S.uploads.filter((u) => u.status === 'done' && !u.scanned);
  if (fresh.length) {
    fresh.forEach((u) => (u.scanned = true));
    if (fresh.some((u) => extOf(u.name) !== 'lrc')) {
      api('POST', '/api/library/rescan').then(() => {
        if (S.library) { S.library.scanning = true; renderLibrarySummary(); }
        watchScan();
      }).catch(() => {});
    }
  }
  renderUploads();
}
function uploadError(status, code) {
  if (status === 0) return 'Connection lost';
  if (status === 415) return 'Rondo can’t play this kind of file';
  if (status === 507) return 'Not enough space on the card';
  if (status === 403) return 'Not allowed in that folder';
  return code ? `Failed (${code})` : `Failed (error ${status})`;
}
function startUpload(u) {
  const xhr = new XMLHttpRequest();
  u.xhr = xhr;
  u.status = 'uploading';
  u.started = performance.now();
  u.loaded = 0;
  u.error = '';
  xhr.open('PUT', `/api/library/upload?name=${encodeURIComponent(u.name)}&folder=${encodeURIComponent(u.folder)}`);
  if (S.session) xhr.setRequestHeader('X-Rondo-Session', S.session);
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');
  xhr.upload.onprogress = (e) => { u.loaded = e.loaded; updateUploadProgress(u); };
  xhr.onload = () => {
    let data = null;
    try { data = JSON.parse(xhr.responseText); } catch { /* none */ }
    if (xhr.status === 200) { u.status = 'done'; u.loaded = u.size; u.path = data?.path; if (S.libKind === 'files' && u.folder === S.filesDir) loadFiles(S.filesDir, true); }
    else if (xhr.status === 401) { u.status = 'queued'; needPair(); return; }
    else { u.status = 'error'; u.error = uploadError(xhr.status, data?.error); }
    u.xhr = null;
    pumpUploads();
  };
  xhr.onerror = () => { u.status = 'error'; u.error = uploadError(0); u.xhr = null; pumpUploads(); };
  xhr.onabort = () => { u.status = 'canceled'; u.xhr = null; pumpUploads(); };
  xhr.send(u.file);
  renderUploads();
}
function uploadMeta(u) {
  if (u.status === 'queued') return { text: `Waiting · ${fmtBytes(u.size)}`, cls: '' };
  if (u.status === 'uploading') return { text: `${Math.floor((u.loaded / u.size) * 100)}% · ${fmtBytes(u.loaded)} of ${fmtBytes(u.size)}`, cls: '' };
  if (u.status === 'done') return { text: `Copied to ${fileName(u.folder)} · ${fmtBytes(u.size)}`, cls: 'ok' };
  if (u.status === 'canceled') return { text: 'Canceled', cls: '' };
  return { text: u.error || 'Failed', cls: 'err' };
}
function uploadsHead() {
  const list = S.uploads;
  const active = list.filter((u) => u.status === 'queued' || u.status === 'uploading');
  const done = list.filter((u) => u.status === 'done').length;
  const failed = list.filter((u) => u.status === 'error').length;
  const totalBytes = list.filter((u) => u.status !== 'canceled').reduce((n, u) => n + u.size, 0);
  const sent = list.reduce((n, u) => n + (u.status === 'done' ? u.size : u.status === 'uploading' ? u.loaded : 0), 0);
  const cur = uploading();
  let title, sub;
  if (active.length) {
    const nth = list.filter((u) => u.status !== 'canceled').indexOf(cur || active[0]) + 1;
    title = `Uploading ${nth} of ${list.filter((u) => u.status !== 'canceled').length}`;
    const elapsed = cur ? (performance.now() - cur.started) / 1000 : 0;
    const speed = cur && elapsed > 1.5 ? cur.loaded / elapsed : 0;
    const left = active.reduce((n, u) => n + u.size - u.loaded, 0);
    const eta = speed ? left / speed : 0;
    sub = `${fmtBytes(sent)} of ${fmtBytes(totalBytes)}${eta > 5 ? ` · about ${eta > 90 ? `${Math.round(eta / 60)} min` : `${Math.round(eta)} s`} left` : ''}. Keep the dashboard open on your Rondo.`;
  } else {
    title = failed ? `${plural(done, 'file')} copied, ${failed} failed` : `${plural(done, 'file')} copied to the card`;
    sub = S.library?.scanning ? 'Adding them to the library…' : done ? 'They’re in the library now.' : 'Nothing was copied.';
  }
  const pct = totalBytes ? (sent / totalBytes) * 100 : 0;
  return { title, sub, pct, active: active.length > 0 };
}
function renderUploads() {
  const box = $('#uploads');
  if (!S.uploads.length) { box.hidden = true; updateBadges(); return; }
  box.hidden = false;
  const h = uploadsHead();
  const free = S.status?.storage?.freeBytes;
  const pendingBytes = S.uploads.filter((u) => u.status === 'queued' || u.status === 'uploading').reduce((n, u) => n + u.size - u.loaded, 0);
  box.innerHTML = `
    <div class="up-head">
      <span class="tile t-music">${icon('upload')}</span>
      <div class="row-main"><div class="row-title" id="upTitle">${esc(h.title)}</div><div class="row-sub" id="upSub">${esc(h.sub)}</div>${h.active ? `<div class="meter"><span id="upMeter" style="--p:${h.pct.toFixed(1)}%"></span></div>` : ''}</div>
      ${h.active ? '<button class="btn btn-sm btn-quiet" type="button" data-up="cancel-all">Cancel all</button>' : '<button class="btn btn-sm" type="button" data-up="clear">Done</button>'}
    </div>
    ${free != null && pendingBytes > free ? `<div class="note err" style="margin:0 16px 14px">${icon('alert')}<span>These files need ${fmtBytes(pendingBytes)} but the card has ${fmtBytes(free)} free. Some will fail.</span></div>` : ''}
    <div class="up-list">${S.uploads.map((u) => {
      const m = uploadMeta(u);
      const btn = u.status === 'error'
        ? `<button class="ib ib-sm" type="button" data-up="retry" data-id="${u.id}" aria-label="Try ${esc(u.name)} again">${icon('refresh')}</button>`
        : `<button class="ib ib-sm" type="button" data-up="${u.status === 'queued' || u.status === 'uploading' ? 'cancel' : 'remove'}" data-id="${u.id}" aria-label="${u.status === 'queued' || u.status === 'uploading' ? 'Cancel' : 'Remove'} ${esc(u.name)}">${icon('close')}</button>`;
      return `<div class="up-row" id="up-${u.id}"><span class="up-name">${esc(u.name)}</span>${btn}<span class="up-meta ${m.cls}">${esc(m.text)}</span>${u.status === 'uploading' ? `<div class="meter"><span style="--p:${((u.loaded / u.size) * 100).toFixed(1)}%"></span></div>` : ''}</div>`;
    }).join('')}</div>`;
  updateBadges();
}
let progressFrame = 0;
function updateUploadProgress(u) {
  if (progressFrame) return;
  progressFrame = requestAnimationFrame(() => {
    progressFrame = 0;
    const row = $(`#up-${u.id}`);
    if (!row) return;
    const meter = $('.meter span', row);
    if (meter) meter.style.setProperty('--p', `${((u.loaded / u.size) * 100).toFixed(1)}%`);
    $('.up-meta', row).textContent = uploadMeta(u).text;
    const h = uploadsHead();
    $('#upTitle').textContent = h.title;
    $('#upSub').textContent = h.sub;
    $('#upMeter')?.style.setProperty('--p', `${h.pct.toFixed(1)}%`);
  });
}
$('#uploads').addEventListener('click', (e) => {
  const b = e.target.closest('[data-up]');
  if (!b) return;
  const u = S.uploads.find((x) => x.id === +b.dataset.id);
  switch (b.dataset.up) {
    case 'cancel': if (u.xhr) u.xhr.abort(); else { u.status = 'canceled'; renderUploads(); } break;
    case 'retry': u.status = 'queued'; u.loaded = 0; renderUploads(); pumpUploads(); break;
    case 'remove': S.uploads = S.uploads.filter((x) => x !== u); renderUploads(); break;
    case 'cancel-all':
      S.uploads.forEach((x) => { if (x.status === 'queued') x.status = 'canceled'; });
      uploading()?.xhr?.abort();
      renderUploads();
      break;
    case 'clear': S.uploads = S.uploads.filter((x) => x.status === 'queued' || x.status === 'uploading'); renderUploads(); break;
  }
});
function updateBadges() {
  const n = S.uploads.filter((u) => u.status === 'queued' || u.status === 'uploading').length;
  $$('[data-id="library"] .nav-badge').forEach((b) => (b.innerHTML = n ? `<span class="dot-badge" aria-label="${n} uploading">${n}</span>` : ''));
}
window.addEventListener('beforeunload', (e) => {
  if (S.uploads.some((u) => u.status === 'uploading' || u.status === 'queued')) { e.preventDefault(); e.returnValue = ''; }
});

// drag and drop anywhere on the page
const drop = $('#drop');
let dragDepth = 0;
const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes('Files');
const canDrop = () => S.booted && screens.pair.hidden && !sheet.open;
window.addEventListener('dragenter', (e) => { if (!hasFiles(e) || !canDrop()) return; e.preventDefault(); dragDepth++; drop.hidden = false; });
window.addEventListener('dragover', (e) => { if (!hasFiles(e)) return; e.preventDefault(); e.dataTransfer.dropEffect = canDrop() ? 'copy' : 'none'; });
window.addEventListener('dragleave', (e) => { if (!hasFiles(e)) return; dragDepth = Math.max(0, dragDepth - 1); if (!dragDepth) drop.hidden = true; });
window.addEventListener('drop', (e) => {
  if (!hasFiles(e)) return;
  e.preventDefault();
  dragDepth = 0;
  drop.hidden = true;
  if (!canDrop()) return;
  if (S.route !== 'library') location.hash = '#library';
  enqueue([...e.dataTransfer.files]);
});

// ---------- theme ----------
const MENU = {
  // t: the preview's dark backdrop behind the red logos (Tiles in Widgets.kt)
  music: { tile: 'music', glyph: 'note', brand: 'music', t: ['#3a3a3a', '#141414'], title: 'YouTube Music', logo: MUSIC_LOGO },
  videos: { tile: 'video', glyph: 'play', brand: 'video', t: ['#3a3a3a', '#141414'], title: 'YouTube', logo: VIDEO_LOGO },
  device: { tile: 'device', glyph: 'folder', t: ['#5fb7ff', '#1f73e0'], title: 'Library' },
  nowPlaying: { tile: 'now', glyph: 'bars', t: ['#7be07b', '#26a83b'], title: 'Nothing playing' },
  radio: { tile: 'radio', glyph: 'radio', t: ['#ffb04d', '#f07a12'], title: 'FM Radio' },
  settings: { tile: 'settings', glyph: 'gear', t: ['#b8bcc4', '#6f747d'], title: 'Settings' },
};
async function loadTheme() {
  try {
    S.theme = await api('GET', '/api/theme');
    applyAccent(S.theme.accent);
    renderTheme();
  } catch (e) {
    if (e.status === 401) return;
    $('#preview').innerHTML = '';
    $('#applied').className = 'applied err';
    $('#applied').innerHTML = `${icon('alert')}<span>Couldn’t load the theme. ${esc(reason(e))}</span> <button class="link" type="button" data-act="reload-theme">Try again</button>`;
  }
}
$('#applied').addEventListener('click', (e) => {
  if (e.target.closest('[data-act="reload-theme"]')) loadTheme();
  if (e.target.closest('[data-act="resend-theme"]')) {
    const t = S.theme;
    themePending = { accent: t.accent, appearance: t.appearance, textScale: t.textScale, menu: t.menu.map(({ id, visible }) => ({ id, visible })) };
    flushTheme();
  }
});
function renderTheme() {
  renderPreview();
  renderSwatches();
  renderThemeSegs();
  renderMenuItems();
}
function renderPreview() {
  const t = S.theme, el = $('#preview');
  if (!t || !el) return;
  const items = t.menu.filter((m) => m.visible);
  if (!items.some((m) => m.id === S.pvSel)) S.pvSel = items[0]?.id;
  const st = S.status || {}, p = st.player;
  const level = st.battery?.level ?? 80;
  const rows = items.map((m) => {
    const d = MENU[m.id] || MENU.settings;
    return `<div class="pv-row${m.id === S.pvSel ? ' sel' : ''}" data-id="${m.id}">${d.brand ? brandTile(d.brand, 'pv-tile brand') : `<span class="pv-tile" style="--t1:${d.t[0]};--t2:${d.t[1]}"><svg aria-hidden="true"><use href="#i-${d.glyph}"/></svg></span>`}<span class="pv-label">${esc(m.label)}</span><svg class="pv-chev" aria-hidden="true"><use href="#i-chevron"/></svg></div>`;
  }).join('');
  el.setAttribute('aria-label', `Preview of your Rondo’s main menu: ${items.map((m) => m.label).join(', ')}. ${t.appearance === 'dark' ? 'Dark' : 'Light'}, ${textScaleName(t.textScale)} text.`);
  el.innerHTML = `<div class="pv${t.appearance === 'dark' ? ' dark' : ''}" style="--s:${t.textScale}">
    <div class="pv-bar">${p?.playing ? '<svg class="pv-play" aria-hidden="true"><use href="#i-play"/></svg>' : ''}<b>Rondo</b>
      <span class="pv-right"><svg class="pv-wifi" style="opacity:${st.wifi ? 1 : 0.25}" aria-hidden="true"><use href="#i-wifi"/></svg><span class="pv-pct">${level}%</span>${battSvg(level, 'pv-batt')}</span></div>
    <div class="pv-body">${items.length ? `<div class="pv-list">${rows}</div><div class="pv-div"></div>${paneHtml(S.pvSel)}` : '<div class="pv-empty">Nothing in the menu</div>'}</div></div>`;
  // like the device: the Now Playing pane takes its color from the cover
  const cover = $('.pv-visual.art img', el);
  if (cover) {
    const tint = () => {
      const c = artColor(cover);
      const pane = cover.closest('.pv-pane');
      if (!c || !pane) return;
      pane.style.setProperty('--p1', c);
      pane.style.setProperty('--p2', darker(c, 0.7));
      pane.style.setProperty('--p3', darker(c, 0.45));
    };
    if (cover.complete && cover.naturalWidth) tint(); else cover.addEventListener('load', tint);
  }
  renderProgress();
}
function paneHtml(id) {
  const d = MENU[id] || MENU.settings, p = S.status?.player;
  if (id === 'nowPlaying' && p) {
    const sub = (p.subtitle || '').split(' · ')[0];
    return `<div class="pv-pane" style="--p1:#4a505c;--p2:#20232a;--p3:#16181d">
      <div class="pv-visual art">${p.art ? `<img alt="" src="${esc(p.art)}" onload="this.classList.add('ok')" onerror="artRetry(this)">` : ''}</div>
      <div class="pv-pt">${esc(p.title)}</div>${sub ? `<div class="pv-pd">${esc(sub)}</div>` : ''}<div class="pv-prog"><span></span></div></div>`;
  }
  const visual = d.logo
    ? `<div class="pv-visual">${d.logo}</div>`
    : `<div class="pv-visual boxed"><svg class="pv-glyph" aria-hidden="true"><use href="#i-${d.glyph}"/></svg></div>`;
  // like the device: the title only, no hint line under it
  return `<div class="pv-pane" style="--p1:${d.t[0]};--p2:${d.t[1]};--p3:${darker(d.t[1], 0.7)}">${visual}<div class="pv-pt">${esc(d.title)}</div></div>`;
}
$('#preview').addEventListener('click', (e) => {
  const r = e.target.closest('.pv-row');
  if (r) { S.pvSel = r.dataset.id; renderPreview(); }
});
const textScaleName = (s) => (s < 1.3 ? 'small' : s > 1.5 ? 'large' : 'medium');
function renderSwatches() {
  const t = S.theme, cur = (t.accent || '').toLowerCase();
  const presets = t.presets?.length ? t.presets : [{ name: 'iPod Blue', accent: '#1c5ccc' }];
  const isPreset = presets.some((p) => p.accent.toLowerCase() === cur);
  $('#swatches').innerHTML = presets.map((p) => `<button class="sw" type="button" role="radio" aria-checked="${p.accent.toLowerCase() === cur}" data-accent="${esc(p.accent)}" style="--c:${esc(p.accent)};--top:${accentTop(p.accent)}"><span class="sw-dot">${icon('check')}</span><span>${esc(p.name)}</span></button>`).join('')
    + `<div class="sw sw-custom" role="radio" aria-checked="${!isPreset}" style="--c:${esc(cur)};--top:${accentTop(cur)}"><span class="sw-dot">${icon('check')}</span><span>${isPreset ? 'Custom' : esc(cur)}</span><input type="color" value="${esc(cur)}" aria-label="Custom accent color"></div>`;
}
function syncSwatches() { // while dragging the color picker: no re-render, just the marks
  const cur = S.theme.accent.toLowerCase();
  let preset = false;
  $$('#swatches .sw[data-accent]').forEach((b) => { const on = b.dataset.accent.toLowerCase() === cur; preset ||= on; b.setAttribute('aria-checked', String(on)); });
  const c = $('#swatches .sw-custom');
  c.setAttribute('aria-checked', String(!preset));
  c.style.setProperty('--c', cur);
  c.style.setProperty('--top', accentTop(cur));
  $('span:not(.sw-dot)', c).textContent = preset ? 'Custom' : cur;
}
$('#swatches').addEventListener('click', (e) => {
  const b = e.target.closest('.sw[data-accent]');
  if (b) changeTheme({ accent: b.dataset.accent.toLowerCase() });
});
$('#swatches').addEventListener('input', (e) => { if (e.target.type === 'color') changeTheme({ accent: e.target.value.toLowerCase() }, 350); });
$('#swatches').addEventListener('change', (e) => { if (e.target.type === 'color') changeTheme({ accent: e.target.value.toLowerCase() }, 0); });
function renderThemeSegs() {
  const t = S.theme;
  $$('#appearance button').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.v === t.appearance)));
  $$('#textScale button').forEach((b) => b.setAttribute('aria-checked', String(Math.abs(+b.dataset.v - t.textScale) < 0.01)));
}
$('#appearance').addEventListener('click', (e) => { const b = e.target.closest('[data-v]'); if (b) changeTheme({ appearance: b.dataset.v }); });
$('#textScale').addEventListener('click', (e) => { const b = e.target.closest('[data-v]'); if (b) changeTheme({ textScale: +b.dataset.v }); });
function renderMenuItems(focus) {
  const menu = S.theme.menu;
  $('#menuItems').innerHTML = menu.map((m, i) => {
    const d = MENU[m.id] || MENU.settings;
    return `<div class="row menu-row${m.visible ? '' : ' is-off'}" data-id="${m.id}">
      ${d.brand ? brandTile(d.brand) : tileHtml(d.tile, d.glyph)}
      <div class="row-main"><div class="row-title">${esc(m.label)}</div>${m.visible ? '' : '<div class="row-sub">Hidden</div>'}</div>
      <div class="row-ctl">
        <button class="ib ib-sm" type="button" data-move="-1" aria-label="Move ${esc(m.label)} up"${i === 0 ? ' disabled' : ''}>${icon('up')}</button>
        <button class="ib ib-sm" type="button" data-move="1" aria-label="Move ${esc(m.label)} down"${i === menu.length - 1 ? ' disabled' : ''}>${icon('down')}</button>
        ${m.locked ? `<span class="locked" title="Settings is always in the menu">${icon('lock')}<span class="sr-only">Always shown</span></span>` : `<button class="switch" type="button" role="switch" aria-checked="${m.visible}" aria-label="Show ${esc(m.label)}"></button>`}
      </div></div>`;
  }).join('');
  if (focus) {
    const row = $(`#menuItems [data-id="${focus.id}"]`);
    const el = row && ($(`[data-move="${focus.move}"]:not([disabled])`, row) || $('[data-move]:not([disabled])', row));
    el?.focus();
  }
}
$('#menuItems').addEventListener('click', (e) => {
  const row = e.target.closest('.menu-row');
  if (!row) return;
  const menu = S.theme.menu.map((m) => ({ ...m }));
  const i = menu.findIndex((m) => m.id === row.dataset.id);
  const mv = e.target.closest('[data-move]');
  if (mv) {
    const j = i + +mv.dataset.move;
    if (j < 0 || j >= menu.length) return;
    [menu[i], menu[j]] = [menu[j], menu[i]];
    changeTheme({ menu }, 0, { id: row.dataset.id, move: mv.dataset.move });
  } else if (e.target.closest('.switch') && !menu[i].locked) {
    menu[i].visible = !menu[i].visible;
    changeTheme({ menu }, 0, { switchId: row.dataset.id });
  }
});

let themePending = {}, themeTimer = 0, themeBusy = false, appliedTimer = 0;
function changeTheme(patch, wait = 0, focus) {
  Object.assign(S.theme, patch);
  if (patch.accent) { applyAccent(patch.accent); syncSwatches(); }
  if (patch.appearance || patch.textScale) renderThemeSegs();
  if (patch.menu) {
    renderMenuItems(focus?.id ? focus : null);
    if (focus?.switchId) $(`#menuItems [data-id="${focus.switchId}"] .switch`)?.focus();
  }
  renderPreview();
  const wire = { ...patch };
  if (wire.menu) wire.menu = wire.menu.map(({ id, visible }) => ({ id, visible }));
  Object.assign(themePending, wire);
  setApplied('busy');
  clearTimeout(themeTimer);
  themeTimer = setTimeout(flushTheme, wait);
}
async function flushTheme() {
  if (themeBusy || !Object.keys(themePending).length) return;
  const body = themePending;
  themePending = {};
  themeBusy = true;
  try {
    const t = await api('PUT', '/api/theme', body);
    if (!Object.keys(themePending).length) {
      const same = t.accent?.toLowerCase() === S.theme.accent?.toLowerCase();
      S.theme = t;
      applyAccent(t.accent);
      if (!same) renderSwatches(); else syncSwatches();
      renderThemeSegs();
      renderMenuItems();
      renderPreview();
    }
    setApplied('ok');
  } catch (e) {
    if (e.status !== 401) setApplied('err', e);
  } finally {
    themeBusy = false;
    if (Object.keys(themePending).length) flushTheme();
  }
}
function setApplied(state, e) {
  const el = $('#applied');
  clearTimeout(appliedTimer);
  if (state === 'busy') { el.className = 'applied'; el.innerHTML = '<span>Applying…</span>'; return; }
  if (state === 'ok') {
    el.className = 'applied ok';
    el.innerHTML = `${icon('check')}<span>Applied on your Rondo</span>`;
    appliedTimer = setTimeout(() => el.classList.add('fade'), 2600);
    return;
  }
  el.className = 'applied err';
  el.innerHTML = `${icon('alert')}<span>Not applied. ${esc(reason(e))}</span> <button class="link" type="button" data-act="resend-theme">Try again</button>`;
}

// ---------- settings ----------
const QUALITY = [[null, 'Auto'], [720, '720p'], [480, '480p'], [360, '360p']];
const TIMEOUTS = [[15, '15 s'], [30, '30 s'], [60, '1 min'], [120, '2 min'], [300, '5 min'], [600, '10 min']];
const WHEEL = [['off', 'Off'], ['normal', 'Normal'], ['fast', 'Fast']];
const BUZZ = [['off', 'Off'], ['light', 'Light'], ['normal', 'Normal'], ['strong', 'Strong']];
async function loadSettings() {
  if (!S.settings) $('#settingsRows').innerHTML = '<div class="lib-skel"></div>'.repeat(5);
  try {
    S.settings = await api('GET', '/api/settings');
    renderSettings();
  } catch (e) {
    if (e.status !== 401 && !S.settings) $('#settingsRows').innerHTML = `<div class="row-empty">Couldn’t load the settings. ${esc(reason(e))} <button class="link" type="button" data-act="reload">Try again</button></div>`;
  }
}
function segHtml(key, options, cur, label) {
  return `<div class="seg seg-wide" role="radiogroup" aria-label="${esc(label)}" data-key="${key}">${options.map(([v, l]) => `<button type="button" role="radio" aria-checked="${v === cur}" data-v="${JSON.stringify(v)}">${l}</button>`).join('')}</div>`;
}
function renderSettings() {
  const s = S.settings;
  if (S.server) renderQuality();
  $('#settingsRows').innerHTML = `
    <div class="row row-wide set-row"><span class="tile t-brightness">${icon('sun')}</span>
      <div class="row-main"><div class="row-title">Brightness</div><div class="row-sub">Lower saves battery.</div></div>
      <div class="row-control"><div class="range-row">${icon('sun')}<input type="range" id="brightness" min="10" max="100" step="5" value="${s.brightness}" aria-label="Brightness"><output id="brightnessOut">${s.brightness}%</output></div></div></div>
    <div class="row row-wide set-row"><span class="tile t-timer">${icon('timer')}</span>
      <div class="row-main"><div class="row-title">Screen off after</div><div class="row-sub">Shorter saves battery.</div></div>
      <div class="row-control">${segHtml('screenTimeoutSec', TIMEOUTS, s.screenTimeoutSec, 'Screen off after')}</div></div>
    <div class="row set-row"><span class="tile t-lyrics">${icon('mic')}</span>
      <div class="row-main"><div class="row-title">Open lyrics automatically</div><div class="row-sub">Show the lyrics when Now Playing opens.</div></div>
      <button class="switch" type="button" role="switch" data-key="lyricsOnOpen" aria-checked="${!!s.lyricsOnOpen}" aria-label="Open lyrics automatically"></button></div>`;
  $('#controlRows').innerHTML = `
    <div class="row row-wide set-row"><span class="tile t-accel">${icon('gauge')}</span>
      <div class="row-main"><div class="row-title">Wheel acceleration</div><div class="row-sub">Long lists scroll faster when you keep spinning the wheel quickly.</div></div>
      <div class="row-control">${segHtml('wheelAcceleration', WHEEL, s.wheelAcceleration, 'Wheel acceleration')}</div></div>
    <div class="row row-wide set-row"><span class="tile t-vibrate">${icon('vibrate')}</span>
      <div class="row-main"><div class="row-title">Wheel vibration</div><div class="row-sub">A short buzz on every wheel step, like Rockbox. Normal matches Rockbox’s intensity 5.</div></div>
      <div class="row-control">${segHtml('wheelVibration', BUZZ, s.wheelVibration ?? 'normal', 'Wheel vibration')}</div></div>
    <div class="row set-row"><span class="tile t-click">${icon('volume')}</span>
      <div class="row-main"><div class="row-title">Click sound</div><div class="row-sub">The tick on every wheel step, separate from vibration.</div></div>
      <button class="switch" type="button" role="switch" data-key="clickSound" aria-checked="${s.clickSound !== false}" aria-label="Click sound"></button></div>`;
  setFill($('#brightness'));
  renderSubtitles();
}
// Subtitles: the same choices as Settings › Display › Subtitles on the Y1, by index
const SUB_OPACITY = [[25, '25%'], [50, '50%'], [75, '75%'], [100, '100%']];
const SUB_BACK_OPACITY = [[0, 'None'], [25, '25%'], [50, '50%'], [75, '75%'], [100, '100%']];
function subLineStyle(s) {
  const C = s.subChoices.colors;
  const rgba = (hex, pct) => { const n = parseInt(hex.slice(1), 16); return `rgba(${n >> 16 & 255},${n >> 8 & 255},${n & 255},${pct / 100})`; };
  const fg = C[s.subColor].hex, bg = C[s.subBack].hex;
  const edge = fg === '#000000' ? '255,255,255' : '0,0,0', light = edge === '0,0,0' ? '255,255,255' : '0,0,0';
  const a = Math.min(1, s.subOpacity / 100 + 0.1), la = s.subOpacity / 200;
  const ring = [];
  for (const x of [-1, 0, 1]) for (const y of [-1, 0, 1]) if (x || y) ring.push(`${x}px ${y}px 0 rgba(${edge},${a})`);
  const shadow = ['none', `2px 2px 3px rgba(${edge},${a})`, `-1px -1px 0 rgba(${light},${la}), 1px 1px 0 rgba(${edge},${a})`,
    `-1px -1px 0 rgba(${edge},${a}), 1px 1px 0 rgba(${light},${la})`, ring.join(', ')][s.subEdge] || 'none';
  return `color:${rgba(fg, s.subOpacity)};background:${s.subBackOpacity ? rgba(bg, s.subBackOpacity) : 'transparent'};font-size:${[14, 17, 21, 27][s.subSize]}px;text-shadow:${shadow}`;
}
function renderSubtitles() {
  const s = S.settings, el = $('#subtitleRows');
  if (!el || !s || !s.subChoices) return;
  const ch = s.subChoices;
  const sel = (key, opts, cur, label) => `<div class="select"><select data-key="${key}" aria-label="${esc(label)}">${opts.map(([v, l]) => `<option value="${esc(String(v))}"${v === cur ? ' selected' : ''}>${esc(l)}</option>`).join('')}</select>${icon('down')}</div>`;
  const colors = ch.colors.map((c, i) => [i, c.name]);
  el.innerHTML = `
    <div class="sub-preview" aria-hidden="true"><span class="sub-line" id="subLine" style="${subLineStyle(s)}">Subtitles look like this</span></div>
    <div class="row set-row"><span class="tile t-subtitles">${icon('captions')}</span>
      <div class="row-main"><div class="row-title">Show subtitles</div><div class="row-sub">When a video has them: in the language below, the video’s own track before the speech recognition’s.</div></div>
      <button class="switch" type="button" role="switch" data-key="subtitles" aria-checked="${!!s.subtitles}" aria-label="Show subtitles"></button></div>
    <div class="row set-row"><div class="row-main"><div class="row-title">Language</div><div class="row-sub">The one looked for first.</div></div>
      <div class="row-control">${sel('subLang', ch.languages.map((l) => [l.code, l.name]), s.subLang || '', 'Language')}</div></div>
    <div class="row row-wide set-row"><div class="row-main"><div class="row-title">Size</div></div>
      <div class="row-control">${segHtml('subSize', ch.sizes.map((n, i) => [i, n]), s.subSize, 'Size')}</div></div>
    <div class="row set-row"><div class="row-main"><div class="row-title">Text color</div></div>
      <div class="row-control">${sel('subColor', colors, s.subColor, 'Text color')}</div></div>
    <div class="row row-wide set-row"><div class="row-main"><div class="row-title">Text opacity</div></div>
      <div class="row-control">${segHtml('subOpacity', SUB_OPACITY, s.subOpacity, 'Text opacity')}</div></div>
    <div class="row set-row"><div class="row-main"><div class="row-title">Background</div></div>
      <div class="row-control">${sel('subBack', colors, s.subBack, 'Background')}</div></div>
    <div class="row row-wide set-row"><div class="row-main"><div class="row-title">Background opacity</div></div>
      <div class="row-control">${segHtml('subBackOpacity', SUB_BACK_OPACITY, s.subBackOpacity, 'Background opacity')}</div></div>
    <div class="row set-row"><div class="row-main"><div class="row-title">Edges</div></div>
      <div class="row-control">${sel('subEdge', ch.edges.map((n, i) => [i, n]), s.subEdge, 'Edges')}</div></div>
    <div class="sub-actions"><button class="btn btn-sm" type="button" data-act="sub-reset">Use YouTube’s look</button></div>`;
}
const settingsRows = $('#view-settings');
settingsRows.addEventListener('click', (e) => {
  if (e.target.closest('[data-act="reload"]')) return loadSettings();
  if (e.target.closest('[data-act="sub-reset"]')) return saveSetting('subReset', true);
  const seg = e.target.closest('.seg[data-key]'), b = e.target.closest('[data-v]');
  if (seg && b) {
    $$('button', seg).forEach((x) => x.setAttribute('aria-checked', String(x === b)));
    return saveSetting(seg.dataset.key, JSON.parse(b.dataset.v));
  }
  const sw = e.target.closest('.switch[data-key]');
  if (sw) {
    const on = sw.getAttribute('aria-checked') !== 'true';
    sw.setAttribute('aria-checked', String(on));
    saveSetting(sw.dataset.key, on);
  }
});
let brightTimer = 0;
settingsRows.addEventListener('input', (e) => {
  if (e.target.id !== 'brightness') return;
  setFill(e.target);
  $('#brightnessOut').textContent = `${e.target.value}%`;
  clearTimeout(brightTimer);
  brightTimer = setTimeout(() => saveSetting('brightness', +e.target.value, true), 250);
});
settingsRows.addEventListener('change', (e) => {
  const sel = e.target.closest('select[data-key]');
  if (sel) return saveSetting(sel.dataset.key, sel.dataset.key === 'subLang' ? sel.value : +sel.value);
  if (e.target.id !== 'brightness') return;
  clearTimeout(brightTimer);
  saveSetting('brightness', +e.target.value);
});
async function saveSetting(key, value, quiet = false) {
  try {
    if (key.startsWith('sub') && S.settings) {  // the preview at once
      const now = { ...S.settings, [key]: value };
      const line = $('#subLine');
      if (line && key !== 'subReset') line.setAttribute('style', subLineStyle(now));
    }
    S.settings = await api('PUT', '/api/settings', { [key]: value });
    if (key.startsWith('sub')) renderSubtitles();
    if (!quiet) toast('Saved on your Rondo');
  } catch (e) {
    if (e.status === 401) return;
    toast(`Not saved. ${reason(e)}`, 'err');
    if (S.settings) renderSettings();
  }
}

// ---------- FM radio (the Rondo's radio: player/FmRadio.kt) ----------
// Refreshes every 1.5 s while the page is open (station names and signal change on their own).
// The page is rebuilt only when its shape changes (power, station, lists, settings); signal, names
// and mute are updated in place, so a click is never lost to a rebuild under the pointer.
let radioTimer = 0;
const radioShape = (r) => JSON.stringify([r.headphones, r.error, r.on, r.starting, r.seeking, r.scanning, r.frequency, r.band, r.speaker, r.presets, r.found]);
function radioState(r) {
  const busy = r.starting ? 'Turning on…' : r.scanning ? 'Scanning…' : r.seeking ? 'Searching…' : '';
  return !r.on ? (busy || 'Off') : [busy || (r.muted ? 'Muted' : r.stereo ? 'Stereo' : 'Mono'), r.speaker ? 'Speaker' : 'Headphones'].join(' · ');
}
function updateHero(r) {
  const set = (id, t) => { const el = document.getElementById(id); if (el && el.textContent !== t) el.textContent = t; };
  set('fmStateText', radioState(r));
  set('fmName', r.name || '');
  set('fmText', r.text || '');
  set('fmMute', r.muted ? 'Unmute' : 'Mute');
  $$('#fmBars i').forEach((b, i) => b.classList.toggle('on', i < r.signal));
  // the volume follows the wheel on the Rondo, unless it is being dragged here
  const vol = document.getElementById('fmVol');
  if (vol && !radioDragging && +vol.value !== r.volume) {
    vol.max = r.maxVolume;
    vol.value = r.volume;
    setFill(vol);
    set('fmVolOut', String(r.volume));
  }
}
let radioDragging = false, fmVolTimer = 0;
const fmName = (f) => `${Math.floor(f / 10)}.${f % 10}`;
async function loadRadio(quiet = false) {
  clearTimeout(radioTimer);
  const body = $('#radioBody');
  if (!S.radio && !quiet) body.innerHTML = '<div class="group"><div class="lib-skel"></div><div class="lib-skel"></div></div>';
  try {
    const prev = S.radio;
    S.radio = await api('GET', '/api/radio');
    if (!prev || !$('.fm-hero', body) || radioShape(prev) !== radioShape(S.radio)) renderRadio();
    else updateHero(S.radio);
  } catch (e) {
    if (e.status !== 401 && !S.radio) body.innerHTML = `<div class="group"><div class="row-empty">Couldn’t load the radio. ${esc(reason(e))}</div></div>`;
  }
  if (S.route === 'radio' && !document.hidden) radioTimer = setTimeout(() => loadRadio(true), 1500);
}
function fmHero(r) {
  const bars = `<span class="fm-bars" id="fmBars" aria-hidden="true">${[1, 2, 3, 4].map((i) => `<i class="${i <= r.signal ? 'on' : ''}" style="height:${i * 3}px"></i>`).join('')}</span>`;
  const state = radioState(r);
  const saved = (r.presets || []).some((p) => p.frequency === r.frequency);
  const step = (d) => `<button class="btn btn-sm" type="button" data-fm="step" data-d="${d}"${r.on ? '' : ' disabled'}>${d > 0 ? '+' : '−'}0.1</button>`;
  const labels = [];
  for (let f = Math.ceil(r.min / 40) * 40; f <= r.max; f += (r.band === 'japan' ? 20 : 40)) labels.push(`<span>${f / 10}</span>`);
  return `<div class="fm-hero${r.on ? '' : ' is-off'}">
      <div class="fm-top"><span class="fm-state">${r.on ? bars : ''}<span id="fmStateText">${esc(state)}</span></span>
        <button class="switch" type="button" role="switch" data-fm="power" aria-checked="${r.on || r.starting}" aria-label="Radio"${r.starting ? ' disabled' : ''}></button></div>
      <div class="fm-freq" id="fmFreq">${fmName(r.frequency)}<small>MHz</small></div>
      <div class="fm-name" id="fmName">${esc(r.name || '')}</div>
      <div class="fm-text" id="fmText">${esc(r.text || '')}</div>
      <input class="fm-dial" type="range" id="fmDial" min="${r.min}" max="${r.max}" step="1" value="${r.frequency}" aria-label="Frequency"${r.on ? '' : ' disabled'}>
      <div class="fm-scale">${labels.join('')}</div>
      <div class="fm-controls">
        <button class="ib" type="button" data-fm="seek" data-up="0" aria-label="Previous station"${r.on ? '' : ' disabled'}>${icon('prev')}</button>
        ${step(-1)}
        <form class="fm-tune" id="fmTune"><input type="text" inputmode="decimal" id="fmTuneIn" value="${fmName(r.frequency)}" aria-label="Frequency in MHz"${r.on ? '' : ' disabled'}><button class="btn btn-sm" type="submit"${r.on ? '' : ' disabled'}>Tune</button></form>
        ${step(1)}
        <button class="ib" type="button" data-fm="seek" data-up="1" aria-label="Next station"${r.on ? '' : ' disabled'}>${icon('next')}</button>
      </div>
      <div class="fm-volume">${icon('volume')}<input type="range" id="fmVol" min="0" max="${r.maxVolume}" step="1" value="${r.volume}" aria-label="Radio volume"><output id="fmVolOut">${r.volume}</output></div>
      <div class="fm-controls">
        <button class="btn btn-sm" type="button" id="fmMute" data-fm="mute"${r.on ? '' : ' disabled'}>${r.muted ? 'Unmute' : 'Mute'}</button>
        <button class="btn btn-sm" type="button" data-fm="save"${saved ? ' disabled' : ''}>${saved ? 'Saved' : 'Save this station'}</button>
      </div>
    </div>`;
}
function renderRadio() {
  const r = S.radio, body = $('#radioBody');
  const editing = body.contains(document.activeElement) && document.activeElement.tagName === 'INPUT' && document.activeElement.type !== 'range';
  const notes = [];
  if (!r.headphones) notes.push(`<div class="note">${icon('alert')}<span>Plug wired headphones into your Rondo: their cable is the radio’s antenna.</span></div>`);
  else if (r.error === 'chip') notes.push(`<div class="note err">${icon('alert')}<span>The radio didn’t start. Turn it on again.</span></div>`);
  if (editing && $('.fm-hero', body)) {
    // keep the fields being typed in: only the top part follows the radio
    const hero = $('.fm-hero', body);
    hero.outerHTML = fmHero(r);
    setFill($('#fmDial'));
    return;
  }
  const presets = (r.presets || []).map((p, i, all) => `<div class="row fm-preset" data-f="${p.frequency}">
      <div class="row-main"><button class="btn btn-sm btn-quiet fm-row-freq" type="button" data-fm="play" data-f="${p.frequency}">${fmName(p.frequency)}</button>
        <input type="text" class="fm-name-input" data-fm="name" data-f="${p.frequency}" value="${esc(p.name)}" placeholder="Name" maxlength="40" aria-label="Name for ${fmName(p.frequency)} MHz">
        ${p.frequency === r.frequency && r.on ? '<span class="fm-playing">Playing</span>' : ''}</div>
      <div class="bt-actions">
        <button class="ib ib-sm" type="button" data-fm="move" data-i="${i}" data-d="-1" aria-label="Move up"${i === 0 ? ' disabled' : ''}>${icon('up')}</button>
        <button class="ib ib-sm" type="button" data-fm="move" data-i="${i}" data-d="1" aria-label="Move down"${i === all.length - 1 ? ' disabled' : ''}>${icon('down')}</button>
        <button class="ib ib-sm danger" type="button" data-fm="remove" data-i="${i}" aria-label="Remove ${fmName(p.frequency)}">${icon('trash')}</button>
      </div></div>`).join('') || '<div class="row-empty">No saved stations yet. Tune one in and press Save this station.</div>';
  const found = (r.found || []).map((s) => `<div class="row">
      <div class="row-main"><div class="row-title">${fmName(s.frequency)} MHz${s.frequency === r.frequency && r.on ? ' <span class="fm-playing">Playing</span>' : ''}</div>${s.name ? `<div class="row-sub">${esc(s.name)}</div>` : ''}</div>
      <div class="bt-actions"><button class="btn btn-sm" type="button" data-fm="play" data-f="${s.frequency}"${r.on ? '' : ' disabled'}>Play</button>
        <button class="btn btn-sm btn-quiet" type="button" data-fm="add" data-f="${s.frequency}">Save</button></div></div>`).join('')
    || `<div class="row-empty">${r.scanning ? 'Scanning… this takes about 20 seconds.' : 'Scan to list every station your Rondo can receive.'}</div>`;
  const segs = (key, options, cur) => `<div class="seg" role="radiogroup" data-fm="${key}">${options.map(([v, l]) => `<button type="button" role="radio" aria-checked="${v === cur}" data-v="${v}">${l}</button>`).join('')}</div>`;
  body.innerHTML = `${notes.join('')}${fmHero(r)}
    <h2 class="gh">Saved stations</h2><div class="group">${presets}</div>
    <h2 class="gh" style="display:flex;align-items:center;justify-content:space-between">Found <button class="btn btn-sm" type="button" data-fm="scan"${r.on && !r.scanning ? '' : ' disabled'}>${r.scanning ? 'Scanning…' : 'Scan'}</button></h2>
    <div class="group">${found}</div>
    <h2 class="gh">Output and region</h2>
    <div class="group">
      <div class="row row-wide set-row"><div class="row-main"><div class="row-title">Output</div><div class="row-sub">The speaker plays out loud; the headphones stay plugged in as the antenna.</div></div>
        <div class="row-control">${segs('speaker', [['0', 'Headphones'], ['1', 'Speaker']], r.speaker ? '1' : '0')}</div></div>
      <div class="row row-wide set-row"><div class="row-main"><div class="row-title">Region</div><div class="row-sub">Japan uses 76–95 MHz; most of the world 87.5–108 MHz.</div></div>
        <div class="row-control">${segs('band', [['world', 'Most of the world'], ['japan', 'Japan']], r.band)}</div></div>
    </div>`;
  setFill($('#fmDial'));
  setFill($('#fmVol'));
}
async function radioPut(path, body, method = 'PUT') {
  try {
    S.radio = await api(method, `/api/radio${path}`, body);
    renderRadio();
  } catch (e) {
    if (e.status !== 401) toast(`Didn’t work. ${reason(e)}`, 'err');
  }
}
const savePresets = (presets) => radioPut('/presets', { presets });
$('#radioBody').addEventListener('click', (e) => {
  const b = e.target.closest('[data-fm]');
  if (!b || b.disabled || b.tagName === 'INPUT') return;
  const r = S.radio, act = b.dataset.fm;
  if (act === 'power') return radioPut('', { on: !(r.on || r.starting) });
  if (act === 'seek') return radioPut('/seek', { up: b.dataset.up === '1' }, 'POST');
  if (act === 'step') return radioPut('', { frequency: Math.min(r.max, Math.max(r.min, r.frequency + +b.dataset.d)) });
  if (act === 'mute') return radioPut('', { muted: !r.muted });
  if (act === 'scan') return radioPut('/scan', undefined, 'POST');
  if (act === 'play') return radioPut('', r.on ? { frequency: +b.dataset.f } : { frequency: +b.dataset.f, on: true });
  if (act === 'save') return savePresets([...r.presets, { frequency: r.frequency, name: r.name || '' }].sort((a, x) => a.frequency - x.frequency));
  if (act === 'add') {
    const f = +b.dataset.f, heard = r.found.find((s) => s.frequency === f)?.name || '';
    return savePresets([...r.presets, { frequency: f, name: heard }]);
  }
  if (act === 'remove') return savePresets(r.presets.filter((_, i) => i !== +b.dataset.i));
  if (act === 'move') {
    const list = [...r.presets], i = +b.dataset.i, j = i + +b.dataset.d;
    [list[i], list[j]] = [list[j], list[i]];
    return savePresets(list);
  }
  const seg = e.target.closest('.seg[data-fm] [data-v]');
  if (seg) {
    const key = seg.closest('.seg').dataset.fm;
    return radioPut('', key === 'speaker' ? { speaker: seg.dataset.v === '1' } : { band: seg.dataset.v });
  }
});
$('#radioBody').addEventListener('input', (e) => {
  if (e.target.id === 'fmVol') {
    radioDragging = true;
    setFill(e.target);
    $('#fmVolOut').textContent = e.target.value;
    clearTimeout(fmVolTimer);
    fmVolTimer = setTimeout(() => api('PUT', '/api/radio', { volume: +e.target.value }).catch(() => {}), 150);
    return;
  }
  if (e.target.id !== 'fmDial') return;
  setFill(e.target);
  $('#fmFreq').firstChild.textContent = fmName(+e.target.value);
});
$('#radioBody').addEventListener('change', (e) => {
  if (e.target.id === 'fmDial') return radioPut('', { frequency: +e.target.value });
  if (e.target.id === 'fmVol') {
    clearTimeout(fmVolTimer);
    radioDragging = false;
    return radioPut('', { volume: +e.target.value });
  }
  if (e.target.dataset.fm === 'name') {
    const f = +e.target.dataset.f;
    savePresets(S.radio.presets.map((p) => (p.frequency === f ? { ...p, name: e.target.value.trim() } : p)));
  }
});
$('#radioBody').addEventListener('submit', (e) => {
  if (e.target.id !== 'fmTune') return;
  e.preventDefault();
  const mhz = parseFloat($('#fmTuneIn').value.replace(',', '.'));
  const f = Math.round(mhz * 10), r = S.radio;
  if (!(f >= r.min && f <= r.max)) return toast(`Pick a frequency between ${fmName(r.min)} and ${fmName(r.max)} MHz.`, 'err');
  $('#fmTuneIn').blur();
  radioPut('', { frequency: f });
});

// ---------- Bluetooth (the Rondo's Bluetooth screen: ui/screens/BluetoothScreens.kt) ----------
// The Rondo searches while this page is open (every refresh keeps it going, 30 s without one stops it).
let btTimer = 0;
async function loadBluetooth(quiet = false) {
  clearTimeout(btTimer);
  const body = $('#btBody');
  if (!S.bt && !quiet) body.innerHTML = '<div class="group"><div class="lib-skel"></div><div class="lib-skel"></div></div>';
  try {
    S.bt = await api('GET', '/api/bluetooth');
    renderBluetooth();
  } catch (e) {
    if (e.status !== 401 && !S.bt) body.innerHTML = `<div class="group"><div class="row-empty">Couldn’t load Bluetooth. ${esc(reason(e))}</div></div>`;
  }
  if (S.route === 'bluetooth' && !document.hidden) btTimer = setTimeout(() => loadBluetooth(true), 2000);
}
function btDeviceRow(d) {
  const connected = d.status === 'connected', connecting = d.status === 'connecting';
  const sub = connected ? 'Music plays here' : connecting ? 'Connecting…' : d.audio ? `${d.kind} · not connected` : d.kind;
  const main = connecting ? `<span class="joining">${spinner()}Connecting…</span>`
    : connected ? `<button class="btn btn-sm" type="button" data-bt="disconnect" data-addr="${esc(d.address)}">Disconnect</button>`
    : `<button class="btn btn-sm btn-primary" type="button" data-bt="connect" data-addr="${esc(d.address)}">Connect</button>`;
  return `<div class="row"><div class="row-main"><div class="row-title">${esc(d.name)}${connected ? '<span class="tag ok">Connected</span>' : ''}</div><div class="row-sub">${esc(sub)}</div></div>
    <div class="bt-actions">${main}<button class="btn btn-sm btn-red-text" type="button" data-bt="forget" data-addr="${esc(d.address)}" data-name="${esc(d.name)}">Forget</button></div></div>`;
}
function btOtherRow(d) {
  const action = d.pairing ? `<span class="joining">${spinner()}Pairing…</span>`
    : `<button class="btn btn-sm" type="button" data-bt="connect" data-addr="${esc(d.address)}">Pair</button>`;
  return `<div class="row"><div class="row-main"><div class="row-title">${esc(d.name)}</div><div class="row-sub">${esc(d.kind)}</div></div><div class="bt-actions">${action}</div></div>`;
}
function renderBluetooth() {
  const b = S.bt, body = $('#btBody');
  $('#btSearch').hidden = !(b.available && b.state === 'on');
  if (!b.available) {
    body.innerHTML = `<div class="note">${icon('alert')}<span>This Rondo has no Bluetooth.</span></div>`;
    return;
  }
  const on = b.state === 'on', turning = b.state.startsWith('turning');
  const stateText = { on: 'On', off: 'Off', turning_on: 'Turning on…', turning_off: 'Turning off…' }[b.state] || '';
  const power = `<div class="group"><div class="row wifi-current"><span class="tile t-bluetooth">${icon('bluetooth')}</span>
      <div class="row-main"><div class="row-title">Bluetooth</div><div class="row-sub">${stateText}</div></div>
      <button class="switch" type="button" role="switch" data-bt="power" aria-checked="${on || b.state === 'turning_on'}" aria-label="Bluetooth"${turning ? ' disabled' : ''}></button></div></div>`;
  if (!on) {
    body.innerHTML = power;
    return;
  }
  const mine = (b.mine || []).map(btDeviceRow).join('') || '<div class="row-empty">No paired devices yet.</div>';
  const others = (b.others || []).map(btOtherRow).join('')
    || `<div class="row-empty">${b.searching ? 'Looking for devices…' : 'No devices found.'} Put your headphones in pairing mode.</div>`;
  body.innerHTML = `${power}
    <h2 class="gh">My devices</h2><div class="group">${mine}</div>
    <h2 class="gh">Other devices${b.searching ? ` <span class="joining">${spinner()}Searching…</span>` : ''}</h2><div class="group">${others}</div>
    <div class="note info">${icon('info')}<span>Headphones that ask for a PIN get 0000. One device plays at a time: connecting one disconnects the other.</span></div>`;
}
$('#btSearch').addEventListener('click', async () => {
  try { S.bt = await api('POST', '/api/bluetooth/search'); renderBluetooth(); } catch (e) { if (e.status !== 401) toast(`Couldn’t search. ${reason(e)}`, 'err'); }
});
$('#btBody').addEventListener('click', async (e) => {
  const b = e.target.closest('[data-bt]');
  if (!b || b.disabled) return;
  const act = b.dataset.bt;
  try {
    if (act === 'power') {
      const on = b.getAttribute('aria-checked') !== 'true';
      b.setAttribute('aria-checked', String(on));
      S.bt = await api('PUT', '/api/bluetooth', { enabled: on });
    } else {
      if (act === 'forget' && !(await confirmSheet({ title: `Forget “${b.dataset.name}”?`, text: 'Your Rondo stops connecting to it. Pair it again to use it.', action: 'Forget' }))) return;
      S.bt = await api('POST', `/api/bluetooth/${act}`, { address: b.dataset.addr });
    }
    renderBluetooth();
  } catch (err) {
    if (err.status !== 401) toast(`Didn’t work. ${reason(err)}`, 'err');
  }
});

// ---------- Wi-Fi ----------
let wifiOpen = null, joinTimer = 0;
async function loadWifi(quiet = false) {
  const body = $('#wifiBody');
  if (!S.wifi && !quiet) body.innerHTML = '<div class="group"><div class="lib-skel"></div><div class="lib-skel"></div></div>';
  const btn = $('#wifiRefresh');
  btn.classList.add('is-busy');
  try {
    S.wifi = await api('GET', '/api/wifi');
    renderWifi();
  } catch (e) {
    if (e.status !== 401 && !S.wifi) body.innerHTML = `<div class="group"><div class="row-empty">Couldn’t load Wi-Fi. ${esc(reason(e))}</div></div>`;
  }
  btn.classList.remove('is-busy');
}
$('#wifiRefresh').addEventListener('click', () => loadWifi(true));
function renderWifi() {
  const w = S.wifi, body = $('#wifiBody');
  if (!w.enabled) {
    body.innerHTML = `<div class="note">${icon('alert')}<span>Wi-Fi is off on your Rondo. Turn it on under Settings › Wi-Fi on the device.</span></div>`;
    return;
  }
  const cur = w.current;
  const current = `<div class="group"><div class="row wifi-current">
      <span class="tile t-wifi">${icon('wifi')}</span>
      <div class="row-main"><div class="row-title">${cur ? esc(cur.ssid) : 'Not connected'}</div><div class="row-sub">${cur ? `Connected · ${esc(cur.ip || '')}` : 'Pick a network below.'}</div></div>
      ${cur ? `<span class="row-value" aria-label="Signal ${cur.signal} of 4">${sigSvg(cur.signal ?? 0)}</span>` : ''}
    </div></div>`;
  const saved = w.saved.length
    ? w.saved.map((s) => `<div class="row row-flat"><div class="row-main"><div class="row-title">${esc(s.ssid)}${cur?.ssid === s.ssid ? '<span class="tag ok">Connected</span>' : ''}</div></div>
        <button class="btn btn-sm btn-red-text" type="button" data-forget="${s.id}">Forget</button></div>`).join('')
    : '<div class="row-empty">No saved networks.</div>';
  const nearby = [...w.nearby].sort((a, b) => (b.ssid === cur?.ssid) - (a.ssid === cur?.ssid) || b.signal - a.signal || collator.compare(a.ssid, b.ssid));
  const near = nearby.length ? nearby.map((n) => netRow(n, cur)).join('') : '<div class="row-empty">No networks found. Try Refresh.</div>';
  body.innerHTML = `${current}<h2 class="gh">Saved networks</h2><div class="group">${saved}</div><h2 class="gh">Nearby</h2><div class="group" id="nearby">${near}</div>`;
  if (wifiOpen) $('#nearby .pw input')?.focus();
}
function netRow(n, cur) {
  const connected = cur?.ssid === n.ssid;
  const open = wifiOpen === n.ssid;
  const joining = S.joining?.ssid === n.ssid;
  const secured = n.security !== 'open';
  const label = `${n.ssid}${secured ? ', secured' : ''}, signal ${n.signal} of 4`;
  const row = `<button class="row row-flat net" type="button" data-ssid="${esc(n.ssid)}" aria-expanded="${open}" aria-label="${esc(label)}"${connected ? ' disabled' : ''}>
      <span class="ico">${secured ? icon('lock') : ''}</span>
      <span class="row-main"><span class="row-title" style="display:block">${esc(n.ssid)}${connected ? '<span class="tag ok">Connected</span>' : n.saved ? '<span class="tag">Saved</span>' : ''}</span></span>
      ${joining ? `<span class="joining">${spinner()}Joining…</span>` : ''}
      ${sigSvg(n.signal)}
    </button>`;
  if (!open) return row;
  if (n.security === 'eap') return row + `<div class="join"><div class="note">${icon('info')}<span>Rondo can’t join enterprise networks like this one yet.</span></div></div>`;
  const needPw = secured && !n.saved;
  const warn = cur && !connected
    ? `<div class="note">${icon('alert')}<span>Your Rondo will leave “${esc(cur.ssid)}”. If this phone or computer isn’t on “${esc(n.ssid)}” too, this page will lose its connection. The Rondo shows its new address under Settings › Dashboard.</span></div>` : '';
  return row + `<form class="join" data-join="${esc(n.ssid)}">
      ${needPw ? `<div><label class="field-label" for="wifiPw">Password for “${esc(n.ssid)}”</label><div class="pw"><input type="password" id="wifiPw" autocomplete="off" autocapitalize="off" spellcheck="false" minlength="${n.security === 'psk' ? 8 : 1}" required><button class="btn btn-sm btn-quiet" type="button" data-act="reveal">Show</button></div></div>` : ''}
      ${warn}
      <p class="field-err" id="joinErr" hidden></p>
      <div class="join-actions"><button class="btn" type="button" data-act="cancel">Cancel</button><button class="btn btn-primary" type="submit">Join</button></div>
    </form>`;
}
$('#wifiBody').addEventListener('click', async (e) => {
  const forget = e.target.closest('[data-forget]');
  if (forget) return forgetNet(+forget.dataset.forget);
  if (e.target.closest('[data-act="reveal"]')) {
    const input = $('#wifiPw'), b = e.target.closest('[data-act="reveal"]');
    input.type = input.type === 'password' ? 'text' : 'password';
    b.textContent = input.type === 'password' ? 'Show' : 'Hide';
    return input.focus();
  }
  if (e.target.closest('[data-act="cancel"]')) { wifiOpen = null; return renderWifi(); }
  const net = e.target.closest('.net');
  if (net) { wifiOpen = wifiOpen === net.dataset.ssid ? null : net.dataset.ssid; renderWifi(); }
});
$('#wifiBody').addEventListener('submit', async (e) => {
  e.preventDefault();
  const form = e.target.closest('[data-join]');
  const ssid = form.dataset.join;
  const pw = $('#wifiPw', form);
  const err = $('#joinErr', form);
  if (pw && pw.value.length < (+pw.minLength || 1)) {
    err.hidden = false;
    err.textContent = pw.minLength > 1 ? 'Wi-Fi passwords are at least 8 characters.' : 'Type the password.';
    pw.classList.add('is-invalid');
    return pw.focus();
  }
  const submit = $('[type="submit"]', form);
  submit.disabled = true;
  submit.textContent = 'Joining…';
  try {
    await api('POST', '/api/wifi/join', pw ? { ssid, password: pw.value } : { ssid });
  } catch (e2) {
    if (e2.status === 401) return;
    submit.disabled = false;
    submit.textContent = 'Join';
    err.hidden = false;
    err.textContent = `Couldn’t join. ${reason(e2)}`;
    return;
  }
  S.joining = { ssid, until: Date.now() + 30000 };
  wifiOpen = null;
  renderWifi();
  watchJoin();
});
function watchJoin() {
  clearTimeout(joinTimer);
  joinTimer = setTimeout(async () => {
    const j = S.joining;
    if (!j) return;
    try {
      S.wifi = await api('GET', '/api/wifi');
      if (S.wifi.current?.ssid === j.ssid) {
        S.joining = null;
        toast(`Connected to “${j.ssid}”`);
        poll();
      } else if (S.wifi.lastError === 'auth' && S.wifi.joining === j.ssid) {
        // the Rondo reports the network refused the password: say so at once and reopen the form
        S.joining = null;
        wifiOpen = j.ssid;
        toast(`Wrong password for “${j.ssid}”. Try again.`, 'err');
      } else if (Date.now() > j.until) {
        S.joining = null;
        toast(`Couldn’t join “${j.ssid}”. Check the password and try again.`, 'err');
      }
    } catch { /* the banner explains a dropped connection */ }
    if (S.route === 'wifi' && S.wifi) renderWifi();
    if (S.joining) watchJoin();
  }, 2000);
}
async function forgetNet(id) {
  const net = S.wifi.saved.find((s) => s.id === id);
  if (!net) return;
  const isCurrent = S.wifi.current?.ssid === net.ssid;
  const ok = await confirmSheet({
    title: `Forget “${net.ssid}”?`,
    text: `Your Rondo won’t join it on its own any more.${isCurrent ? ' It’s the network your Rondo is on now, so this page will disconnect.' : ''}`,
    action: 'Forget',
  });
  if (!ok) return;
  try {
    await api('DELETE', `/api/wifi/saved?id=${encodeURIComponent(id)}`);
    toast(`Forgot “${net.ssid}”`);
    loadWifi(true);
  } catch (e) { if (e.status !== 401) toast(`Couldn’t forget it. ${reason(e)}`, 'err'); }
}

// ---------- YouTube › Download ----------
// The form (a link or an id, the height), then what's on its way (progress, cancel), then the videos on
// the card (delete). Asked again every 2 s while one is on its way and the page is open.
let dlTimer = 0;
const DL_QUALITY = [[720, '720p'], [480, '480p'], [360, '360p']];
async function loadDownloads() {
  clearTimeout(dlTimer);
  try {
    S.downloads = await api('GET', '/api/downloads');
    renderDownloads();
  } catch (e) {
    if (e.status !== 401 && !S.downloads) $('#dlBody').innerHTML = `<div class="group"><div class="row-empty">Couldn’t load the downloads. ${esc(reason(e))}</div></div>`;
  }
  if (S.route === 'server' && S.downloads?.jobs.some((j) => j.state === 'waiting' || j.state === 'running')) dlTimer = setTimeout(loadDownloads, 2000);
}
function renderDownloads() {
  const d = S.downloads, el = $('#dlBody');
  if (!el || !d) return;
  if (!$('#dlForm')) {
    el.innerHTML = `<div class="group"><form class="dl-form" id="dlForm">
        <input type="text" id="dlVideo" placeholder="A YouTube link or video id" aria-label="YouTube link or video id" autocomplete="off" spellcheck="false">
        ${segHtml('dlQuality', DL_QUALITY, S.dlQuality || 480, 'Quality').replace('seg seg-wide', 'seg')}
        <button class="btn btn-primary" type="submit">${icon('download')}<span>Download</span></button></form></div>
      <div class="group" id="dlRows"></div>`;
  }
  const jobs = d.jobs.filter((j) => j.state !== 'done').reverse();
  const rows = [
    ...jobs.map((j) => {
      const going = j.state === 'running' || j.state === 'waiting';
      const sub = j.state === 'running' ? `${esc(j.stage || 'Downloading')} · ${j.percent}% · ${j.quality}p` : j.state === 'waiting' ? `Waiting · ${j.quality}p` : `Didn’t download: ${esc(j.error)}`;
      return `<div class="row dl-row"><span class="tile t-downloads">${icon('download')}</span>
        <div class="row-main"><div class="row-title">${esc(j.title)}</div><div class="row-sub">${sub}</div>
          ${j.state === 'running' ? `<div class="dl-bar"><span style="width:${j.percent}%"></span></div>` : ''}</div>
        ${going ? `<button class="btn btn-sm" type="button" data-dl-cancel="${esc(j.id)}">Cancel</button>`
          : `<button class="btn btn-sm" type="button" data-dl-again="${esc(j.id)}" data-q="${j.quality}">Try again</button>`}</div>`;
    }),
    ...d.files.map((x) => `<div class="row dl-row"><span class="tile t-videos">${icon('film')}</span>
        <div class="row-main"><div class="row-title">${esc(x.title)}</div><div class="row-sub">${esc(x.subtitle)}</div></div>
        <a class="btn btn-sm" href="/api/files/download?path=${encodeURIComponent(x.file)}" download>Save</a>
        <button class="btn btn-sm btn-red-text" type="button" data-dl-delete="${esc(x.file)}" aria-label="Delete ${esc(x.title)}">${icon('trash')}</button></div>`),
  ];
  $('#dlRows').innerHTML = rows.length ? rows.join('') : '<div class="row-empty">Nothing downloaded yet.</div>';
}
$('#dlBody').addEventListener('click', async (e) => {
  const seg = e.target.closest('.seg[data-key="dlQuality"]'), b = e.target.closest('[data-v]');
  if (seg && b) {
    $$('button', seg).forEach((x) => x.setAttribute('aria-checked', String(x === b)));
    S.dlQuality = JSON.parse(b.dataset.v);
    return;
  }
  const cancel = e.target.closest('[data-dl-cancel]'), again = e.target.closest('[data-dl-again]'), del = e.target.closest('[data-dl-delete]');
  try {
    if (cancel) S.downloads = await api('POST', '/api/downloads/cancel', { id: cancel.dataset.dlCancel });
    else if (again) S.downloads = await api('POST', '/api/downloads', { video: again.dataset.dlAgain, quality: +again.dataset.q }, { timeout: 40000 });
    else if (del) {
      const name = del.closest('.row').querySelector('.row-title').textContent;
      if (!await confirmSheet({ title: `Delete “${name}”?`, text: 'It goes from the card with its subtitles.', action: 'Delete' })) return;
      S.downloads = await api('DELETE', `/api/downloads?file=${encodeURIComponent(del.dataset.dlDelete)}`);
      toast('Deleted from the card');
    } else return;
    renderDownloads();
    loadDownloads();
  } catch (err) { if (err.status !== 401) toast(reason(err), 'err'); }
});
$('#dlBody').addEventListener('submit', async (e) => {
  e.preventDefault();
  const input = $('#dlVideo'), video = input.value.trim();
  if (!video) return input.focus();
  try {
    S.downloads = await api('POST', '/api/downloads', { video, quality: S.dlQuality || 480 }, { timeout: 40000 });
    input.value = '';
    toast('Downloading on your Rondo');
    renderDownloads();
    loadDownloads();
  } catch (err) { if (err.status !== 401) toast(reason(err), 'err'); }
});

// ---------- music server ----------
async function loadServer() {
  if (!S.server) $('#serverBody').innerHTML = '<div class="group"><div class="lib-skel"></div><div class="lib-skel"></div></div>';
  try {
    S.server = await api('GET', '/api/server');
    renderServer();
  } catch (e) {
    if (e.status !== 401 && !S.server) $('#serverBody').innerHTML = `<div class="group"><div class="row-empty">Couldn’t load the server settings. ${esc(reason(e))}</div></div>`;
  }
}
// a plain-http address (a server at home reached by IP) is fine on your own Wi-Fi only
const insecure = (url) => /^http:\/\//i.test(url || '');
// direct mode's YouTube account, as on the server's admin page: the code sign-in (the TV app's, like a
// smart TV) for the YouTube feeds and YouTube Music's playlists, and cookies.txt for YouTube Music's own
// home and history; the Rondo signs its own requests with them
async function loadYtAccount(quiet = true) {
  const [acct, tv] = await Promise.all([
    api('GET', '/api/youtube/account', undefined, { timeout: 30000 }).catch((e) => (e.status === 401 ? null : { mode: 'direct', signedIn: false, unreachable: true })),
    api('GET', '/api/youtube/tv', undefined, { timeout: 30000 }).catch((e) => (e.status === 401 ? null : { mode: 'direct', signedIn: false, unreachable: true })),
  ]);
  if (acct) S.ytAccount = acct;
  if (tv) S.ytTv = tv;
  if (S.server) renderAccount();
  watchTv();
}
// while a code waits for its approval: asked every 3 s, the countdown every second
let tvTimer = 0, tvTick = 0;
function watchTv() {
  clearTimeout(tvTimer);
  clearInterval(tvTick);
  if (!S.ytTv?.pending) return;
  tvTick = setInterval(() => {
    const cd = $('#tvCountdown'), p = S.ytTv?.pending;
    if (!cd || !p) return clearInterval(tvTick);
    const left = Math.max(0, Math.ceil((p.expiresAt - Date.now()) / 1000));
    cd.textContent = left ? `Expires in ${Math.floor(left / 60)}:${String(left % 60).padStart(2, '0')}` : 'This code expired. Get a new one.';
  }, 1000);
  tvTimer = setTimeout(async () => {
    if (S.route !== 'server') return;
    try {
      const was = S.ytTv;
      S.ytTv = await api('GET', '/api/youtube/tv', undefined, { timeout: 30000 });
      if (!was?.signedIn && S.ytTv.signedIn) toast(`Signed in${S.ytTv.name ? ` as ${S.ytTv.name}` : ''}`);
      if (S.ytTv.error && !S.ytTv.pending && was?.pending) toast(TV_ERRORS[S.ytTv.error] || 'The sign-in didn’t work. Try again.', 'err');
      renderAccount();
    } catch (e) { if (e.status === 401) return; }
    watchTv();
  }, 3000);
}
const TV_ERRORS = {
  denied: 'The sign-in was refused at Google.',
  expired: 'The code expired before it was entered. Get a new one.',
  revoked: 'The sign-in was removed from the Google account. Sign in again.',
  failed: 'YouTube didn’t give a code. Check the Rondo’s Wi-Fi and try again.',
};
async function tvAct(action) {
  try {
    S.ytTv = await api('POST', `/api/youtube/tv/${action}`, {}, { timeout: 45000 });
    renderAccount();
    watchTv();
    if (action === 'signout') toast('Signed out on your Rondo');
  } catch (e) { if (e.status !== 401) toast(reason(e), 'err'); }
}
function tvHtml() {
  const t = S.ytTv;
  if (!t) return `<div class="row set-row"><span class="tile t-account">${icon('key')}</span><div class="row-main"><div class="row-title">Sign in with a code</div><div class="row-sub">Checking…</div></div></div>`;
  if (t.pending) {
    return `<div class="row row-wide set-row tv-pending"><span class="tile t-account">${icon('key')}</span>
      <div class="row-main"><div class="row-title">Enter this code</div>
        <div class="tv-code" aria-label="The code">${esc(t.pending.code)}</div>
        <ol class="steps tv-steps">
          <li>On your phone or computer, open <a class="link" href="${esc(t.pending.url)}" target="_blank" rel="noopener noreferrer"><b>${esc(t.pending.url.replace(/^https?:\/\//, ''))}</b></a>.</li>
          <li>Enter the code and pick the Google account for your Rondo.</li>
          <li>Allow access. This page updates by itself.</li>
        </ol>
        <div class="row-sub"><span id="tvCountdown"></span> · Waiting for Google…</div></div>
      <div class="row-control acct-actions"><button class="btn btn-sm" type="button" data-act="tv-copy">Copy code</button><button class="btn btn-sm" type="button" data-act="tv-cancel">Cancel</button></div></div>`;
  }
  if (t.signedIn) {
    return `<div class="row row-wide set-row"><span class="tile t-account">${icon('key')}</span>
      <div class="row-main"><div class="row-title">Signed in${t.name ? ` as ${esc(t.name)}` : ''}</div><div class="row-sub">${t.handle ? `${esc(t.handle)} · ` : ''}YouTube’s home, Subscriptions and History; YouTube Music’s playlists and Liked Music.</div></div>
      <div class="row-control acct-actions"><button class="btn btn-sm btn-red-text" type="button" data-act="tv-signout">Sign out</button></div></div>`;
  }
  const err = t.error && TV_ERRORS[t.error] ? `<div class="row-sub tv-err">${TV_ERRORS[t.error]}</div>` : '';
  return `<div class="row row-wide set-row"><span class="tile t-account">${icon('key')}</span>
      <div class="row-main"><div class="row-title">Sign in with a code</div><div class="row-sub">Like signing in a smart TV: get a code, enter it at google.com/device. Brings your YouTube home, Subscriptions and History, and your YouTube Music playlists.</div>${err}</div>
      <div class="row-control acct-actions"><button class="btn btn-primary btn-sm" type="button" data-act="tv-begin">${icon('key')}<span>Get a code</span></button></div></div>`;
}
function renderAccount() {
  const el = $('#ytAccount');
  if (!el) return;
  const a = S.ytAccount;
  if (!S.server?.direct) {
    el.innerHTML = `<div class="group"><div class="row set-row"><span class="tile t-account">${icon('key')}</span>
      <div class="row-main"><div class="row-title">YouTube account</div><div class="row-sub">Your server has its own: sign in on its admin page${S.server?.url ? ` (<a class="link" href="${esc(S.server.url.replace(/\/+$/, ''))}/admin" target="_blank" rel="noopener">open it</a>)` : ''}.</div></div></div></div>`;
    return;
  }
  const state = !a ? 'Checking…' : a.signedIn ? `Signed in${a.name ? ` as <b>${esc(a.name)}</b>` : ''}${a.handle ? ` (${esc(a.handle)})` : ''}${a.unchecked ? ' (not checked: no internet)' : ''}`
    : a.expired ? 'Signed out: YouTube no longer takes these cookies. Export them again.' : a.unreachable ? 'Couldn’t ask your Rondo.' : 'Signed out';
  el.innerHTML = `<h2 class="gh">YouTube account</h2><div class="group">${tvHtml()}</div>
    <h2 class="gh">YouTube Music home and history</h2>
    <div class="group">
      <div class="row row-wide set-row"><span class="tile t-cookies">${icon('file')}</span>
        <div class="row-main"><div class="row-title">cookies.txt</div><div class="row-sub">${state}</div>
          <div class="row-sub">Optional: only YouTube Music’s own home and history need a cookies.txt from a browser; everything else works with the code sign-in.</div></div>
        <div class="row-control acct-actions">
          <button class="btn btn-primary btn-sm" type="button" data-act="cookies">${icon('upload')}<span>${a?.signedIn ? 'Replace cookies.txt' : 'Upload cookies.txt'}</span></button>
          ${a?.signedIn || a?.expired ? '<button class="btn btn-sm" type="button" data-act="signout">Sign out</button>' : ''}
          <input type="file" id="cookiesFile" accept=".txt,text/plain" hidden></div></div>
    </div>
    <h2 class="gh">Export cookies.txt safely</h2>
    <div class="group"><ol class="steps">
      <li><b>Open a private window</b> (incognito) in your browser on a computer.</li>
      <li><b>Sign in to youtube.com</b> (or music.youtube.com: the same cookies) in that window.</li>
      <li><b>Export the cookies</b> with a cookies.txt browser extension while that page is open, then upload the file here.</li>
      <li><b>Close the private window at once.</b> If a browser keeps using that session, Google signs it out and your Rondo loses access.</li>
    </ol><p class="row-sub steps-note">A separate Google account for your Rondo is the safest choice. The file stays on your Rondo and this page never shows what’s inside.</p></div>`;
}
// a cookies.txt cut to its youtube.com lines (an export of every site is large, and none of the rest is needed)
function youtubeCookies(text) {
  return text.split(/\r?\n/).filter((l) => {
    if (!l.trim()) return false;
    if (l.startsWith('#') && !l.startsWith('#HttpOnly_')) return l.startsWith('# Netscape');
    const domain = l.replace(/^#HttpOnly_/, '').split('\t')[0] || '';
    return /(^|\.)youtube\.com$/.test(domain);
  }).join('\n') + '\n';
}
async function sendCookies(file) {
  if (file.size > 4 * 1024 * 1024) return toast('That file is too large to be a cookies.txt file. Export it again.', 'err');
  const text = youtubeCookies(await file.text());
  try {
    S.ytAccount = await api('PUT', '/api/youtube/cookies', { cookies: text }, { timeout: 40000 });
    renderAccount();
    toast(S.ytAccount.signedIn ? `Signed in${S.ytAccount.name ? ` as ${S.ytAccount.name}` : ''}` : 'YouTube didn’t take these cookies. Export them again.', S.ytAccount.signedIn ? undefined : 'err');
  } catch (e) { if (e.status !== 401) toast(reason(e), 'err'); }
}
function renderQuality() {
  const el = $('#ytQuality');
  if (!el || !S.settings) return;
  el.innerHTML = `<div class="group"><div class="row row-wide set-row"><span class="tile t-quality">${icon('diamond')}</span>
      <div class="row-main"><div class="row-title">Video quality</div><div class="row-sub">Auto picks up to 360p, which plays smoothest. A video without the one picked plays the next one down.</div></div>
      <div class="row-control">${segHtml('videoQuality', QUALITY, S.settings.videoQuality ?? null, 'Video quality')}</div></div></div>`;
}
function renderServer() {
  const s = S.server;
  const admin = s.url ? `${s.url.replace(/\/+$/, '')}/admin` : '';
  $('#serverBody').innerHTML = `
    <div class="group">
      <div class="row row-wide set-row"><span class="tile t-server">${icon('cloud')}</span>
        <div class="row-main"><div class="row-title">Connect to</div><div class="row-sub">${s.direct ? 'Rondo talks to YouTube itself: nothing to set up. Sign in below for Library, Subscriptions and History.' : 'Everything goes through your server.'}</div></div>
        <div class="row-control"><div class="seg seg-wide" role="radiogroup" aria-label="Connect to" id="srvMode">
          <button type="button" role="radio" aria-checked="${!!s.direct}" data-direct="1">Directly to YouTube</button>
          <button type="button" role="radio" aria-checked="${!s.direct}" data-direct="0"${s.url ? '' : ' disabled title="Connect to a server below first"'}>My server</button></div></div></div>
    </div>
    <div id="ytAccount"></div>
    <h2 class="gh">Playback</h2>
    <div id="ytQuality"></div>
    <h2 class="gh">Your server</h2>
    <div class="group">
      <div class="srv-field">
        <div class="row-title">Your server</div>
        <div class="row-sub srv-now"><span class="srv-url">${esc(s.url || 'None')}</span>
          ${s.url ? `<span class="tag ${insecure(s.url) ? 'warn' : 'ok'}">${insecure(s.url) ? 'Not encrypted' : 'Encrypted'}</span>` : ''}
          <span class="tag ${s.tokenSet ? 'ok' : 'warn'}">${s.tokenSet ? 'Connected' : 'Not connected'}</span></div>
        ${insecure(s.url) ? '<p class="row-sub srv-warn">Plain http: use it only on your home network.</p>' : ''}
        ${admin ? `<p class="row-sub"><a class="link" href="${esc(admin)}" target="_blank" rel="noopener">Open the server’s admin page</a></p>` : ''}
      </div>
    </div>
    <div class="group">
      <form class="srv-field" id="connectForm">
        <div class="row-title">Connect to a server</div>
        <p class="row-sub">On the server’s admin page, open Device › Connect a Rondo and type the code it shows.</p>
        <label class="field-label" for="srvUrl" style="margin-top:12px">Server address</label>
        <div class="field-row"><input type="url" id="srvUrl" value="${esc(s.url || '')}" placeholder="https://music.example.com or http://192.168.1.10:3000" inputmode="url" autocapitalize="off" autocomplete="url" spellcheck="false"></div>
        <p class="row-sub srv-warn" id="urlWarn" ${insecure(s.url) ? '' : 'hidden'}>Plain http isn’t encrypted: use it only on your home network.</p>
        <label class="field-label" for="srvCode" style="margin-top:12px">Connect code</label>
        <div class="field-row"><input type="text" id="srvCode" inputmode="numeric" autocomplete="one-time-code" placeholder="123 456" spellcheck="false"></div>
        <p class="field-err" id="urlErr" hidden></p>
        <div class="join-actions" style="margin-top:12px"><button class="btn" type="button" id="urlOnly">Save address only</button><button class="btn btn-primary" type="submit" id="connectBtn">Connect</button></div>
      </form>
    </div>
    <div class="group">
      <div class="srv-field">
        <div class="srv-token">
          <div class="row-main"><div class="row-title">Use a token instead <span class="tag ${s.tokenSet ? 'ok' : 'warn'}">${s.tokenSet ? 'Set' : 'Not set'}</span></div>
            <div class="row-sub">For servers set up with an API token. It’s stored on your Rondo and never shown here.</div></div>
          <button class="btn btn-sm" type="button" id="tokenEdit">${s.tokenSet ? 'Replace token' : 'Add token'}</button>
        </div>
        <form id="tokenForm" hidden style="margin-top:14px">
          <label class="field-label" for="srvToken">New API token</label>
          <div class="field-row"><input type="password" id="srvToken" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="Paste the token from your server"></div>
          <p class="field-err" id="tokenErr" hidden></p>
          <div class="join-actions" style="margin-top:12px"><button class="btn" type="button" id="tokenCancel">Cancel</button><button class="btn btn-primary" type="submit">Save token</button></div>
        </form>
      </div>
    </div>
    <div class="group test">
      <div class="test-head"><div class="row-main"><div class="row-title">Connection test</div><div class="row-sub">Your Rondo calls the server and reports back.</div></div>
        <button class="btn btn-primary" type="button" id="srvTest">${icon('refresh')}<span>Test</span></button></div>
      <div class="test-res" id="testRes" hidden></div>
    </div>`;
  renderAccount();
  renderQuality();
}
const serverBody = $('#serverBody');
serverBody.addEventListener('change', (e) => { if (e.target.id === 'cookiesFile' && e.target.files[0]) sendCookies(e.target.files[0]); });
serverBody.addEventListener('click', async (e) => {
  const seg = e.target.closest('#ytQuality .seg[data-key]'), v = e.target.closest('#ytQuality [data-v]');
  if (seg && v) {
    $$('button', seg).forEach((x) => x.setAttribute('aria-checked', String(x === v)));
    return saveSetting(seg.dataset.key, JSON.parse(v.dataset.v));
  }
  if (e.target.closest('[data-act="cookies"]')) return $('#cookiesFile').click();
  if (e.target.closest('[data-act="tv-begin"]')) return tvAct('begin');
  if (e.target.closest('[data-act="tv-cancel"]')) return tvAct('cancel');
  if (e.target.closest('[data-act="tv-signout"]')) return tvAct('signout');
  if (e.target.closest('[data-act="tv-copy"]')) {
    try { await navigator.clipboard.writeText(S.ytTv?.pending?.code || ''); toast('Code copied'); } catch { toast('Couldn’t copy: type it in', 'err'); }
    return;
  }
  if (e.target.closest('[data-act="signout"]')) {
    try {
      S.ytAccount = await api('DELETE', '/api/youtube/cookies', undefined, { timeout: 30000 });
      renderAccount();
      toast('Signed out on your Rondo');
    } catch (err) { if (err.status !== 401) toast(reason(err), 'err'); }
    return;
  }
  const b = e.target.closest('#srvMode [data-direct]');
  if (!b || b.disabled || b.getAttribute('aria-checked') === 'true') return;
  try {
    S.server = await api('PUT', '/api/server', { direct: b.dataset.direct === '1' });
    renderServer();
    loadYtAccount();
    toast('Saved on your Rondo');
  } catch (err) { if (err.status !== 401) toast(`Not saved. ${reason(err)}`, 'err'); }
});
serverBody.addEventListener('input', (e) => {
  if (e.target.id === 'srvUrl') {
    e.target.classList.remove('is-invalid');
    $('#urlErr').hidden = true;
    $('#urlWarn').hidden = !insecure(e.target.value.trim());
  }
  if (e.target.id === 'srvCode') {
    e.target.classList.remove('is-invalid');
    $('#urlErr').hidden = true;
    spaceCode(e.target);
  }
});
// a connect code as 123 456, the way the admin page shows it, however it was typed or pasted
// ("123456", "123-456", "Code: 123 456")
function spaceCode(el) {
  const digits = el.value.replace(/\D/g, '').slice(0, 6);
  const shown = digits.length > 3 ? `${digits.slice(0, 3)} ${digits.slice(3)}` : digits;
  if (el.value === shown) return;
  const atEnd = el.selectionStart === el.value.length;
  el.value = shown;
  if (atEnd) el.setSelectionRange(shown.length, shown.length);
}
// the address typed in, or null after showing why it isn't one
function serverUrl() {
  const input = $('#srvUrl'), err = $('#urlErr');
  const v = input.value.trim().replace(/\/+$/, '');
  let ok = false;
  try { ok = ['http:', 'https:'].includes(new URL(v).protocol); } catch { /* invalid */ }
  if (ok) return v;
  input.classList.add('is-invalid');
  err.hidden = false;
  err.textContent = 'Enter a full address, like https://music.example.com or http://192.168.1.10:3000';
  input.focus();
  return null;
}
serverBody.addEventListener('click', async (e) => {
  const id = e.target.closest('button')?.id;
  if (id === 'tokenEdit') { $('#tokenForm').hidden = false; $('#tokenEdit').hidden = true; $('#srvToken').focus(); }
  if (id === 'tokenCancel') { $('#tokenForm').hidden = true; $('#tokenEdit').hidden = false; $('#srvToken').value = ''; }
  if (id === 'srvTest') testServer();
  if (id === 'urlOnly') {
    const url = serverUrl();
    if (!url) return;
    try {
      S.server = await api('PUT', '/api/server', { url });
      renderServer();
      toast('Server address saved');
    } catch (e2) { if (e2.status !== 401) { $('#urlErr').hidden = false; $('#urlErr').textContent = `Not saved. ${reason(e2)}`; } }
  }
});
serverBody.addEventListener('submit', async (e) => {
  e.preventDefault();
  if (e.target.id === 'connectForm') {
    const url = serverUrl();
    if (!url) return;
    const codeInput = $('#srvCode'), err = $('#urlErr'), btn = $('#connectBtn');
    const code = codeInput.value.replace(/\D/g, '');
    if (code.length !== 6) { codeInput.classList.add('is-invalid'); err.hidden = false; err.textContent = 'Type the 6-digit code from the admin page.'; return codeInput.focus(); }
    btn.disabled = true;
    btn.textContent = 'Connecting…';
    try {
      S.server = await api('PUT', '/api/server', { url, code });
      renderServer();
      toast('Connected to your server');
    } catch (e2) {
      if (e2.status === 401) return;
      btn.disabled = false;
      btn.textContent = 'Connect';
      err.hidden = false;
      err.textContent = e2.status === 403 ? 'That code is wrong or has expired. Make a new one on the admin page.'
        : e2.code === 'unreachable' ? 'Your Rondo can’t reach that address. Check it, and that both are on the same network for a home server.'
        : `Couldn’t connect. ${reason(e2)}`;
    }
  }
  if (e.target.id === 'tokenForm') {
    const input = $('#srvToken'), err = $('#tokenErr');
    const v = input.value.trim();
    if (!v) { err.hidden = false; err.textContent = 'Paste the token first.'; return input.focus(); }
    try {
      S.server = await api('PUT', '/api/server', { token: v });
      renderServer();
      toast('Token saved');
    } catch (e2) { if (e2.status !== 401) { err.hidden = false; err.textContent = `Not saved. ${reason(e2)}`; } }
  }
});
// one fact of the connection test: a check (or an alert when not ok), title, note, value
function testRow(ok, title, sub, value, detail = '', muted = false) {
  const ico = ok ? (muted ? '<svg class="i" aria-hidden="true"><use href="#i-info"/></svg>' : '<svg class="i ok-ico" aria-hidden="true"><use href="#i-check"/></svg>')
    : '<svg class="i" aria-hidden="true" style="color:var(--amber)"><use href="#i-alert"/></svg>';
  const subs = [sub, detail].filter(Boolean).map((t) => `<div class="row-sub">${esc(t)}</div>`).join('');
  return `<div class="row">${ico}<div class="row-main"><div class="row-title">${esc(title)}</div>${subs}</div><div class="row-value">${esc(value)}</div></div>`;
}
async function testServer() {
  const btn = $('#srvTest'), res = $('#testRes');
  btn.disabled = true;
  btn.classList.add('is-busy');
  $('span', btn).textContent = 'Testing…';
  let r;
  try { r = await api('POST', '/api/server/test', undefined, { timeout: 30000 }); } catch (e) {
    if (e.status === 401) return;
    r = { ok: false, error: e.status === 0 ? 'rondo' : e.code || `error ${e.status}` };
  }
  btn.disabled = false;
  btn.classList.remove('is-busy');
  $('span', btn).textContent = 'Test again';
  res.hidden = false;
  if (r.ok) {
    res.innerHTML = `
      <div class="row"><svg class="i ok-ico" aria-hidden="true"><use href="#i-check"/></svg><div class="row-main"><div class="row-title">Connected</div></div><div class="row-value strong">${r.latencyMs} ms</div></div>
      ${testRow(r.playbackOk !== false, 'Playback', r.playbackOk === false ? 'The server’s last playback check failed. Update yt-dlp on the server’s admin page.' : '', r.playbackOk === false ? 'Failing' : r.playbackOk ? 'Working' : 'Not checked yet', r.ytdlp ? `yt-dlp ${r.ytdlp}` : '')}
      ${testRow(true, 'YouTube account', r.signedIn ? '' : 'Search and playback work without one. Sign in on the server’s admin page for your home, subscriptions and playlists.', r.signedIn ? 'Signed in' : 'Signed out', '', !r.signedIn)}`;
  } else {
    const msg = {
      unreachable: ['Can’t reach the server', 'Check the address, and that the server is running.'],
      unauthorized: ['The server refused the token', 'Replace the token with the one your server expects.'],
      rondo: ['Your Rondo didn’t answer', 'Keep Settings › Dashboard open on the Rondo and try again.'],
    }[r.error] || ['The test failed', r.error || 'No details.'];
    res.innerHTML = `<div class="row"><svg class="i err-ico" aria-hidden="true"><use href="#i-alert"/></svg><div class="row-main"><div class="row-title">${esc(msg[0])}</div><div class="row-sub">${esc(msg[1])}</div></div></div>`;
  }
}

// ---------- toast ----------
const toastEl = $('#toast');
let toastTimer = 0;
function toast(msg, kind = 'ok') {
  toastEl.className = `toast${kind === 'err' ? ' err' : ''}`;
  toastEl.innerHTML = `${icon(kind === 'err' ? 'alert' : 'check')}<span>${esc(msg)}</span>`;
  void toastEl.offsetWidth;
  toastEl.classList.add('show');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => toastEl.classList.remove('show'), kind === 'err' ? 4200 : 2600);
}

// ---------- go ----------
setFill(npSeek);
setFill(npVol);
boot();

// ---------- Date & time (Settings › Date & time on the Rondo) ----------
async function loadDateTime() {
  try {
    S.dateTime = await api('GET', '/api/datetime');
    S.dateTimeAt = Date.now();
    renderDateTime();
  } catch (e) {
    if (e.status !== 401 && !S.dateTime) $('#dateRows').innerHTML = `<div class="row-empty">Couldn’t load the date and time. ${esc(reason(e))}</div>`;
  }
}
// the Rondo's clock now, as a datetime-local value in its own time zone
function rondoLocal(d) {
  const t = new Date((d.now + d.offsetSec) * 1000 + (Date.now() - S.dateTimeAt));
  return t.toISOString().slice(0, 16);
}
function rondoClock(d) {
  const t = new Date((d.now + d.offsetSec) * 1000 + (Date.now() - S.dateTimeAt));
  return t.toLocaleString(undefined, { timeZone: 'UTC', dateStyle: 'medium', timeStyle: 'short', hour12: !d.clock24 });
}
function renderDateTime() {
  const d = S.dateTime;
  if (!d) return;
  const zones = d.zones.map((z) => `<option value="${esc(z.id)}"${z.id === d.zone ? ' selected' : ''}>${esc(z.gmt)} · ${esc(z.city)}</option>`).join('');
  $('#dateRows').innerHTML = `
    <div class="row set-row"><span class="tile t-sync">${icon('sync')}</span>
      <div class="row-main"><div class="row-title">Set automatically</div><div class="row-sub">From the network when Wi-Fi is on. Now: <b id="dtNow">${esc(rondoClock(d))}</b></div></div>
      <button class="switch" type="button" role="switch" data-dt="autoTime" aria-checked="${d.autoTime}" aria-label="Set the time automatically"></button></div>
    ${d.autoTime ? '' : `<div class="row row-wide set-row"><span class="tile t-clock">${icon('clock')}</span>
      <div class="row-main"><div class="row-title">Date and time</div><div class="row-sub">Set on your Rondo, in its time zone.</div></div>
      <div class="row-control date-set"><input type="datetime-local" id="dtValue" value="${esc(rondoLocal(d))}" aria-label="Date and time">
        <button class="btn btn-sm" type="button" id="dtFromHere">Use this computer’s time</button><button class="btn btn-sm btn-primary" type="button" id="dtSet">Set</button></div></div>`}
    <div class="row set-row"><span class="tile t-zone">${icon('broadcast')}</span>
      <div class="row-main"><div class="row-title">Time zone automatically</div><div class="row-sub">Where your Rondo is online. Now: ${esc(d.city)} (${esc(d.zone)})</div></div>
      <button class="switch" type="button" role="switch" data-dt="autoZone" aria-checked="${d.autoZone}" aria-label="Set the time zone automatically"></button></div>
    ${d.autoZone ? '' : `<div class="row row-wide set-row"><span class="tile t-globe">${icon('globe')}</span>
      <div class="row-main"><div class="row-title">Time zone</div></div>
      <div class="row-control"><div class="select"><select id="dtZone" aria-label="Time zone">${zones}</select>${icon('down')}</div></div></div>`}
    <div class="row set-row"><span class="tile t-timer">${icon('timer')}</span>
      <div class="row-main"><div class="row-title">24-hour time</div><div class="row-sub">Off: AM and PM.</div></div>
      <button class="switch" type="button" role="switch" data-dt="clock24" aria-checked="${d.clock24}" aria-label="24-hour time"></button></div>`;
}
async function putDateTime(body) {
  try {
    S.dateTime = await api('PUT', '/api/datetime', body);
    S.dateTimeAt = Date.now();
    renderDateTime();
    toast('Saved on your Rondo');
  } catch (e) {
    if (e.status !== 401) toast(`Not saved. ${reason(e)}`, 'err');
    renderDateTime();
  }
}
const dateRows = $('#dateRows');
dateRows.addEventListener('click', (e) => {
  const sw = e.target.closest('.switch[data-dt]');
  if (sw) return putDateTime({ [sw.dataset.dt]: sw.getAttribute('aria-checked') !== 'true' });
  if (e.target.closest('#dtFromHere')) {
    const now = new Date(), d = S.dateTime;
    // this computer's moment, shown in the Rondo's zone
    $('#dtValue').value = new Date(now.getTime() + d.offsetSec * 1000).toISOString().slice(0, 16);
    return;
  }
  if (e.target.closest('#dtSet')) {
    const v = $('#dtValue').value, d = S.dateTime;
    if (!v) return;
    // the value is the Rondo's local time: back to UTC by its offset
    const epoch = Math.round(Date.parse(v + ':00Z') / 1000) - d.offsetSec;
    putDateTime({ now: epoch });
  }
});
dateRows.addEventListener('change', (e) => { if (e.target.id === 'dtZone') putDateTime({ zone: e.target.value }); });
setInterval(() => { const el = document.getElementById('dtNow'); if (el && S.dateTime && S.route === 'settings') el.textContent = rondoClock(S.dateTime); }, 15000);

// ---------- Sound (Settings › Sound on the Rondo) ----------
// Each output's own: the equalizer (a preset, or ten bands of your own), headphone match, the loudness
// boost; for every output: volume leveling, the amplifier and the volume limit.
let soundOut = null, eqTimer = 0;
const BOOSTS = [[0, 'Off'], [3, '+3 dB'], [6, '+6 dB'], [9, '+9 dB'], [12, '+12 dB']];
const AMPS = [[0, 'Standard'], [1, '+2 dB'], [2, '+4 dB'], [3, '+6 dB']];
const freqName = (f) => (f >= 1000 ? `${f / 1000}k` : String(f));
const gainName = (g) => (g > 0 ? `+${g.toFixed(1)}` : g.toFixed(1)).replace('.0', '');
async function loadSound(quiet = false) {
  if (!S.sound && !quiet) $('#soundBody').innerHTML = '<div class="group"><div class="lib-skel"></div><div class="lib-skel"></div><div class="lib-skel"></div></div>';
  try {
    S.sound = await api('GET', '/api/sound');
    renderSound();
  } catch (e) {
    if (e.status !== 401 && !S.sound) $('#soundBody').innerHTML = `<div class="group"><div class="row-empty">Couldn’t load the sound settings. ${esc(reason(e))} <button class="link" type="button" data-act="reload">Try again</button></div></div>`;
  }
}
function soundOutput() {
  const s = S.sound;
  if (!soundOut || !s.outputs.some((o) => o.key === soundOut)) soundOut = s.current;
  return s.outputs.find((o) => o.key === soundOut) || s.outputs[0];
}
function renderSound() {
  const s = S.sound, o = soundOutput();
  const outs = s.outputs.map((x) => `<button type="button" role="radio" aria-checked="${x.key === o.key}" data-out="${esc(x.key)}">${esc(x.label)}${x.key === s.current ? ' · playing' : ''}</button>`).join('');
  const presets = [...s.presets.map((p) => [p.id, p.name + (p.note ? ` (${p.note})` : '')]), ...s.userPresets.map((u) => ['user:' + u.name, u.name]),
    ...(o.match ? [['autoeq', `${o.match.name} (headphone match)`]] : []), ['custom', 'Custom (your own bands)']];
  const bands = s.freqs.map((fr, i) => `<label class="eq-band"><output>${gainName(o.gains[i])}</output>
      <input type="range" min="-12" max="12" step="0.5" value="${o.gains[i]}" data-band="${i}" aria-label="${freqName(fr)} Hz"><span>${freqName(fr)}</span></label>`).join('');
  const user = o.preset.startsWith('user:') ? o.preset.slice(5) : '';
  const limitName = (v) => (v >= s.maxVolume ? 'No limit' : `${v} of ${s.maxVolume}`);
  $('#soundBody').innerHTML = `
    <div class="group"><div class="row row-wide set-row"><span class="tile t-output">${icon('headphones')}</span>
      <div class="row-main"><div class="row-title">Output</div><div class="row-sub">The one music plays on now is marked “playing”. Changes to another apply when it plays.</div></div>
      <div class="row-control"><div class="seg seg-wide sound-out" role="radiogroup" aria-label="Output" id="soundOuts">${outs}</div></div></div></div>
    <h2 class="gh">${esc(o.label)}</h2>
    <div class="group">
      <div class="row row-wide set-row"><span class="tile t-eq">${icon('sliders')}</span>
        <div class="row-main"><div class="row-title">Equalizer</div><div class="row-sub">Moving a band makes it your own (Custom).</div></div>
        <div class="row-control"><div class="select"><select id="eqPreset" aria-label="Equalizer preset">${presets.map(([v, l]) => `<option value="${esc(v)}"${v === o.preset ? ' selected' : ''}>${esc(l)}</option>`).join('')}</select>${icon('down')}</div></div></div>
      <div class="eq" id="eqBands">${bands}</div>
      <div class="eq-actions"><button class="btn btn-sm" type="button" id="eqFlat">Flat</button><button class="btn btn-sm" type="button" id="eqSave">${icon('plus')}<span>Save as a preset</span></button>
        ${user ? `<button class="btn btn-sm btn-red-text" type="button" id="eqDelete" data-name="${esc(user)}">${icon('trash')}<span>Delete “${esc(user)}”</span></button>` : ''}</div>
      ${o.key === 'speaker' ? '' : `<div class="row set-row"><span class="tile t-search">${icon('search')}</span>
        <div class="row-main"><div class="row-title">Headphone match</div><div class="row-sub">${o.match ? `${esc(o.match.name)}, measured by ${esc(o.match.source)}${o.preset === 'autoeq' ? ' · in use' : ''}` : 'A correction for your model, from AutoEq’s measurements of about 9,000 headphones.'}</div></div>
        <button class="btn btn-sm" type="button" id="matchFind">${o.match ? 'Change model' : 'Find your model'}</button></div>`}
      <div class="row row-wide set-row"><span class="tile t-now">${icon('volume')}</span>
        <div class="row-main"><div class="row-title">Loudness boost</div><div class="row-sub">Louder; the peaks are held, never clipped.</div></div>
        <div class="row-control">${segHtml('boostDb', BOOSTS, o.boostDb, 'Loudness boost')}</div></div>
    </div>
    <h2 class="gh">Every output</h2>
    <div class="group">
      <div class="row set-row"><span class="tile t-level">${icon('gauge')}</span>
        <div class="row-main"><div class="row-title">Volume leveling</div><div class="row-sub">${esc(s.levelingNote)}</div></div>
        <button class="switch" type="button" role="switch" data-key="leveling" aria-checked="${s.leveling}" aria-label="Volume leveling"></button></div>
      ${s.amplifier >= 0 ? `<div class="row row-wide set-row"><span class="tile t-amp">${icon('bolt')}</span>
        <div class="row-main"><div class="row-title">Amplifier</div><div class="row-sub">The Y1’s own, for headphones and the speaker. Standard is the cleanest, +6 dB the loudest with more hiss.</div></div>
        <div class="row-control">${segHtml('amplifier', AMPS, s.amplifier, 'Amplifier')}</div></div>` : ''}
      <div class="row row-wide set-row"><span class="tile t-limit">${icon('lock')}</span>
        <div class="row-main"><div class="row-title">Volume limit</div><div class="row-sub">The highest step the wheel and this page can set, on every output. Anything louder comes down to it.</div></div>
        <div class="row-control"><div class="range-row">${icon('volume')}<input type="range" id="volLimit" min="1" max="${s.maxVolume}" step="1" value="${s.volumeLimit}" aria-label="Volume limit"><output id="volLimitOut">${limitName(s.volumeLimit)}</output></div></div></div>
    </div>`;
  setFill($('#volLimit'));
}
async function putSound(body, { quiet = false } = {}) {
  try {
    S.sound = await api('PUT', '/api/sound', { output: soundOutput().key, ...body });
    renderSound();
    if (!quiet) toast('Saved on your Rondo');
    return true;
  } catch (e) {
    if (e.status !== 401) toast(`Not saved. ${reason(e)}`, 'err');
    loadSound(true);
    return false;
  }
}
const soundBody = $('#soundBody');
soundBody.addEventListener('click', async (e) => {
  if (e.target.closest('[data-act="reload"]')) return loadSound();
  const out = e.target.closest('#soundOuts [data-out]');
  if (out) { soundOut = out.dataset.out; return renderSound(); }
  const seg = e.target.closest('.seg[data-key]'), b = e.target.closest('[data-v]');
  if (seg && b) {
    $$('button', seg).forEach((x) => x.setAttribute('aria-checked', String(x === b)));
    return putSound({ [seg.dataset.key]: JSON.parse(b.dataset.v) });
  }
  const sw = e.target.closest('.switch[data-key]');
  if (sw) return putSound({ [sw.dataset.key]: sw.getAttribute('aria-checked') !== 'true' });  // leveling
  if (e.target.closest('#eqFlat')) return putSound({ preset: 'flat' });
  if (e.target.closest('#eqSave')) {
    const name = await promptSheet({ title: 'Name this preset', text: 'Kept with the presets, for every output.', action: 'Save', placeholder: 'Name' });
    if (name) putSound({ savePreset: name, custom: soundOutput().gains });
    return;
  }
  const del = e.target.closest('#eqDelete');
  if (del) {
    if (await confirmSheet({ title: `Delete “${del.dataset.name}”?`, text: 'Outputs using it go back to Flat.', action: 'Delete' })) putSound({ deletePreset: del.dataset.name });
    return;
  }
  if (e.target.closest('#matchFind')) findMatch();
});
soundBody.addEventListener('change', (e) => {
  if (e.target.id === 'eqPreset') putSound({ preset: e.target.value });
  if (e.target.id === 'volLimit') putSound({ volumeLimit: +e.target.value });
});
soundBody.addEventListener('input', (e) => {
  const band = e.target.dataset?.band;
  if (band != null) {
    const o = soundOutput(), v = +e.target.value;
    o.gains[+band] = v;
    e.target.previousElementSibling.textContent = gainName(v);
    clearTimeout(eqTimer);
    // the sound follows as you drag; saved as your own bands
    eqTimer = setTimeout(() => putSound({ custom: o.gains }, { quiet: true }), 200);
    return;
  }
  if (e.target.id === 'volLimit') {
    setFill(e.target);
    $('#volLimitOut').textContent = +e.target.value >= S.sound.maxVolume ? 'No limit' : `${e.target.value} of ${S.sound.maxVolume}`;
  }
});
// Headphone match: a model looked up on the server's AutoEq index, its correction kept with the output
function findMatch() {
  const o = soundOutput();
  openSheet(`<h2>Headphone match</h2><p>For ${esc(o.label)}. Through your music server.</p>
    <form class="sheet-form" id="matchForm"><label class="search pick-search">${icon('search')}<input type="search" placeholder="Like Sennheiser HD 650 or AirPods Pro" value="${esc(o.key.startsWith('bt:') ? o.label : '')}" autocomplete="off" aria-label="Headphone model"></label></form>
    <div class="match-list" id="matchList"></div><div class="sheet-actions"><button class="btn" type="button" data-r="0">Close</button></div>`);
  const input = $('input', sheet), list = $('#matchList', sheet);
  let asked = 0;
  const search = async () => {
    const q = input.value.trim();
    if (!q) return;
    const mine = ++asked;
    list.innerHTML = '<div class="row-empty">Looking…</div>';
    try {
      const r = await api('GET', `/api/sound/match?q=${encodeURIComponent(q)}`, undefined, { timeout: 60000 });
      if (mine !== asked) return;
      const items = r.items || [];
      list.innerHTML = items.length ? items.map((it) => `<button type="button" data-id="${esc(it.id)}" data-name="${esc(it.name)}" data-source="${esc(it.source)}"><div class="row-title">${esc(it.name)}</div><div class="row-sub">Measured by ${esc(it.source)}</div></button>`).join('') : '<div class="row-empty">No model by that name.</div>';
    } catch (e) {
      if (mine === asked) list.innerHTML = `<div class="row-empty">${esc(e.status === 404 ? 'Your server can’t look headphones up yet: update it.' : reason(e))}</div>`;
    }
  };
  $('#matchForm', sheet).addEventListener('submit', (e) => { e.preventDefault(); search(); });
  $('[data-r="0"]', sheet).addEventListener('click', () => sheet.close());
  list.addEventListener('click', async (e) => {
    const b = e.target.closest('[data-id]');
    if (!b) return;
    list.innerHTML = `<div class="row-empty">Getting ${esc(b.dataset.name)}…</div>`;
    try {
      S.sound = await api('POST', '/api/sound/match', { output: o.key, id: b.dataset.id, name: b.dataset.name, source: b.dataset.source }, { timeout: 60000 });
      sheet.close();
      renderSound();
      toast(`${b.dataset.name}: in use on ${o.label}`);
    } catch (err) { list.innerHTML = `<div class="row-empty">${esc(reason(err))}</div>`; }
  });
  input.focus();
  if (input.value) search();
}

// ---------- smart playlists (core/smart.h on the Rondo) ----------
// Songs picked by rules (genre is Pop, played in the last 30 days, 4 stars and up…), worked out by the
// Rondo whenever they're opened: they follow what you add, play and rate. Saved on the card beside
// the playlists; the Rondo lists and plays them under Library › Playlists.
const SM_FIELDS = [
  ['title', 'Title', 'text'], ['artist', 'Artist', 'text'], ['album', 'Album', 'text'], ['albumArtist', 'Album artist', 'text'],
  ['genre', 'Genre', 'text'], ['folder', 'Folder', 'text'], ['year', 'Year', 'number'], ['rating', 'Rating (stars)', 'number'],
  ['plays', 'Plays', 'number'], ['length', 'Length (seconds)', 'number'], ['added', 'Added', 'date'], ['lastPlayed', 'Last played', 'date'],
];
const SM_OPS = {
  text: [['contains', 'contains'], ['is', 'is'], ['isNot', 'is not'], ['notContains', 'doesn’t contain'], ['startsWith', 'starts with']],
  number: [['atLeast', 'is at least'], ['atMost', 'is at most'], ['is', 'is'], ['isNot', 'is not']],
  date: [['inLast', 'in the last'], ['notInLast', 'not in the last']],
};
const SM_SORTS = [['artist', 'Artist'], ['album', 'Album'], ['title', 'Title'], ['random', 'Random'], ['added', 'Recently added'],
  ['lastPlayed', 'Recently played'], ['plays', 'Most played'], ['rating', 'Best rated'], ['year', 'Newest year']];
const smKind = (field) => (SM_FIELDS.find((x) => x[0] === field) || SM_FIELDS[0])[2];
function smRuleText(r) {
  const fl = (SM_FIELDS.find((x) => x[0] === r.field) || [r.field, r.field])[1].replace(/ \(.*\)/, '').toLowerCase();
  const op = ((SM_OPS[smKind(r.field)] || []).find((x) => x[0] === r.op) || [r.op, r.op])[1];
  const v = smKind(r.field) === 'date' ? plural(r.value, 'day') : smKind(r.field) === 'text' ? `“${r.value}”` : r.value;
  return `${fl} ${op} ${v}`;
}
const smSub = (p) => `${plural(p.count, 'song')}${p.durationSec ? ` · ${fmtDur(p.durationSec)}` : ''} · ${p.rules.map(smRuleText).join(p.match === 'any' ? ' or ' : ', ')}`;
function smartListHtml(q) {
  const all = S.smart || [];
  const shown = q ? all.filter((p) => q.split(/\s+/).every((w) => fold(p.name).includes(w))) : all;
  return `<div class="lhdr" role="heading" aria-level="3">Smart playlists</div>
    <div class="song pl-row pl-new" role="listitem" data-act="sm-new"><span class="art art-add">${icon('plus')}</span><span class="s-main"><span class="s-title">New smart playlist</span><span class="s-sub">Songs picked by rules: a genre, a year, what you play and rate</span></span></div>` +
    shown.map((p) => `<div class="song pl-row" role="listitem" data-sm="${esc(p.name)}"><span class="art">${icon('bolt')}</span>
      <span class="s-main"><span class="s-title">${esc(p.name)}</span><span class="s-sub">${esc(smSub(p))}</span></span>
      <span class="s-acts">${p.count ? `<button class="ib ib-sm" type="button" data-act="sm-play" aria-label="Play ${esc(p.name)} on your Rondo">${icon('play')}</button>` : ''}<button class="ib ib-sm" type="button" data-act="sm-edit" aria-label="Edit ${esc(p.name)}">${icon('gear')}</button></span></div>`).join('');
}
async function renderSmart() {
  const name = S.openSmart;
  const p = (S.smart || []).find((x) => x.name === name);
  if (!p) { S.openSmart = null; return renderPlaylists(); }
  $('#libSummary').textContent = 'A smart playlist: the Rondo picks its songs by its rules.';
  libList.innerHTML = '<div class="lib-skel"></div>'.repeat(4);
  let songs;
  try { songs = await api('GET', `/api/smart/songs?name=${encodeURIComponent(name)}`); } catch (e) {
    if (e.status !== 401) libList.innerHTML = `<div class="lib-empty"><h3>Couldn’t read it</h3><p>${esc(reason(e))}</p></div>`;
    return;
  }
  if (S.openSmart !== name) return;
  S.smartPaths = songs.paths;
  const head = `<div class="pl-head"><button class="btn btn-sm" type="button" data-act="sm-back">${icon('chevron', 'i flip')}<span>Playlists</span></button>
      <h2>${esc(p.name)}</h2><p>${esc(smSub(songs))}</p>
      <div class="pl-acts">${songs.count ? `<button class="btn btn-primary btn-sm" type="button" data-act="sm-play">${icon('play')}<span>Play on Rondo</span></button>` : ''}
        <button class="btn btn-sm" type="button" data-act="sm-edit">${icon('gear')}<span>Edit rules</span></button>
        <button class="btn btn-sm btn-red-text" type="button" data-act="sm-delete">${icon('trash')}<span>Delete</span></button></div></div>`;
  const rows = songs.paths.map((path) => {
    const t = S.libByPath?.get(path);
    if (!t) return '';
    return `<div class="song${isPlaying(t) ? ' is-playing' : ''}" role="listitem" data-smpath="${esc(path)}">
      <span class="art"${t.art ? ` data-art="${esc(path)}"` : ''}>${icon('note')}</span>
      <span class="s-main"><span class="s-title">${esc(t.title || fileName(path))}</span><span class="s-sub">${esc([t.artist, t.album].filter(Boolean).join(' · '))}</span></span>
      <span class="s-time">${fmtDur(t.durationSec)}</span></div>`;
  }).join('');
  libList.innerHTML = head + (rows || '<div class="lib-empty"><h3>No songs match</h3><p>Change the rules, or play and rate more songs.</p></div>');
  $$('.art[data-art]', libList).forEach((el) => artIO.observe(el));
}
async function playSmart(p, from) {
  try {
    const songs = from && S.smartPaths ? { paths: S.smartPaths } : await api('GET', `/api/smart/songs?name=${encodeURIComponent(p.name)}`);
    if (!songs.paths.length) return;
    const pl = await api('POST', '/api/player/play', { path: from || songs.paths[0], queue: songs.paths });
    if (S.status) { S.status.player = pl; S.statusAt = performance.now(); }
    renderMini(); markPlayingRows();
    toast(`Playing “${p.name}” on your Rondo`);
  } catch (e) { if (e.status !== 401) toast(`Couldn’t play it. ${reason(e)}`, 'err'); }
}
// true when the click was a smart playlist's
async function onSmartClick(e) {
  const b = e.target.closest('[data-act]'), row = e.target.closest('.song');
  const act = b?.dataset.act || (row?.dataset.sm ? 'sm-open' : row?.dataset.smpath ? 'sm-song' : '');
  if (!act.startsWith('sm-')) return false;
  const name = row?.dataset.sm || S.openSmart;
  const p = (S.smart || []).find((x) => x.name === name);
  if (act === 'sm-new') { editSmart(null); return true; }
  if (act === 'sm-back') { S.openSmart = null; renderPlaylists(); return true; }
  if (!p) return true;
  if (act === 'sm-open') { S.openSmart = p.name; S.openPlaylist = null; renderPlaylists(); return true; }
  if (act === 'sm-play') { playSmart(p); return true; }
  if (act === 'sm-song') { playSmart(p, row.dataset.smpath); return true; }
  if (act === 'sm-edit') { editSmart(p); return true; }
  if (act === 'sm-delete') {
    if (!(await confirmSheet({ title: `Delete “${p.name}”?`, text: 'The smart playlist goes; its songs stay on the card.', action: 'Delete' }))) return true;
    try {
      S.smart = (await api('DELETE', `/api/smart?name=${encodeURIComponent(p.name)}`)).playlists;
      S.openSmart = null;
      renderPlaylists();
      toast(`Deleted “${p.name}”`);
    } catch (e2) { if (e2.status !== 401) toast(`Couldn’t delete it. ${reason(e2)}`, 'err'); }
    return true;
  }
  return true;
}
// The editor: a name, all or any of the rules, the order and a limit; what they pick shows as you go
function editSmart(p) {
  const draft = p ? JSON.parse(JSON.stringify({ name: p.name, match: p.match, rules: p.rules, sort: p.sort, limit: p.limit }))
    : { name: '', match: 'all', rules: [{ field: 'genre', op: 'contains', value: '' }], sort: 'artist', limit: 0 };
  const opts = (list, cur) => list.map(([v, l]) => `<option value="${esc(v)}"${v === cur ? ' selected' : ''}>${esc(l)}</option>`).join('');
  const ruleHtml = (r, i) => {
    const kind = smKind(r.field);
    const value = kind === 'text' ? `<input type="text" data-k="value" value="${esc(r.value ?? '')}" placeholder="${r.field === 'genre' ? 'Pop' : r.field === 'folder' ? 'Music/Jazz' : ''}" aria-label="Value">`
      : `<input type="number" data-k="value" value="${esc(r.value ?? '')}" min="${kind === 'date' ? 1 : 0}" step="1" aria-label="${kind === 'date' ? 'Days' : 'Number'}">${kind === 'date' ? '<span class="sm-unit">days</span>' : ''}`;
    return `<div class="sm-rule" data-i="${i}"><div class="select"><select data-k="field" aria-label="Field">${opts(SM_FIELDS.map((x) => [x[0], x[1]]), r.field)}</select>${icon('down')}</div>
      <div class="select"><select data-k="op" aria-label="Condition">${opts(SM_OPS[kind], r.op)}</select>${icon('down')}</div>${value}
      <button class="ib ib-sm" type="button" data-rm="${i}" aria-label="Remove this rule"${draft.rules.length > 1 ? '' : ' disabled'}>${icon('close')}</button></div>`;
  };
  const html = () => `<h2>${p ? 'Edit smart playlist' : 'New smart playlist'}</h2>
    <form class="sheet-form sm-form" novalidate>
      <label class="field-label" for="smName">Name</label>
      <div class="field-row"><input type="text" id="smName" maxlength="60" value="${esc(draft.name)}" placeholder="Like Pop of the 2010s" autocomplete="off"></div>
      <div class="sm-match">Songs that match <div class="select"><select id="smMatch" aria-label="Match">${opts([['all', 'all'], ['any', 'any']], draft.match)}</select>${icon('down')}</div> of these rules:</div>
      <div id="smRules">${draft.rules.map(ruleHtml).join('')}</div>
      <button class="btn btn-sm" type="button" id="smAdd"${draft.rules.length >= 12 ? ' disabled' : ''}>${icon('plus')}<span>Add a rule</span></button>
      <div class="sm-match">In the order of <div class="select"><select id="smSort" aria-label="Order">${opts(SM_SORTS, draft.sort)}</select>${icon('down')}</div>,
        at most <input type="number" id="smLimit" min="0" max="5000" step="1" value="${draft.limit || ''}" placeholder="any number of" aria-label="Songs at most"> songs</div>
      <p class="sm-preview" id="smPreview" aria-live="polite"></p>
      <p class="field-err" id="smErr" hidden></p>
      <div class="sheet-actions"><button class="btn" type="button" data-r="0">Cancel</button><button class="btn btn-primary" type="submit">Save</button></div>
    </form>`;
  openSheet(html());
  let timer = 0, asked = 0;
  const read = () => {
    draft.name = $('#smName', sheet).value.trim();
    draft.match = $('#smMatch', sheet).value;
    draft.sort = $('#smSort', sheet).value;
    draft.limit = Math.max(0, Math.min(5000, parseInt($('#smLimit', sheet).value, 10) || 0));
    $$('.sm-rule', sheet).forEach((el, i) => {
      const r = draft.rules[i];
      r.field = $('[data-k="field"]', el).value;
      r.op = $('[data-k="op"]', el).value;
      const v = $('[data-k="value"]', el).value;
      r.value = smKind(r.field) === 'text' ? v : Number(v);
    });
  };
  const preview = () => {
    clearTimeout(timer);
    timer = setTimeout(async () => {
      const mine = ++asked, out = $('#smPreview', sheet);
      if (!out) return;
      try {
        const r = await api('POST', '/api/smart/preview', { playlist: draft });
        if (mine === asked) out.textContent = r.count ? `Now: ${plural(r.count, 'song')}${r.durationSec ? `, ${fmtDur(r.durationSec)}` : ''}` : 'Now: no songs match';
      } catch (e) { if (mine === asked) out.textContent = e.code && e.code !== 'pair' ? e.code : ''; }
    }, 300);
  };
  const redrawRules = () => { $('#smRules', sheet).innerHTML = draft.rules.map(ruleHtml).join(''); $('#smAdd', sheet).disabled = draft.rules.length >= 12; };
  const form = $('form', sheet);
  form.addEventListener('input', () => { read(); preview(); });
  form.addEventListener('change', (e) => {
    const k = e.target.dataset?.k;
    if (k === 'field') {  // a field of another kind takes that kind's first condition and no value
      const el = e.target.closest('.sm-rule'), r = draft.rules[+el.dataset.i];
      const kind = smKind(e.target.value);
      if (kind !== smKind(r.field)) { r.field = e.target.value; r.op = SM_OPS[kind][0][0]; r.value = kind === 'date' ? 30 : kind === 'number' ? (r.field === 'rating' ? 4 : 0) : ''; redrawRules(); }
    }
    read(); preview();
  });
  form.addEventListener('click', (e) => {
    const rm = e.target.closest('[data-rm]');
    if (rm) { read(); draft.rules.splice(+rm.dataset.rm, 1); redrawRules(); preview(); return; }
    if (e.target.closest('#smAdd')) { read(); draft.rules.push({ field: 'rating', op: 'atLeast', value: 4 }); redrawRules(); preview(); }
    if (e.target.closest('[data-r="0"]')) sheet.close();
  });
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    read();
    const err = $('#smErr', sheet);
    if (!draft.name) { err.textContent = 'Type a name.'; err.hidden = false; return; }
    try {
      S.smart = (await api('PUT', '/api/smart', { playlist: draft, oldName: p ? p.name : undefined })).playlists;
      sheet.close();
      // the name as the card keeps it (FAT: no : / \ * ? " < > |)
      if (S.openSmart) S.openSmart = draft.name.replace(/[\\/:*?"<>|]/g, ' ').replace(/\s+/g, ' ').trim();
      renderPlaylists();
      toast(`Saved “${draft.name}”`);
    } catch (e2) { if (e2.status !== 401) { err.textContent = e2.code && e2.code !== 'pair' ? e2.code : reason(e2); err.hidden = false; } }
  });
  $('#smName', sheet).focus();
  read();
  preview();
}

// ---------- Organize the card (Library › Organize the card on the Rondo) ----------
// Every song into Music/Artist/Album (Year)/, every video into Shows/ or Movies/, as Jellyfin expects:
// first what would move, then the moves only when asked. Playlists, plays and ratings go along.
$('#organizeBtn').addEventListener('click', openOrganize);
async function openOrganize() {
  openSheet('<h2>Organize the card</h2><p>Working out where everything goes…</p>');
  let plan;
  try { plan = await api('GET', '/api/organize', undefined, { timeout: 60000 }); } catch (e) {
    if (e.status !== 401) openSheet(`<h2>Organize the card</h2><p>${esc(reason(e))}</p><div class="sheet-actions"><button class="btn" type="button" data-r="0">Close</button></div>`);
    $('[data-r="0"]', sheet)?.addEventListener('click', () => sheet.close());
    return;
  }
  if (plan.status.running) return organizeProgress();
  const n = plan.moves.length;
  const kindName = { song: 'Song', video: 'Video', lyrics: 'Lyrics', subtitles: 'Subtitles', cover: 'Picture' };
  const shown = plan.moves.slice(0, 400);
  const facts = [['Songs', plan.songs, 'Music/Artist/Album (Year)/01 - Title'], ['Videos', plan.videos, 'Shows/Series (Year)/Season 01, or Movies/Title (Year)'],
    ['Lyrics and subtitles', plan.sidecars, 'with their songs and videos'], ['Folder pictures', plan.covers, 'with their albums'], ['Already in place', plan.alreadyThere, '']]
    .filter((x) => x[1]);
  openSheet(`<h2>Organize the card</h2>
    <p>${n ? `Folders the way Jellyfin, Plex and Kodi expect them, from the songs’ tags. Nothing moves until you press Organize; playlists, plays and ratings go along.` : 'Everything on the card is where it belongs.'}</p>
    <dl class="facts">${facts.map(([k, v, sub]) => `<dt>${k}</dt><dd>${v}${sub ? ` <span class="org-sub">${esc(sub)}</span>` : ''}</dd>`).join('')}</dl>
    ${n ? `<div class="org-list" role="list" aria-label="The moves">${shown.map((m) => `<div class="org-move" role="listitem"><span class="tag">${kindName[m.kind] || m.kind}</span>
      <div><div class="org-to">${esc(m.to)}</div><div class="org-from">from ${esc(m.from)}</div></div></div>`).join('')}
      ${n > shown.length ? `<div class="row-empty">and ${plural(n - shown.length, 'more')}</div>` : ''}</div>` : ''}
    <div class="sheet-actions"><button class="btn" type="button" data-r="0">${n ? 'Cancel' : 'Close'}</button>${n ? `<button class="btn btn-primary" type="button" data-r="go">Organize ${plural(n, 'file')}</button>` : ''}</div>`);
  $('[data-r="0"]', sheet).addEventListener('click', () => sheet.close());
  $('[data-r="go"]', sheet)?.addEventListener('click', async (e) => {
    e.target.closest('button').disabled = true;
    try { await api('POST', '/api/organize'); organizeProgress(); } catch (err) {
      if (err.status !== 401) toast(`Didn’t start. ${err.code && err.code !== 'pair' ? err.code : reason(err)}`, 'err');
      e.target.closest('button').disabled = false;
    }
  });
}
async function organizeProgress() {
  openSheet('<h2>Organizing</h2><div class="meter"><span id="orgBar" style="--p:0%"></span></div><p id="orgNote">Starting…</p>');
  for (;;) {
    let st;
    try { st = await api('GET', '/api/organize/status'); } catch { await new Promise((r) => setTimeout(r, 1500)); continue; }
    const bar = document.getElementById('orgBar'), note = document.getElementById('orgNote');
    if (!bar) return;  // the sheet was closed
    bar.style.setProperty('--p', `${st.total ? (st.done / st.total) * 100 : 0}%`);
    note.textContent = st.running ? `Moving ${st.done} of ${st.total}… Keep your Rondo on.` : '';
    if (!st.running) {
      openSheet(`<h2>Organized</h2><p>${plural(st.moved, 'file')} moved${st.errors.length ? `, ${plural(st.errors.length, 'file')} couldn’t be:` : '.'}</p>
        ${st.errors.length ? `<div class="org-list">${st.errors.slice(0, 100).map((x) => `<div class="org-move"><div class="org-from">${esc(x)}</div></div>`).join('')}</div>` : ''}
        <div class="sheet-actions"><button class="btn btn-primary" type="button" data-r="0">Done</button></div>`);
      $('[data-r="0"]', sheet).addEventListener('click', () => sheet.close());
      setTimeout(() => loadLibrary(true), 1500);  // the new paths
      return;
    }
    await new Promise((r) => setTimeout(r, 700));
  }
}

// ---------- Files: the card as it is (the Y1's Library › Folders) ----------
// Open folders, make one, rename, move and delete files and folders (several at once), upload into the
// folder you're in, download a file. Songs and videos keep their plays, ratings and playlists when
// they move; a folder takes everything in it along.
const FILE_ICON = { folder: 'folder', song: 'note', video: 'film', image: 'sun', text: 'lyrics', other: 'info' };
async function loadFiles(dir, quiet = false) {
  if (!quiet) libList.innerHTML = '<div class="lib-skel"></div>'.repeat(5);
  try {
    S.files = await api('GET', `/api/files${dir ? `?path=${encodeURIComponent(dir)}` : ''}`);
    S.filesDir = S.files.path;
    if (!quiet) S.fileSel = new Set();
  } catch (e) {
    if (e.status === 404 && dir) return loadFiles(null, quiet);  // a folder that went: the card's top
    if (e.status !== 401) libList.innerHTML = `<div class="lib-empty"><h3>Couldn’t open the folder</h3><p>${esc(reason(e))}</p></div>`;
    return;
  }
  renderLibrary();
}
const relName = (p) => { for (const r of S.files?.roots || [{ path: '/storage/sdcard0', name: 'Card' }, { path: '/storage/sdcard1', name: 'Internal storage' }]) if (p === r.path) return r.name; else if (p.startsWith(r.path + '/')) return `${r.name}/${p.slice(r.path.length + 1)}`; return p; };
function renderFiles() {
  const F = S.files;
  if (!F) { libList.innerHTML = '<div class="lib-skel"></div>'.repeat(5); return; }
  S.fileSel = S.fileSel || new Set();
  const q = fold(S.libQuery.trim());
  const list = F.entries.filter((e) => !q || fold(e.name + ' ' + (e.title || '') + ' ' + (e.artist || '')).includes(q))
    .sort((a, b) => (b.kind === 'folder') - (a.kind === 'folder') || collator.compare(a.name, b.name));
  const sel = [...S.fileSel].filter((n) => F.entries.some((e) => e.name === n));
  S.fileSel = new Set(sel);
  // where we are: the card's top, then each folder down to this one
  let crumbs = '', at = '';
  for (const r of F.roots) if (F.path === r.path || F.path.startsWith(r.path + '/')) at = r.path;
  const root = F.roots.find((r) => r.path === at);
  crumbs += `<button class="link" type="button" data-dir="${esc(at)}">${esc(root ? root.name : 'Card')}</button>`;
  let acc = at;
  for (const part of F.path.slice(at.length).split('/').filter(Boolean)) { acc += '/' + part; crumbs += ` › <button class="link" type="button" data-dir="${esc(acc)}">${esc(part)}</button>`; }
  const one = sel.length === 1 ? F.entries.find((e) => e.name === sel[0]) : null;
  $('#libSummary').textContent = `${plural(F.entries.length, 'item')} in ${relName(F.path)}`;
  libList.innerHTML = `<div class="files-head">
      <div class="crumbs">${crumbs}${F.roots.length > 1 ? `<span class="crumb-roots">${F.roots.filter((r) => r.path !== at).map((r) => `<button class="btn btn-sm btn-quiet" type="button" data-dir="${esc(r.path)}">${esc(r.name)}</button>`).join('')}</span>` : ''}</div>
      <div class="files-acts">${sel.length ? `<span class="sel-count">${sel.length} selected</span>
          ${one ? `<button class="btn btn-sm" type="button" data-fa="rename">Rename</button>` : ''}
          ${one && one.kind !== 'folder' ? `<button class="btn btn-sm" type="button" data-fa="download">Download</button>` : ''}
          <button class="btn btn-sm" type="button" data-fa="move">Move…</button>
          <button class="btn btn-sm btn-red-text" type="button" data-fa="delete">${icon('trash')}<span>Delete</span></button>
          <button class="btn btn-sm btn-quiet" type="button" data-fa="none">Clear</button>`
        : `<button class="btn btn-sm" type="button" data-fa="folder">${icon('plus')}<span>New folder</span></button>
          <button class="btn btn-sm" type="button" data-fa="upload">${icon('upload')}<span>Upload here</span></button>`}</div></div>
    ${F.parent ? `<div class="song file-row" role="listitem" data-dir="${esc(F.parent)}"><span class="fsel"></span><span class="art">${icon('up')}</span><span class="s-main"><span class="s-title">Up a folder</span></span></div>` : ''}
    ${list.length ? list.map((e) => `<div class="song file-row${S.fileSel.has(e.name) ? ' is-sel' : ''}" role="listitem" data-name="${esc(e.name)}" data-kind="${e.kind}">
        <label class="fsel"><input type="checkbox" ${S.fileSel.has(e.name) ? 'checked' : ''} aria-label="Select ${esc(e.name)}"></label>
        <span class="art">${icon(FILE_ICON[e.kind] || 'info')}</span>
        <span class="s-main"><span class="s-title">${esc(e.name)}</span><span class="s-sub">${esc(e.kind === 'folder' ? 'Folder' : [e.title && e.title !== e.name ? e.title : '', e.artist, fmtBytes(e.size)].filter(Boolean).join(' · '))}</span></span>
        <span class="s-time">${new Date(e.modified).toLocaleDateString(undefined, { dateStyle: 'medium' })}</span></div>`).join('')
      : `<div class="lib-empty"><h3>${q ? 'No matches' : 'An empty folder'}</h3><p>${q ? 'Nothing here matches.' : 'Upload files here, or move some in.'}</p></div>`}`;
}
async function fileAction(act) {
  const F = S.files, sel = [...S.fileSel], paths = sel.map((n) => `${F.path}/${n}`);
  if (act === 'none') { S.fileSel.clear(); return renderFiles(); }
  if (act === 'upload') {
    const input = document.createElement('input');
    input.type = 'file'; input.multiple = true;
    input.onchange = () => enqueue([...input.files], F.path);
    return input.click();
  }
  if (act === 'folder') {
    const name = await promptSheet({ title: 'New folder', text: `In ${esc(relName(F.path))}.`, action: 'Make', placeholder: 'Name' });
    if (!name) return;
    try { S.files = await api('POST', '/api/files/folder', { path: F.path, name }); renderFiles(); toast(`Made “${name}”`); }
    catch (e) { if (e.status !== 401) toast(`Not made. ${e.code && e.code !== 'pair' ? e.code : reason(e)}`, 'err'); }
    return;
  }
  if (act === 'rename') {
    const name = await promptSheet({ title: 'Rename', value: sel[0], action: 'Rename' });
    if (!name || name === sel[0]) return;
    try { S.files = await api('POST', '/api/files/rename', { path: paths[0], name }); S.fileSel = new Set(); renderFiles(); toast('Renamed'); }
    catch (e) { if (e.status !== 401) toast(`Not renamed. ${e.code && e.code !== 'pair' ? e.code : reason(e)}`, 'err'); }
    return;
  }
  if (act === 'download') {
    const a = document.createElement('a');
    a.href = `/api/files/download?path=${encodeURIComponent(paths[0])}`;
    a.download = sel[0];
    document.body.append(a); a.click(); a.remove();
    return;
  }
  if (act === 'delete') {
    const folders = sel.filter((n) => F.entries.find((e) => e.name === n)?.kind === 'folder').length;
    const what = sel.length === 1 ? `“${esc(sel[0])}”` : plural(sel.length, 'item');
    if (!(await confirmSheet({ title: `Delete ${what}?`, text: `${folders ? 'Folders go with everything in them. ' : ''}This can’t be undone.`, action: 'Delete' }))) return;
    try {
      const r = await api('POST', '/api/files/delete', { paths }, { timeout: 120000 });
      if (r.errors.length) toast(`${plural(r.errors.length, 'file')} couldn’t be deleted`, 'err'); else toast(`Deleted ${what.replace(/&quot;/g, '"')}`);
      loadFiles(F.path, true); loadLibrary(true);
    } catch (e) { if (e.status !== 401) toast(`Not deleted. ${reason(e)}`, 'err'); }
    return;
  }
  if (act === 'move') return moveSheet(paths);
}
// a folder to move into: browse the folders, then Move here
async function moveSheet(paths) {
  let dir = S.files.path;
  const draw = async () => {
    let L;
    try { L = await api('GET', `/api/files?path=${encodeURIComponent(dir)}`); } catch (e) { return; }
    const folders = L.entries.filter((e) => e.kind === 'folder').sort((a, b) => collator.compare(a.name, b.name));
    openSheet(`<h2>Move ${plural(paths.length, 'item')}</h2><p>Into <b>${esc(relName(L.path))}</b></p>
      <div class="org-list mv-list">${L.parent ? `<button type="button" class="mv-dir" data-to="${esc(L.parent)}">${icon('up')}<span>Up a folder</span></button>` : ''}
        ${folders.map((e) => `<button type="button" class="mv-dir" data-to="${esc(L.path + '/' + e.name)}"${paths.includes(L.path + '/' + e.name) ? ' disabled' : ''}>${icon('folder')}<span>${esc(e.name)}</span></button>`).join('') || '<div class="row-empty">No folders in here.</div>'}</div>
      <div class="sheet-actions"><button class="btn" type="button" data-r="0">Cancel</button><button class="btn btn-primary" type="button" data-r="here"${L.path === S.files.path ? ' disabled' : ''}>Move here</button></div>`);
    $('[data-r="0"]', sheet).addEventListener('click', () => sheet.close());
    $$('.mv-dir', sheet).forEach((b) => b.addEventListener('click', () => { dir = b.dataset.to; draw(); }));
    $('[data-r="here"]', sheet).addEventListener('click', async () => {
      try {
        const r = await api('POST', '/api/files/move', { paths, to: L.path }, { timeout: 120000 });
        sheet.close();
        if (r.errors.length) toast(r.errors[0], 'err');
        else if (r.background) toast(`Moving to ${relName(L.path)} in the background (between the card and the internal storage)`);
        else toast(`Moved ${plural(r.moved, 'item')} to ${relName(L.path)}`);
        if (r.background) setTimeout(() => { loadFiles(S.files.path, true); loadLibrary(true); }, 8000);
        S.fileSel = new Set();
        loadFiles(S.files.path, true); loadLibrary(true);
      } catch (e) { if (e.status !== 401) toast(`Not moved. ${reason(e)}`, 'err'); }
    });
  };
  draw();
}
libList.addEventListener('click', (e) => {
  if (S.libKind !== 'files') return;
  e.stopImmediatePropagation();
  const fa = e.target.closest('[data-fa]');
  if (fa) return fileAction(fa.dataset.fa);
  const go = e.target.closest('[data-dir]');
  if (go) return loadFiles(go.dataset.dir);
  const row = e.target.closest('.file-row[data-name]');
  if (!row) return;
  const name = row.dataset.name;
  if (e.target.closest('.fsel')) {  // the checkbox: selected or not
    const box = $('input', row);
    if (e.target !== box) box.checked = !box.checked;
    if (box.checked) S.fileSel.add(name); else S.fileSel.delete(name);
    return renderFiles();
  }
  const ent = S.files.entries.find((x) => x.name === name);
  if (ent?.kind === 'folder') return loadFiles(`${S.files.path}/${name}`);
  if (ent?.kind === 'song') { const it = S.libByPath?.get(`${S.files.path}/${name}`); if (it) return openItem(it); }
  // anything else: selected
  if (S.fileSel.has(name)) S.fileSel.delete(name); else S.fileSel.add(name);
  renderFiles();
}, true);

// ---------- Albums: Cover Flow (Library › Cover Flow on the Rondo) ----------
// The albums' covers in perspective, the one in the middle facing you; the wheel, the arrow keys, a drag
// or a click move through them, and the album's songs are listed below, to play on the Rondo.
function albumsOf() {
  const L = S.library;
  if (!L) return [];
  if (S.albumsFor === L) return S.albums;
  const by = new Map();
  for (const s of L.songs) {
    if (!s.album) continue;
    const key = fold(s.album) + '\u001f' + fold(s.albumArtist || '');
    let a = by.get(key);
    if (!a) by.set(key, (a = { name: s.album, artist: s.albumArtist || s.artist, songs: [], years: {} }));
    a.songs.push(s);
    if (s.year) a.years[s.year] = (a.years[s.year] || 0) + 1;
  }
  S.albums = [...by.values()].sort((a, b) => collator.compare(a.name, b.name));
  for (const a of S.albums) {
    a.songs.sort((x, y) => (x.disc || 0) - (y.disc || 0) || (x.track || 0) - (y.track || 0) || collator.compare(x.title, y.title));
    a.year = +Object.entries(a.years).sort((x, y) => y[1] - x[1])[0]?.[0] || 0;
    a.cover = a.songs.find((x) => x.art)?.path || a.songs[0].path;
  }
  S.albumsFor = L;
  return S.albums;
}
function renderAlbums() {
  if (!S.library) { libList.innerHTML = '<div class="lib-skel"></div>'.repeat(4); return; }
  const q = fold(S.libQuery.trim());
  const all = albumsOf().filter((a) => !q || fold(a.name + ' ' + a.artist).includes(q));
  S.cfList = all;
  S.cfAt = Math.max(0, Math.min(all.length - 1, S.cfAt || 0));
  $('#libSummary').textContent = all.length ? `${plural(all.length, 'album')} on the card` : 'No albums';
  if (!all.length) { libList.innerHTML = '<div class="lib-empty"><h3>No albums</h3><p>Songs tagged with an album show up here.</p></div>'; return; }
  libList.innerHTML = `<div class="cf" id="cf" tabindex="0" aria-label="Cover Flow: arrow keys move through the albums">
      <button class="cf-nav prev" type="button" data-cf="-1" aria-label="Previous album">${icon('chevron', 'i flip')}</button>
      <button class="cf-nav next" type="button" data-cf="1" aria-label="Next album">${icon('chevron')}</button>
      <div id="cfItems"></div><div class="cf-caption"><h3 id="cfTitle"></h3><p id="cfSub"></p></div></div>
    <div class="cf-songs" id="cfSongs"></div>`;
  layoutFlow(true);
}
// the covers near the middle, each turned and set back by how far from it it is
function layoutFlow(rebuild = false) {
  const all = S.cfList, at = S.cfAt, box = $('#cfItems');
  if (!box) return;
  const near = [];
  for (let i = Math.max(0, at - 8); i <= Math.min(all.length - 1, at + 8); i++) near.push(i);
  // the elements kept by index, so a move animates (they're made only for the covers near the middle)
  const have = new Map([...box.children].map((el) => [+el.dataset.i, el]));
  for (const [i, el] of have) if (!near.includes(i)) el.remove();
  // the covers as large as the pane allows (240 px wide open), the spacing in step with them
  const W = box.parentElement.clientWidth, size = W < 480 ? 150 : W < 760 ? 190 : 240, k = size / 240;
  for (const i of near) {
    let el = have.get(i);
    if (!el) {
      el = document.createElement('div');
      el.className = 'cf-item';
      el.dataset.i = i;
      el.innerHTML = `<div class="cf-cover">${icon('note')}</div>`;
      const img = new Image();
      img.onload = () => { const c = $('.cf-cover', el); c.style.backgroundImage = `url("${img.src}")`; c.innerHTML = ''; };
      img.src = artUrl(all[i].cover, 300);
      box.append(el);
    }
    const d = i - at, ad = Math.abs(d), side = Math.sign(d);
    // the middle one forward, its neighbors well clear of it and turned, each further one a step back
    el.style.width = el.style.height = `${size}px`;
    el.style.marginLeft = `${-size / 2}px`;
    const x = ad === 0 ? 0 : side * (210 + (ad - 1) * 66) * k;
    const turn = ad === 0 ? 0 : -side * 62, z = (ad === 0 ? 90 : -120 - ad * 10) * k;
    el.style.transform = `translateX(${x}px) translateZ(${z}px) rotateY(${turn}deg)`;
    el.style.zIndex = String(1000 - ad);
    el.style.opacity = ad > 7 ? '0' : String(Math.max(0.55, 1 - ad * 0.06));
  }
  const a = all[at];
  $('#cfTitle').textContent = a.name;
  $('#cfSub').textContent = [a.artist, a.year || '', `${at + 1} of ${all.length}`].filter(Boolean).join(' · ');
  clearTimeout(S.cfSongsTimer);
  S.cfSongsTimer = setTimeout(() => {  // the songs once the glide stops
    const songs = $('#cfSongs');
    if (!songs || S.cfList[S.cfAt] !== a) return;
    songs.innerHTML = `<div class="lhdr" role="heading" aria-level="3">${esc(a.name)}<small>${esc(plural(a.songs.length, 'song'))}</small></div>` +
      a.songs.map((s) => rowHtml(s)).join('');
    $$('.art[data-art]', songs).forEach((el) => artIO.observe(el));
    markPlayingRows();
  }, rebuild ? 0 : 260);
}
function moveFlow(by) {
  const n = S.cfList?.length || 0;
  const next = Math.max(0, Math.min(n - 1, S.cfAt + by));
  if (next === S.cfAt) return;
  S.cfAt = next;
  layoutFlow();
}
libList.addEventListener('click', (e) => {
  if (S.libKind !== 'albums') return;
  const nav = e.target.closest('[data-cf]');
  if (nav) { e.stopImmediatePropagation(); return moveFlow(+nav.dataset.cf); }
  const item = e.target.closest('.cf-item');
  if (item) { e.stopImmediatePropagation(); S.cfAt = +item.dataset.i; return layoutFlow(); }
  // a song row below: the library's own handler (play, details)
}, true);
libList.addEventListener('keydown', (e) => {
  if (S.libKind !== 'albums' || !e.target.closest('#cf')) return;
  if (e.key === 'ArrowLeft') { e.preventDefault(); moveFlow(-1); }
  if (e.key === 'ArrowRight') { e.preventDefault(); moveFlow(1); }
  if (e.key === 'Home') { e.preventDefault(); moveFlow(-1e9); }
  if (e.key === 'End') { e.preventDefault(); moveFlow(1e9); }
});
let cfWheel = 0;
libList.addEventListener('wheel', (e) => {
  if (S.libKind !== 'albums' || !e.target.closest('#cf')) return;
  e.preventDefault();
  cfWheel += Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.deltaY;
  while (Math.abs(cfWheel) >= 60) { moveFlow(Math.sign(cfWheel)); cfWheel -= Math.sign(cfWheel) * 60; }
}, { passive: false });
let cfDrag = null;
libList.addEventListener('pointerdown', (e) => { if (S.libKind === 'albums' && e.target.closest('#cf') && !e.target.closest('button')) cfDrag = { x: e.clientX, moved: 0 }; });
window.addEventListener('pointermove', (e) => {
  if (!cfDrag) return;
  const dx = e.clientX - cfDrag.x;
  if (Math.abs(dx) >= 70) { moveFlow(dx < 0 ? 1 : -1); cfDrag.x = e.clientX; cfDrag.moved++; }
});
window.addEventListener('pointerup', () => { cfDrag = null; });


// ---------- Photos (Library › Photos on the Rondo) ----------
// Every picture on the card by folder, thumbnails the Rondo makes (turned as the camera says), and a
// lightbox: arrow keys or the buttons go through them, Space starts a slideshow, Esc closes.
async function loadPhotos() {
  if (!S.photos) libList.innerHTML = '<div class="lib-skel"></div>'.repeat(4);
  try { S.photos = (await api('GET', '/api/photos', undefined, { timeout: 60000 })).photos; } catch (e) {
    if (e.status !== 401 && !S.photos) libList.innerHTML = `<div class="lib-empty"><h3>Couldn’t look for photos</h3><p>${esc(reason(e))}</p></div>`;
    return;
  }
  renderLibrary();
}
const photoUrl = (p, size) => `/api/photo?path=${encodeURIComponent(p)}&size=${size}`;
function renderPhotos() {
  const all = S.photos;
  if (!all) { libList.innerHTML = '<div class="lib-skel"></div>'.repeat(4); return; }
  const q = fold(S.libQuery.trim());
  const shown = q ? all.filter((p) => fold(p.path).includes(q)) : all;
  S.photosShown = shown;
  $('#libSummary').textContent = all.length ? `${plural(all.length, 'photo')} on your Rondo` : 'No photos';
  if (!shown.length) { libList.innerHTML = `<div class="lib-empty"><h3>${all.length ? 'No matches' : 'No photos'}</h3><p>${all.length ? 'No photo’s name or folder matches.' : 'Upload JPEG or PNG pictures in the Files tab, or copy them to the card.'}</p></div>`; return; }
  const groups = [];
  for (let i = 0; i < shown.length; i++) {
    const dir = shown[i].path.slice(0, shown[i].path.lastIndexOf('/'));
    let g = groups.find((x) => x.dir === dir);
    if (!g) groups.push((g = { dir, items: [] }));
    g.items.push(i);
  }
  libList.innerHTML = groups.map((g) => `<div class="lhdr" role="heading" aria-level="3">${esc(relName(g.dir))}<small>${esc(plural(g.items.length, 'photo'))}</small></div>
    <div class="ph-grid">${g.items.map((i) => `<button class="ph-cell" type="button" data-ph="${i}" aria-label="${esc(fileName(shown[i].path))}"><img alt="" loading="lazy" decoding="async" src="${photoUrl(shown[i].path, 320)}" onload="this.classList.add('ok')" onerror="artRetry(this)"></button>`).join('')}</div>`).join('');
}
libList.addEventListener('click', (e) => {
  if (S.libKind !== 'photos') return;
  const cell = e.target.closest('[data-ph]');
  if (!cell) return;
  e.stopImmediatePropagation();
  openLightbox(+cell.dataset.ph);
}, true);
let lb = null;
function openLightbox(i) {
  if (!lb) {
    lb = document.createElement('div');
    lb.className = 'lightbox';
    lb.setAttribute('role', 'dialog');
    lb.setAttribute('aria-label', 'Photo');
    lb.innerHTML = `<div class="lb-stage"><button class="lb-nav prev" type="button" data-lb="-1" aria-label="Previous photo">${icon('chevron', 'i flip')}</button>
      <button class="lb-nav next" type="button" data-lb="1" aria-label="Next photo">${icon('chevron')}</button></div>
      <div class="lb-bar"><span class="lb-name"></span><span class="lb-meta"></span>
        <button class="btn btn-sm" type="button" data-lb="show">${icon('play')}<span>Slideshow</span></button>
        <a class="btn btn-sm" data-lb="save" download>Download</a>
        <button class="btn btn-sm" type="button" data-lb="close">${icon('close')}<span>Close</span></button></div>`;
    document.body.append(lb);
    lb.addEventListener('click', (e) => {
      const b = e.target.closest('[data-lb]');
      if (!b) { if (e.target === lb || e.target.classList.contains('lb-stage')) closeLightbox(); return; }
      if (b.dataset.lb === 'close') closeLightbox();
      else if (b.dataset.lb === 'show') toggleShow();
      else if (b.dataset.lb !== 'save') lbGo(lb._i + +b.dataset.lb, +b.dataset.lb);
    });
    // a swipe on a touch screen: the photo follows the finger, then goes on or springs back
    let swipe = null;
    const stage = $('.lb-stage', lb);
    stage.addEventListener('pointerdown', (e) => { if (e.pointerType !== 'mouse' && !e.target.closest('button')) swipe = { x: e.clientX, id: e.pointerId }; });
    stage.addEventListener('pointermove', (e) => {
      if (!swipe || e.pointerId !== swipe.id) return;
      const cur = $('img.lb-cur', lb);
      if (cur) cur.style.transform = `translateX(${e.clientX - swipe.x}px)`;
    });
    const end = (e) => {
      if (!swipe || e.pointerId !== swipe.id) return;
      const dx = e.clientX - swipe.x, cur = $('img.lb-cur', lb);
      swipe = null;
      if (Math.abs(dx) > 60) return lbGo(lb._i + (dx < 0 ? 1 : -1), dx < 0 ? 1 : -1, false, dx);
      if (cur) cur.animate([{ transform: `translateX(${dx}px)` }, { transform: 'none' }], { duration: 220, easing: 'cubic-bezier(.2,.7,.2,1)' }).onfinish = () => { cur.style.transform = ''; };
    };
    stage.addEventListener('pointerup', end);
    stage.addEventListener('pointercancel', end);
    document.addEventListener('keydown', (e) => {
      if (!lb || lb.hidden) return;
      if (e.key === 'Escape') closeLightbox();
      else if (e.key === 'ArrowLeft') lbGo(lb._i - 1, -1);
      else if (e.key === 'ArrowRight') lbGo(lb._i + 1, 1);
      else if (e.key === ' ') { e.preventDefault(); toggleShow(); }
    });
  }
  const opening = lb.hidden || lb._i === undefined;
  lb.hidden = false;
  lb._i = undefined;
  if (opening && !reduceMotion()) lb.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 200 });
  lbGo(i, 0);
}
const reduceMotion = () => matchMedia('(prefers-reduced-motion: reduce)').matches;
const viewUrl = (p) => `/api/files/view?path=${encodeURIComponent(p)}`;
// One photo after another: it slides in from the side gone toward (dir 1: the next, from the right),
// the one before slides out the other way; the slideshow crossfades; the first opens with a little zoom.
// from: where a swipe left the photo before (px).
function lbGo(i, dir = 0, fade = false, from = 0) {
  const all = S.photosShown || [];
  if (!all.length) return;
  i = (i + all.length) % all.length;
  if (i === lb._i) return;
  lb._i = i;
  const p = all[i], stage = $('.lb-stage', lb), old = $('img.lb-cur', lb);
  const img = document.createElement('img');
  img.alt = '';
  img.className = 'lb-cur';
  img.decoding = 'async';
  // the original, at its size (the browser turns it as its camera says); the Rondo's 1600 px one if it can't be shown
  img.onerror = () => { if (!img.src.includes('/api/photo?')) img.src = photoUrl(p.path, 1600); };
  img.src = viewUrl(p.path);
  stage.prepend(img);
  const still = reduceMotion();
  const ease = 'cubic-bezier(.2,.7,.2,1)', ms = 340;
  if (old) {
    old.classList.remove('lb-cur');
    if (still || (!dir && !fade)) old.remove();
    else if (fade) old.animate([{ opacity: 1 }, { opacity: 0 }], { duration: 600, easing: 'ease' }).onfinish = () => old.remove();
    else old.animate([{ transform: `translateX(${from}px)`, opacity: 1 }, { transform: `translateX(${-dir * 100}%)`, opacity: 0.35 }], { duration: ms, easing: ease }).onfinish = () => old.remove();
  }
  if (!still) {
    if (fade) img.animate([{ opacity: 0 }, { opacity: 1 }], { duration: 600, easing: 'ease' });
    else if (dir && old) img.animate([{ transform: `translateX(${dir * 100}%)` }, { transform: 'none' }], { duration: ms, easing: ease });
    else img.animate([{ opacity: 0, transform: 'scale(.96)' }, { opacity: 1, transform: 'none' }], { duration: 260, easing: ease });
  }
  // the neighbors fetched ahead: the next turn has its photo at once
  for (const k of [i + 1, i - 1]) { const n = all[(k + all.length) % all.length]; if (n && n !== p) { const pre = new Image(); pre.decoding = 'async'; pre.src = viewUrl(n.path); } }
  $('.lb-name', lb).textContent = fileName(p.path);
  $('.lb-meta', lb).textContent = `${new Date(p.modified).toLocaleDateString(undefined, { dateStyle: 'medium' })} · ${i + 1} of ${all.length}`;
  const save = $('[data-lb="save"]', lb);
  save.href = `/api/files/download?path=${encodeURIComponent(p.path)}`;
  save.setAttribute('download', fileName(p.path));
}
let showTimer = 0;
function toggleShow() {
  const b = $('[data-lb="show"] span', lb);
  if (showTimer) { clearInterval(showTimer); showTimer = 0; b.textContent = 'Slideshow'; return; }
  showTimer = setInterval(() => lbGo(lb._i + 1, 1, true), 4000);
  b.textContent = 'Stop';
}
function closeLightbox() {
  if (showTimer) toggleShow();
  lb.hidden = true;
}
