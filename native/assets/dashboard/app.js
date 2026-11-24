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

// ---------- language (lang/*.json, one file per language: docs/translating.md) ----------
// t(key, {name: …, count: n}) is a text in the Rondo's language with its blanks filled ("count"
// also picks the plural form); te() is the same escaped for HTML, th() keeps the text's own <b>, <i>,
// <code> and <br> (its blanks escaped). Elements with data-i18n (text), data-i18n-html and
// data-i18n-attr="aria-label:key;title:key" get theirs from applyLang().
const L = { code: 'en', name: 'English', languages: [], strings: {} };
const plurals = () => (L._pr && L._pr.code === L.code ? L._pr.rules : (L._pr = { code: L.code, rules: new Intl.PluralRules(L.code) }).rules);
const numbers = () => (L._nf && L._nf.code === L.code ? L._nf.format : (L._nf = { code: L.code, format: new Intl.NumberFormat(L.code) }).format);
function t(key, params) {
  let v = L.strings[key];
  if (v == null) return key;
  if (typeof v === 'object') {
    const n = params?.count;
    const form = typeof n === 'number' ? (n === 0 && v.zero != null ? 'zero' : plurals().select(n)) : 'other';
    v = v[form] ?? v.other;
  }
  return params ? v.replace(/\{(\w+)\}/g, (m, k) => (k in params ? (typeof params[k] === 'number' ? numbers().format(params[k]) : String(params[k])) : m)) : v;
}
const te = (key, params) => esc(t(key, params));
// HTML from a text: escaped, then its simple markup back; a blank given as html(…) goes in as it is
const html = (s) => ({ html: String(s) });
function th(key, params) {
  const marks = [];
  const safe = params && Object.fromEntries(Object.entries(params).map(([k, v]) => [k, v && typeof v === 'object' && 'html' in v ? (marks.push(v.html), `\u0001${marks.length - 1}\u0001`) : v]));
  return esc(t(key, safe)).replace(/&lt;(\/?)(b|i|em|strong|code|br)&gt;/g, '<$1$2>').replace(/\u0001(\d+)\u0001/g, (m, i) => marks[+i]);
}
function applyLang(root = document) {
  for (const el of root.querySelectorAll('[data-i18n]')) el.textContent = t(el.dataset.i18n);
  for (const el of root.querySelectorAll('[data-i18n-html]')) el.innerHTML = th(el.dataset.i18nHtml);
  for (const el of root.querySelectorAll('[data-i18n-attr]'))
    for (const pair of el.dataset.i18nAttr.split(';')) {
      const [a, k] = pair.split(':').map((s) => s.trim());
      if (a && k) el.setAttribute(a, t(k));
    }
}

const store = {
  get(k) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k, v) { try { v == null ? localStorage.removeItem(k) : localStorage.setItem(k, v); } catch { /* private mode */ } },
};

// the Rondo's language (its page says it: <html lang>), with English where it has no text; tried again
// every few seconds while the Rondo can't be reached (the page's own English shows meanwhile)
async function loadLang() {
  for (;;) {
    try {
      const want = document.documentElement.lang || 'en';
      const res = await fetch(`/lang/${encodeURIComponent(want)}.json`, { cache: 'no-cache' });
      if (res.ok) {
        Object.assign(L, await res.json());
        document.documentElement.lang = L.code;
        applyLang();
        return;
      }
    } catch { /* not reachable now */ }
    $('#splash')?.setAttribute('hidden', '');
    $('#offline')?.removeAttribute('hidden');
    // the rest of the page isn't running yet: its Try again tries this again
    await new Promise((r) => {
      const again = () => { clearTimeout(timer); r(); };
      const timer = setTimeout(() => { $('#offlineRetry')?.removeEventListener('click', again); r(); }, 4000);
      $('#offlineRetry')?.addEventListener('click', again, { once: true });
    });
  }
}
await loadLang();

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
// dates in the page's language (English: the browser's own format, as before)
const dateLocale = () => (L.code === 'en' ? undefined : L.code);
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
// YouTube's, YouTube Music's and WhatsApp's marks in white on their own colors, as on the Y1's menu
// (YouTube's play mark cut out in its tile's red)
const MARK = {
  video: (cls) => `<svg class="${cls} yt" viewBox="0 0 28.57 20" aria-hidden="true"><path fill="currentColor" d="M27.9727 3.12324C27.6435 1.89323 26.6768 .926623 25.4468 .597366C23.2197 0 14.285 0 14.285 0C14.285 0 5.35042 0 3.12323 .597366C1.89323 .926623 .926623 1.89323 .597366 3.12324C0 5.35042 0 10 0 10C0 10 0 14.6496 .597366 16.8768C.926623 18.1068 1.89323 19.0734 3.12323 19.4026C5.35042 20 14.285 20 14.285 20C14.285 20 23.2197 20 25.4468 19.4026C26.6768 19.0734 27.6435 18.1068 27.9727 16.8768C28.5701 14.6496 28.5701 10 28.5701 10C28.5701 10 28.5677 5.35042 27.9727 3.12324Z"/><path class="cut" d="M11.4253 14.2854L18.8477 10.0004L11.4253 5.71533Z"/></svg>`,
  music: (cls) => `<svg class="${cls}" viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="10.2" fill="none" stroke="currentColor" stroke-width="2.3"/><path fill="currentColor" d="M9.3 16.4L16.9 12L9.3 7.6Z"/></svg>`,
  whatsapp: (cls) => `<svg class="${cls}" viewBox="0 0 24 24" aria-hidden="true"><path fill="currentColor" d="M17.472 14.382c-.297-.149-1.758-.867-2.03-.967-.273-.099-.471-.148-.67.15-.197.297-.767.966-.94 1.164-.173.199-.347.223-.644.075-.297-.15-1.255-.463-2.39-1.475-.883-.788-1.48-1.761-1.653-2.059-.173-.297-.018-.458.13-.606.134-.133.298-.347.446-.52.149-.174.198-.298.298-.497.099-.198.05-.371-.025-.52-.075-.149-.669-1.612-.916-2.207-.242-.579-.487-.5-.669-.51-.173-.008-.371-.01-.57-.01-.198 0-.52.074-.792.372-.272.297-1.04 1.016-1.04 2.479 0 1.462 1.065 2.875 1.213 3.074.149.198 2.096 3.2 5.077 4.487.709.306 1.262.489 1.694.625.712.227 1.36.195 1.871.118.571-.085 1.758-.719 2.006-1.413.248-.694.248-1.289.173-1.413-.074-.124-.272-.198-.57-.347z m-5.421 7.403h-.004a9.87 9.87 0 01-5.031-1.378l-.361-.214-3.741.982.998-3.648-.235-.374a9.86 9.86 0 01-1.51-5.26c.001-5.45 4.436-9.884 9.888-9.884 2.64 0 5.122 1.03 6.988 2.898a9.825 9.825 0 012.893 6.994c-.003 5.45-4.437 9.884-9.885 9.884m8.413-18.297A11.815 11.815 0 0012.05 0C5.495 0 .16 5.335.157 11.892c0 2.096.547 4.142 1.588 5.945L.057 24l6.305-1.654a11.882 11.882 0 005.683 1.448h.005c6.554 0 11.89-5.335 11.893-11.893a11.821 11.821 0 00-3.48-8.413z"/></svg>`,
};
const BRAND_CLASS = { video: 't-youtube', music: 't-ytmusic', whatsapp: 't-whatsapp' };
const brandTile = (brand, cls = 'tile') => `<span class="${cls} ${BRAND_CLASS[brand]}">${MARK[brand]('brand-glyph')}</span>`;

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
const reason = (e) => (e?.status === 0 ? t('common.rondoNoAnswer') : e?.code && e.code !== 'pair' ? t('common.itSaid', { code: e.code }) : t('common.somethingWrong'));

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
    $('#bannerText').textContent = S.joining ? t('banner.joining', { name: S.joining.ssid }) : t('banner.lostOpen');
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
  codeMsg.textContent = t('common.checking');
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
  if (!res) codeMsg.textContent = t('pair.cantReach');
  else if (res.status === 429) {
    wrongCodes = 0;
    codeMsg.textContent = t('pair.tooMany', { seconds: data?.retryAfterSec ?? 30 });
  } else if (res.status === 403) {
    wrongCodes++;
    codeMsg.textContent = t('pair.wrong');
  } else codeMsg.textContent = t('pair.failed');
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
      $('#offlineNote').textContent = t('offline.error', { code: String(e.code || e.status) });
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
  $('#offlineNote').textContent = t('offline.trying');
  boot();
});

// ---------- navigation ----------
// the sidebar in groups, as on the Rondo: what's on it, how it connects, how it behaves
const NAV = [
  { id: 'overview', label: t('nav.overview'), tile: 'rondo', tab: true },
  { id: 'library', label: t('common.library'), tile: 'device', glyph: 'library', tab: true },
  { id: 'radio', label: t('nav.fmRadio'), short: t('common.radio'), tile: 'radio', glyph: 'radio', tab: true },
  { id: 'youtube', label: 'YouTube', brand: 'video', tab: true },
  { group: t('nav.connections') },
  { id: 'wifi', label: 'Wi-Fi', tile: 'wifi', glyph: 'wifi' },
  { id: 'bluetooth', label: 'Bluetooth', tile: 'bluetooth', glyph: 'bluetooth' },
  { id: 'server', label: t('nav.server'), tile: 'server', glyph: 'cloud' },
  { group: t('common.settings') },
  { id: 'sound', label: t('nav.sound'), tile: 'sound', glyph: 'sliders' },
  { id: 'theme', label: t('nav.theme'), tile: 'theme', glyph: 'theme' },
  { id: 'settings', label: t('nav.general'), tile: 'settings', glyph: 'gear' },
];
// the phone's fifth tab: the pages without a tab of their own, in the sidebar's groups
const MORE = { id: 'more', label: t('common.settings'), tile: 'settings', glyph: 'gear' };
const PAGES = [...NAV.filter((n) => n.id), MORE];
const TABS = [...NAV.filter((n) => n.tab), MORE];
const inMore = (id) => !TABS.some((n) => n.id === id && n !== MORE);
const tileHtml = (tile, glyph) => `<span class="tile t-${tile}">${glyph ? icon(glyph) : markTile}</span>`;
const navTile = (n) => (n.brand ? brandTile(n.brand) : tileHtml(n.tile, n.glyph));  // YouTube's logo for its page
function buildNav() {
  $('#nav').innerHTML = NAV.map((n) => n.group ? `<div class="nav-group">${esc(n.group)}</div>` : `<a href="#${n.id}" data-id="${n.id}">${navTile(n)}<span>${esc(n.label)}</span><span class="nav-badge"></span>${icon('chevron', 'chev')}</a>`).join('');
  $('#moreRows').innerHTML = NAV.filter((n) => !n.tab).map((n) => n.group ? `</div><h2 class="gh">${esc(n.group)}</h2><div class="group">`
    : `<a class="row more-row" href="#${n.id}">${navTile(n)}<div class="row-main"><div class="row-title">${esc(n.label)}</div></div>${icon('chevron', 'chev')}</a>`).join('').replace(/^<\/div>/, '') + '</div>';
  $('#tabs').innerHTML = TABS.map((n) => `<a class="tab" href="#${n.id}" data-id="${n.id}"><span class="tab-wrap">${navTile(n)}<span class="nav-badge"></span></span><span>${esc(n.short || n.label)}</span></a>`).join('');
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
  if (id === 'server') loadServer();
  if (id === 'youtube') { loadServer(); loadYtAccount(); loadDownloads(); if (!S.settings) loadSettings(); }
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
  const extra = p.playing ? (performance.now() - S.statusAt) * (p.speed || 100) / 100 : 0;
  return Math.min(p.positionMs + extra, p.durationMs || Infinity);
}

// ---------- header, sidebar ----------
function renderChrome() {
  const st = S.status;
  if (!st) return;
  const b = st.battery || {};
  const batt = `<span class="stat" title="${b.charging ? te('overview.batteryCharging') : te('overview.battery')}">${battSvg(b.level ?? 0, 'i', b.charging)}<span>${b.level ?? '–'}%</span></span>`;
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
  btn.setAttribute('aria-label', p.playing ? t('common.pause') : t('common.play'));
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
    $('#npTitle').textContent = t('overview.nothingPlaying');
    $('#npSub').textContent = t('overview.empty');
    setNpArt(null);
    renderNpExtras(null);
    return;
  }
  const bk = p.book;
  $('#npSrc').innerHTML = bk ? `${icon('book')}<span>${te('books.audiobook')}</span>${bk.speed !== 100 ? `<span class="np-speed">${esc(speedText(bk.speed))}</span>` : ''}`
    : p.source === 'youtube' ? `${icon('play')}<span>YouTube</span>` : `${icon('folder')}<span>${te('common.library')}</span>`;
  $('#npTitle').textContent = p.title || t('common.untitled');
  $('#npSub').textContent = bk ? [bk.author, bk.chapters ? bk.chapterTitle : '', bk.leftText].filter(Boolean).join(' · ') : p.subtitle || '';
  renderNpExtras(p);
  $('.np-art-glyph use').setAttribute('href', bk ? '#i-book' : '#i-note');  // what shows without artwork
  const tg = $('#npToggle');
  tg.innerHTML = icon(p.playing ? 'pause' : 'play');
  tg.setAttribute('aria-label', p.playing ? t('common.pause') : t('common.play'));
  tg.classList.toggle('is-paused', !p.playing);
  if (!volDragging && Date.now() > volHold) {
    npVol.max = p.maxVolume || 15;
    npVol.value = p.volume ?? 0;
    $('#npVolOut').textContent = p.volume ?? 0;
    setFill(npVol);
  }
  setNpArt(p.art);
  renderProgress();
}
// an audiobook's player shows its chapter, in listening time at its speed (as the Rondo does)
const bookPos = (p, pos) => (p.book ? Math.max(0, pos - p.book.chapterStartMs) * 100 / (p.speed || 100) : pos);
const bookLen = (p) => (p.book ? Math.max(0, p.book.chapterEndMs - p.book.chapterStartMs) * 100 / (p.speed || 100) : p.durationMs || 0);
function speedText(v) {
  const n = v / 100;
  return `${n.toLocaleString(L.code, { maximumFractionDigits: 2 })}×`;
}
function renderNpExtras(p) {
  const box = $('#npExtras');
  const bk = p?.book;
  const sleepNow = p?.sleep ? (p.sleep.mode === 'minutes' ? 'running' : 'end') : 'off';
  const opts = [['off', t('sleep.off')], ['15', t('sleep.minutes', { count: 15 })], ['30', t('sleep.minutes', { count: 30 })], ['45', t('sleep.minutes', { count: 45 })], ['60', t('sleep.minutes', { count: 60 })], ['end', p?.endName || t('sleep.end')]];
  if (sleepNow === 'running') opts.unshift(['running', t('sleep.left', { time: p.sleep.label })]);
  const sleep = `<label class="select"><select data-act="np-sleep" aria-label="${te('sleep.title')}">${opts.map(([v, l]) => `<option value="${v}"${v === sleepNow ? ' selected' : ''}>${esc(v === 'off' ? t('sleep.label', { state: l }) : l)}</option>`).join('')}</select>${icon('down')}</label>`;
  let html = sleep;
  if (bk) {
    const speeds = (S.books?.speeds || [75, 100, 125, 150, 175, 200, 250, 300].map((v) => ({ value: v, text: speedText(v) })))
      .map((o) => `<option value="${o.value}"${o.value === bk.speed ? ' selected' : ''}>${esc(t('books.speedIs', { speed: o.text }))}</option>`).join('');
    html = `<label class="select"><select data-act="np-speed" aria-label="${te('books.speed')}">${speeds}</select>${icon('down')}</label>` + html +
      `<button class="btn btn-sm" type="button" data-act="np-mark">${icon('bookmark')}<span>${te('books.addBookmark')}</span></button>` +
      (bk.musicWaits ? `<button class="btn btn-sm" type="button" data-act="np-music">${icon('note')}<span>${te('books.backToMusic')}</span></button>` : '');
  }
  const key = html;
  box.hidden = !p;
  np.classList.toggle('has-extras', !!p);
  if (box.dataset.key !== key && !box.contains(document.activeElement)) { box.dataset.key = key; box.innerHTML = html; }
}
$('#npExtras').addEventListener('change', async (e) => {
  const sel = e.target;
  const p = S.status?.player;
  try {
    if (sel.dataset.act === 'np-sleep' && sel.value !== 'running') {
      const v = sel.value;
      const res = await api('PUT', '/api/sleep', v === 'off' || v === 'end' ? { mode: v } : { mode: 'minutes', minutes: +v });
      if (S.status) { S.status.player = res; S.statusAt = performance.now(); }
      toast(v === 'off' ? t('sleep.stopped') : t('sleep.set'));
    } else if (sel.dataset.act === 'np-speed' && p?.book) {
      await api('PUT', '/api/books/speed', { key: p.book.key, speed: +sel.value });
      poll();
    }
  } catch (err) {
    if (err.status !== 401) toast(t('books.couldntDo', { reason: reason(err) }), 'err');
  }
  sel.blur();
  renderPlayer();
});
$('#npExtras').addEventListener('click', async (e) => {
  const act = e.target.closest('[data-act]')?.dataset.act;
  if (act !== 'np-mark' && act !== 'np-music') return;
  try {
    if (act === 'np-mark') { await api('POST', '/api/books/bookmark', {}); toast(t('books.bookmarkAdded')); }
    else { const res = await api('POST', '/api/books/music'); if (S.status) { S.status.player = res; S.statusAt = performance.now(); } renderPlayer(); renderMini(); }
  } catch (err) {
    if (err.status !== 401) toast(t('books.couldntDo', { reason: reason(err) }), 'err');
  }
});
function renderProgress() {
  const p = S.status?.player;
  if (!p) return;
  const pos = bookPos(p, position()), dur = bookLen(p);
  if (!seeking && S.route === 'overview') {
    npSeek.value = dur ? Math.round((pos / dur) * 1000) : 0;
    setFill(npSeek);
    $('#npPos').textContent = fmtTime(pos);
    $('#npRem').textContent = dur ? `−${fmtTime(remainMs(dur, pos))}` : '';
    npSeek.setAttribute('aria-valuetext', t('common.nOfTotal', { n: fmtTime(pos), total: fmtTime(dur) }));
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
    if (e.status !== 401) toast(t('overview.couldntControl', { reason: reason(e) }), 'err');
    poll();
  }
}
npSeek.addEventListener('input', () => {
  seeking = true;
  setFill(npSeek);
  const p = S.status?.player;
  const dur = p ? bookLen(p) : 0;  // an audiobook: its chapter, at its speed
  const pos = (npSeek.value / 1000) * dur;
  $('#npPos').textContent = fmtTime(pos);
  $('#npRem').textContent = `−${fmtTime(remainMs(dur, pos))}`;
});
npSeek.addEventListener('change', async () => {
  const p = S.status?.player;
  if (!p) { seeking = false; return; }
  const pos = p.book ? Math.round(p.book.chapterStartMs + (npSeek.value / 1000) * (p.book.chapterEndMs - p.book.chapterStartMs))
    : Math.round((npSeek.value / 1000) * p.durationMs);
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
  } catch (e) { if (e.status !== 401) toast(t('overview.couldntVolume', { reason: reason(e) }), 'err'); }
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
      <div class="row-main"><div class="row-title">${te('overview.battery')}</div>${b.charging ? `<div class="row-sub">${te('overview.charging')}</div>` : level <= 15 ? `<div class="row-sub">${te('overview.low')}</div>` : ''}</div>
      <div class="row-value strong">${level}%</div>
    </div>
    <div class="row">
      <span class="tile t-storage">${icon('folder')}</span>
      <div class="row-main">
        <div class="row-title">${te('overview.storage')}</div>
        <div class="meter ${freePct < 3 ? 'full' : freePct < 10 ? 'low' : ''}" role="img" aria-label="${te('overview.usedPct', { percent: Math.round(pct) })}"><span style="--p:${pct.toFixed(1)}%"></span></div>
        <div class="row-sub">${te('overview.usedOf', { used: fmtBytes(used), total: fmtBytes(s.totalBytes) })}</div>
      </div>
      <div class="row-value strong">${te('overview.free', { size: fmtBytes(s.freeBytes) })}</div>
    </div>
    <div class="row">
      <span class="tile t-wifi">${icon('wifi')}</span>
      <div class="row-main"><div class="row-title">${w ? esc(w.ssid) : 'Wi-Fi'}</div><div class="row-sub">${w ? `Wi-Fi · ${esc(w.ip || '')}` : te('common.notConnected')}${ab.mac ? ` · MAC ${esc(ab.mac)}` : ''}</div></div>
      ${w ? `<span class="row-value" aria-label="${te('common.signal', { signal: w.signal })}">${sigSvg(w.signal ?? 0)}</span>` : ''}
    </div>
    <div class="row">
      ${brandTile('video')}
      <div class="row-main"><div class="row-title">YouTube</div><div class="row-sub">${esc(ab.connectTo || '')}${ab.timeZone ? ` · ${te('overview.timeZone', { zone: ab.timeZone })}` : ''}</div></div>
    </div>
    <div class="row">
      <span class="tile t-rondo">${markTile}</span>
      <div class="row-main"><div class="row-title">${esc(d.name || 'Rondo')} ${esc(d.version || '')}</div><div class="row-sub">${esc(d.model || 'Innioasis Y1')}${d.canUpdate ? ` · <button class="link" type="button" data-update="file">${te('update.fromFile')}</button>` : ''}</div></div>
      ${d.power ? `<div class="bt-actions"><button class="btn btn-sm" type="button" data-power="restart">${te('power.restart')}</button><button class="btn btn-sm btn-red-text" type="button" data-power="shutdown">${icon('power')}<span>${te('power.shutDown')}</span></button></div>` : ''}
    </div>${newest && isNewer(newest.version, d.version) ? `
    <div class="row update-row">
      <span class="tile t-rondo">${icon('upload')}</span>
      <div class="row-main"><div class="row-title">${te('update.isOut', { version: newest.version })}</div><div class="row-sub">${needsFirmware(d) ? th('update.needsFirmware', { link: html(`<a href="${UPDATER_GUIDE}" target="_blank" rel="noopener">Innioasis Updater CE</a>`) }) : te('update.youHave', { version: d.version || '' })} <a href="${esc(newest.url)}" target="_blank" rel="noopener">${te('update.whatsNew')}</a></div></div>
      ${d.canUpdate && !needsFirmware(d) ? `<button class="btn btn-sm btn-primary" type="button" data-update="install">${te('update.install')}</button>` : ''}
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
  if (file) sendUpdate(file, null).catch((err) => toast(t('update.couldnt', { reason: err.message }), 'err'));
});
async function installUpdate(btn) {
  const label = btn.textContent;
  btn.disabled = true;
  try {
    btn.textContent = t('update.downloading');
    const info = await (await fetch(UPDATE_SITE + 'latest.json', { cache: 'no-store' })).json();
    if (newest && info.version !== newest.version) throw new Error(t('update.pageBehind', { version: String(info.version) }));
    if ((Number(info.firmware) || 1) > (S.status?.device?.firmware ?? 1)) throw new Error(t('update.needsUpdater', { version: String(info.version) }));
    const res = await fetch(UPDATE_SITE + (info.file || 'rondo.apk'), { cache: 'no-store' });
    if (!res.ok) throw new Error(t('update.downloadFailed', { status: String(res.status) }));
    await sendUpdate(await res.blob(), btn);
  } catch (e) {
    btn.disabled = false;
    btn.textContent = label;
    toast(t('update.couldnt', { reason: e.message || '' }), 'err');
  }
}
function sendUpdate(blob, btn) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('PUT', '/api/update');
    if (S.session) xhr.setRequestHeader('X-Rondo-Session', S.session);
    // an APK for 1.0.x, a .rondo bundle for Rondo 2.0
    xhr.setRequestHeader('Content-Type', /\.rondo$/.test(blob.name || '') || (S.status?.device?.firmware ?? 1) >= 2 ? 'application/octet-stream' : 'application/vnd.android.package-archive');
    xhr.upload.onprogress = (e) => { if (btn && e.lengthComputable) btn.textContent = t('update.sending', { percent: Math.round((e.loaded / e.total) * 100) }); };
    xhr.onload = () => {
      let data = null;
      try { data = JSON.parse(xhr.responseText); } catch { /* empty */ }
      if (xhr.status !== 200) return reject(new Error(data?.error || t('update.answered', { status: String(xhr.status) })));
      if (btn) btn.textContent = t('update.installing');
      openSheet(`<h2>${te('update.installingTitle', { version: data?.version || '' })}</h2><p>${th('update.installingText')}</p><div class="sheet-actions"><button class="btn btn-primary" type="button" data-r="ok">${te('common.ok')}</button></div>`);
      $('[data-r="ok"]', sheet).addEventListener('click', () => sheet.close(), { once: true });
      resolve();
    };
    xhr.onerror = () => reject(new Error(t('common.rondoNoAnswer')));
    xhr.send(blob);
  });
}
// ---------- a newer Rondo ----------
// Asked by this browser, not by the Rondo (which talks only to its Rondo server, or to YouTube directly), of the update
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
    title: off ? t('power.shutDownTitle') : t('power.restartTitle'),
    text: off ? te('power.shutDownText') : te('power.restartText'),
    action: off ? t('power.shutDown') : t('power.restart'),
  });
  if (!ok) return;
  try {
    await api('POST', '/api/power', { action: b.dataset.power });
    toast(off ? t('power.shuttingDown') : t('power.restarting'));
  } catch (e2) { if (e2.status !== 401) toast(t('power.couldnt', { reason: reason(e2) }), 'err'); }
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
    $('#libSummary').textContent = t('library.reading');
  }
  try {
    S.library = await api('GET', '/api/library', undefined, { timeout: 45000 });
  } catch (e) {
    if (e.status === 401) return;
    if (!S.library) {
      libList.innerHTML = `<div class="lib-empty"><h3>${te('library.couldntRead')}</h3><p>${esc(reason(e))}</p><button class="btn" type="button" data-act="retry">${te('common.tryAgain')}</button></div>`;
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
  const lib = S.library;
  if (!lib) return;
  const el = $('#libSummary');
  if (lib.scanning) { el.innerHTML = `<span class="scan-note">${spinner()}${te('library.scanning')}</span>`; return; }
  if (S.libKind === 'playlists') return;
  const list = S.libKind === 'songs' ? lib.songs : lib.videos;
  const bytes = list.reduce((n, x) => n + (x.size || 0), 0);
  const sum = { count: list.length, size: fmtBytes(bytes) };
  el.textContent = list.length
    ? (S.libKind === 'songs' ? t('library.summarySongs', sum) : t('library.summaryVideos', sum))
    : t('library.summary');
  $('#rescanBtn').disabled = false;
}
// sorts that only songs have (tags, listening)
const SONG_SORTS = ['artist', 'album', 'albumArtist', 'genre', 'year', 'plays', 'played', 'rating'];
const sortKey = () => (S.libKind === 'videos' && SONG_SORTS.includes(S.libSort) ? 'title' : S.libSort);
const stars = (n) => '★'.repeat(n) + '☆'.repeat(5 - n);
function dateGroup(ms) {
  if (!ms) return t('library.group.earlier');
  const now = new Date(), d = new Date(ms);
  const day = 86400000, start = new Date(now.getFullYear(), now.getMonth(), now.getDate()).getTime();
  if (ms >= start) return t('library.group.today');
  if (ms >= start - day) return t('library.group.yesterday');
  if (ms >= start - 6 * day) return t('library.group.thisWeek');
  if (ms >= start - 29 * day) return t('library.group.thisMonth');
  return d.toLocaleDateString(L.code, { month: 'long', year: 'numeric' });
}
function buildRows() {
  const lib = S.library, kind = S.libKind, sort = sortKey();
  const src = kind === 'songs' ? lib.songs : lib.videos;
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
    else if (sort === 'artist') label = it.artist || t('common.unknownArtist');
    else if (sort === 'album') { label = it.album || t('common.unknownAlbum'); sub = it.album ? [it.albumArtist || it.artist, it.year || ''].filter(Boolean).join(' · ') : ''; }
    else if (sort === 'albumArtist') label = it.albumArtist || it.artist || t('common.unknownArtist');
    else if (sort === 'genre') label = it.genre || t('library.noGenre');
    else if (sort === 'year') label = it.year ? String(it.year) : t('library.noYear');
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
  if (S.libKind === 'books') {
    libEl.classList.add('is-playlists');
    $('#libSortWrap').hidden = true;
    $('#libSearch').placeholder = t('books.search');
    libList.setAttribute('aria-label', t('books.title'));
    $('#view-library').classList.remove('is-empty');
    return renderBooks();
  }
  if (photosTab) {
    $('#libSortWrap').hidden = true;
    $('#libSearch').placeholder = t('library.searchPhotos');
    libList.setAttribute('aria-label', t('library.photos'));
    $('#view-library').classList.remove('is-empty');
    return renderPhotos();
  }
  if (albumsTab) {
    $('#libSortWrap').hidden = true;
    $('#libSearch').placeholder = t('library.searchAlbums');
    libList.setAttribute('aria-label', t('library.albums'));
    $('#view-library').classList.remove('is-empty');
    return renderAlbums();
  }
  if (files) {
    $('#libSortWrap').hidden = true;
    $('#libSearch').placeholder = t('library.searchFolder');
    libList.setAttribute('aria-label', t('library.files'));
    $('#view-library').classList.remove('is-empty');
    return renderFiles();
  }
  $('#libSortWrap').hidden = pls;
  if (pls) {
    $('#libSearch').placeholder = t('library.searchPlaylists');
    libList.setAttribute('aria-label', t('common.playlists'));
    $('#view-library').classList.remove('is-empty');
    return renderPlaylists();
  }
  const sel = $('#libSort');
  for (const o of sel.options) o.hidden = videos && SONG_SORTS.includes(o.value);
  sel.value = sortKey();
  $$('#libHead [data-sort]').forEach((b) => (b.dataset.sort === sortKey() ? b.setAttribute('aria-sort', 'ascending') : b.removeAttribute('aria-sort')));
  libList.setAttribute('aria-label', videos ? t('library.videos') : t('library.songs'));
  $('#libSearch').placeholder = videos ? t('library.searchVideos') : matchMedia('(min-width: 900px)').matches ? t('library.searchAll') : t('library.searchSongs');
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
  const LISTENING = { plays: [t('library.nothingPlayed'), t('library.nothingPlayedText')],
    played: [t('library.nothingPlayed'), t('library.nothingPlayedText')],
    rating: [t('library.nothingRated'), t('library.nothingRatedText')] };
  if (!count && total && !S.libQuery.trim() && LISTENING[sortKey()] && !videos) {
    const [h, p] = LISTENING[sortKey()];
    libList.innerHTML = `<div class="lib-empty"><h3>${esc(h)}</h3><p>${esc(p)}</p></div>`;
    return;
  }
  if (!count) {
    const query = S.libQuery.trim();
    libList.innerHTML = query && total
      ? `<div class="lib-empty"><h3>${te('common.noMatches')}</h3><p>${videos ? te('library.noMatchVideos', { query }) : te('library.noMatchSongs', { query })}</p></div>`
      : `<div class="lib-empty"><h3>${videos ? te('library.noVideos') : te('library.noMusic')}</h3><p>${videos ? te('library.noVideosText') : te('library.noMusicText')}</p><button class="btn btn-primary" type="button" data-act="upload">${icon('upload')}<span>${te('library.uploadFiles')}</span></button></div>`;
    return;
  }
  appendRows();
}
const CHUNK = 120;
function appendRows() {
  if (S.libKind !== 'songs' && S.libKind !== 'videos') return;  // the songs' rows stay out of the other tabs
  const end = Math.min(S.libShown + CHUNK, S.libRows.length);
  let out = '';
  for (let i = S.libShown; i < end; i++) out += rowHtml(S.libRows[i]);
  libList.insertAdjacentHTML('beforeend', out);
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
  const play = r._video ? '' : `<button class="ib ib-sm" type="button" data-act="play" aria-label="${te('common.playNameOnRondo', { name: title })}">${icon('play')}</button>`;
  const acts = `<span class="s-acts">${play}<button class="ib ib-sm" type="button" data-act="more" aria-label="${te('library.detailsFor', { name: title })}">${icon('more')}</button></span>`;
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
  // a picture on the card (a playlist's cover) comes as a photo; a song's or a video's art from it
  img.src = el.dataset.photo ? withTry(photoUrl(el.dataset.art, size), el._artTries || 0) : artUrl(el.dataset.art, size, el._artTries || 0);
}

libList.addEventListener('click', (e) => {
  if (S.libKind === 'playlists') return onPlaylistClick(e);
  if (S.libKind === 'books') return onBooksClick(e);
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
$('#shuffleBtn').addEventListener('click', () => shufflePaths(S.libRows.map((s) => s.path), t('library.shufflingSongs')));
// done: what the toast says once they play
async function shufflePaths(paths, done) {
  const q = paths.filter((p) => S.libByPath.has(p));
  if (!q.length) return toast(t('library.noSongsToShuffle'), 'err');
  for (let i = q.length - 1; i > 0; i--) { const j = Math.floor(Math.random() * (i + 1)); [q[i], q[j]] = [q[j], q[i]]; }
  try {
    const st = await api('POST', '/api/player/play', { path: q[0], queue: q });
    if (S.status) { S.status.player = st; S.statusAt = performance.now(); }
    renderMini();
    toast(done);
  } catch (e) { if (e.status !== 401) toast(t('library.couldntPlayThem', { reason: reason(e) }), 'err'); }
}
function setKind(kind) {
  S.libKind = kind;
  showKinds();
  S.openPlaylist = null;
  S.openSmart = null;
  if (S.libKind === 'files') return loadFiles(S.filesDir);
  if (S.libKind === 'albums') S.cfAt = S.cfAt || 0;
  if (S.libKind === 'photos') return loadPhotos();
  if (S.libKind === 'books') return loadBooks();
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
    toast(t('common.playingOnRondo', { name: it.title || fileName(it.path) }));
    return true;
  } catch (e) {
    if (e.status !== 401) toast(t('common.couldntPlayIt', { reason: reason(e) }), 'err');
    return false;
  }
}

// ---------- sheet (details and confirmations) ----------
const sheet = $('#sheet');
sheet.addEventListener('click', (e) => { if (e.target === sheet) sheet.close(); });
function openSheet(body) {
  sheet.innerHTML = `<div class="sheet-body"><div class="sheet-grab" aria-hidden="true"></div>${body}</div>`;
  if (!sheet.open) sheet.showModal();
}
function confirmSheet({ title, text, action, danger = true }) {
  return new Promise((resolve) => {
    openSheet(`<h2>${esc(title)}</h2><p>${text}</p><div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('common.cancel')}</button><button class="btn ${danger ? 'btn-danger' : 'btn-primary'}" type="button" data-r="1">${esc(action)}</button></div>`);
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
    ? [[t('library.details.length'), fmtDur(it.durationSec)], [t('library.details.size'), fmtBytes(it.size)], [t('library.details.file'), it.path, 'path']]
    : [[t('library.sort.artist'), it.artist], [t('library.sort.albumArtist'), it.albumArtist && it.albumArtist !== it.artist ? it.albumArtist : ''], [t('library.sort.album'), it.album],
      [t('library.details.disc'), it.disc || ''], [t('library.details.track'), it.track || ''], [t('library.sort.year'), it.year || ''], [t('library.sort.genre'), it.genre || ''], [t('library.details.length'), fmtDur(it.durationSec)],
      [t('library.details.played'), it.plays ? t('library.details.playedTimes', { count: it.plays, date: new Date(it.lastPlayed).toLocaleDateString(dateLocale(), { dateStyle: 'medium' }) }) : t('library.details.never')],
      [t('library.details.size'), fmtBytes(it.size)], [t('library.details.file'), it.path, 'path']];
  const detail = () => {
    openSheet(`<div class="song-sheet${video ? ' is-video' : ''}"><span class="art"${video || it.art ? ` data-art="${esc(it.path)}"` : ''}>${icon(video ? 'film' : 'note')}</span>
      <div><h2>${esc(title)}</h2><p>${esc(video ? t('library.details.videoOnCard') : [it.artist, it.album].filter(Boolean).join(' · ') || t('common.onCard'))}</p></div></div>
      ${video ? '' : `<div class="rate" role="radiogroup" aria-label="${te('library.sort.rating')}">${[1, 2, 3, 4, 5].map((n) => `<button type="button" role="radio" aria-checked="${n === it.rating}" aria-label="${te('library.details.stars', { count: n })}" data-rate="${n}" class="${n <= it.rating ? 'on' : ''}">★</button>`).join('')}</div>`}
      <dl class="facts">${facts.filter((f) => f[1] !== '' && f[1] != null).map(([k, v, c]) => `<dt>${esc(k)}</dt><dd${c ? ` class="${c}"` : ''}>${esc(v)}</dd>`).join('')}</dl>
      <div class="sheet-actions"><button class="btn btn-red-text" type="button" data-act="delete">${icon('trash')}<span>${te('common.delete')}</span></button>${video ? '' : `<button class="btn" type="button" data-act="playlist">${icon('playlist')}<span>${te('library.details.addToPlaylist')}</span></button>`}${video ? '' : `<button class="btn btn-primary" type="button" data-act="play">${icon('play')}<span>${te('common.playOnRondo')}</span></button>`}</div>`);
    const art = $('.art[data-art]', sheet);
    if (art) loadArtInto(art, 240);
    ($('[data-act="play"]', sheet) || $('[data-act="delete"]', sheet)).focus();
  };
  const confirm = (err = '') => {
    openSheet(`<h2>${video ? te('library.deleteVideo') : te('library.deleteSong')}</h2><p>${te('library.deleteText', { title })}</p>
      ${err ? `<div class="note err" style="margin-top:14px">${icon('alert')}<span>${esc(err)}</span></div>` : ''}
      <div class="sheet-actions"><button class="btn" type="button" data-act="back">${te('common.cancel')}</button><button class="btn btn-danger" type="button" data-act="really">${te('common.delete')}</button></div>`);
    $('[data-act="back"]', sheet).focus();
  };
  const onClick = async (e) => {
    const star = e.target.closest('[data-rate]');
    if (star) {  // the same star again takes the rating away
      const n = +star.dataset.rate === it.rating ? 0 : +star.dataset.rate;
      try {
        const res = await api('PUT', '/api/library/rating', { path: it.path, rating: n });
        it.rating = res.rating ?? n;
        $$('[data-rate]', sheet).forEach((x) => { x.classList.toggle('on', +x.dataset.rate <= it.rating); x.setAttribute('aria-checked', String(+x.dataset.rate === it.rating)); });
        if (sortKey() === 'rating') renderLibrary();
      } catch (err) { if (err.status !== 401) toast(t('common.notSaved', { reason: reason(err) }), 'err'); }
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
      b.textContent = t('library.deleting');
      try {
        await api('DELETE', `/api/library?path=${encodeURIComponent(it.path)}`);
        removeFromLibrary(it);
        sheet.close();
        toast(t('common.deletedName', { name: title }));
      } catch (e2) {
        if (e2.status !== 401) confirm(t('common.couldntDeleteIt', { reason: reason(e2) }));
      }
    }
  };
  sheet.addEventListener('click', onClick);
  sheet.addEventListener('close', () => sheet.removeEventListener('click', onClick), { once: true });
  detail();
}
function removeFromLibrary(it) {
  const lib = S.library;
  S.playlists = null; // the Rondo took it out of every playlist too
  lib.songs = lib.songs.filter((s) => s !== it);
  lib.videos = lib.videos.filter((v) => v !== it);
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
  if (!(S.libKind === 'songs' ? lib.songs : lib.videos).length) renderLibrary();
}

// ---------- playlists (.m3u8 files in the card's Playlists folder; local/Playlists.kt) ----------
const plTime = (sec) => (sec >= 3600 ? `${Math.floor(sec / 3600)} h ${Math.round((sec % 3600) / 60)} min` : `${Math.round(sec / 60)} min`);
const plSub = (p) => [t('common.songs', { count: p.paths.length - p.missing }), p.durationSec ? plTime(p.durationSec) : '', p.missing ? t('playlists.missing', { count: p.missing }) : ''].filter(Boolean).join(' · ');
async function loadPlaylists() {
  try {
    const [pl, sm] = await Promise.all([api('GET', '/api/playlists'), api('GET', '/api/smart').catch(() => ({ playlists: [] }))]);
    S.playlists = pl.playlists;
    S.smart = sm.playlists || [];
  } catch (e) {
    if (e.status !== 401) toast(t('playlists.couldntRead', { reason: reason(e) }), 'err');
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
    if (e.status !== 401) toast(t('playlists.couldntSave', { reason: reason(e) }), 'err');
    return null;
  }
}
// a one-line question in the sheet; resolves to the text, or null on Cancel
function promptSheet({ title, text = '', value = '', action, placeholder = '' }) {
  return new Promise((resolve) => {
    openSheet(`<h2>${esc(title)}</h2>${text ? `<p>${text}</p>` : ''}<form class="sheet-form"><input type="text" maxlength="60" value="${esc(value)}" placeholder="${esc(placeholder)}" aria-label="${esc(title)}" autocomplete="off"><div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('common.cancel')}</button><button class="btn btn-primary" type="submit">${esc(action)}</button></div></form>`);
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
  const name = await promptSheet({ title: t('playlists.new'), action: t('playlists.create'), placeholder: t('common.name') });
  if (!name) return null;
  try {
    const p = await api('POST', '/api/playlists', { name, paths });
    keepPlaylist(p);
    toast(paths.length ? t('playlists.addedTo', { name: p.name }) : t('common.made', { name: p.name }));
    return p;
  } catch (e) {
    if (e.status !== 401) toast(t('playlists.couldntMake', { reason: reason(e) }), 'err');
    return null;
  }
}
function renderPlaylists() {
  const all = S.playlists;
  if (!all) { libList.innerHTML = '<div class="lib-skel"></div>'.repeat(3); return; }
  if (S.openSmart) return renderSmart();
  if (S.openPlaylist) return renderPlaylist();
  const q = fold(S.libQuery.trim());
  const shown = q ? all.filter((p) => q.split(/\s+/).every((w) => fold(p.title || p.name).includes(w))) : all;
  $('#libSummary').textContent = all.length ? t('playlists.summary', { count: all.length }) : t('playlists.summaryNone');
  libList.innerHTML = `<div class="song pl-row pl-new" role="listitem" data-act="pl-new"><span class="art art-add">${icon('plus')}</span><span class="s-main"><span class="s-title">${te('playlists.new')}</span></span></div>` +
    (shown.length ? shown.map((p) => `<div class="song pl-row" role="listitem" data-pl="${esc(p.name)}"><span class="art"${p.cover ? ` data-art="${esc(p.cover)}" data-photo="1"` : ''}>${icon('playlist')}</span>
      <span class="s-main"><span class="s-title">${esc(p.title || p.name)}</span><span class="s-sub">${esc((p.youtube ? 'YouTube Music · ' : '') + plSub(p))}</span></span>
      <span class="s-acts">${p.paths.length > p.missing ? `<button class="ib ib-sm" type="button" data-act="pl-play" aria-label="${te('common.playNameOnRondo', { name: p.title || p.name })}">${icon('play')}</button>` : ''}<button class="ib ib-sm" type="button" data-act="pl-open" aria-label="${te('playlists.openName', { name: p.title || p.name })}">${icon('chevron')}</button></span></div>`).join('')
      : all.length ? `<div class="lib-empty"><h3>${te('common.noMatches')}</h3><p>${te('playlists.noMatch', { query: S.libQuery.trim() })}</p></div>` : '') +
    smartListHtml(q);
  $$('.art[data-art]:not([data-q])', libList).forEach((el) => { el.dataset.q = '1'; artIO.observe(el); });  // the covers
}
function renderPlaylist() {
  const p = S.playlists.find((x) => x.name === S.openPlaylist);
  if (!p) { S.openPlaylist = null; return renderPlaylists(); }
  const yt = !!p.youtube;  // a downloaded YouTube Music playlist: it follows YouTube's, not edited here
  $('#libSummary').textContent = yt ? t('playlists.ytSummary') : t('playlists.onCard');
  const rows = p.paths.map((path, i) => {
    const s = S.libByPath.get(path);
    const up = yt ? '' : `<button class="ib ib-sm" type="button" data-act="pl-up" data-i="${i}" aria-label="${te('common.moveUp')}"${i ? '' : ' disabled'}>${icon('up')}</button>`;
    const down = yt ? '' : `<button class="ib ib-sm" type="button" data-act="pl-down" data-i="${i}" aria-label="${te('common.moveDown')}"${i < p.paths.length - 1 ? '' : ' disabled'}>${icon('down')}</button>`;
    const rm = yt ? '' : `<button class="ib ib-sm" type="button" data-act="pl-remove" data-i="${i}" aria-label="${te('playlists.removeFrom')}">${icon('close')}</button>`;
    if (!s) return `<div class="song pl-song is-missing" role="listitem"><span class="art">${icon('note')}</span><span class="s-main"><span class="s-title">${esc(fileName(path))}</span><span class="s-sub">${te('playlists.notOnCard')}</span></span><span class="s-acts">${up}${down}${rm}</span></div>`;
    return `<div class="song pl-song${isPlaying(s) ? ' is-playing' : ''}" role="listitem" data-path="${esc(path)}" data-i="${i}">
      <span class="art"${s.art ? ` data-art="${esc(path)}"` : ''}>${icon('note')}</span>
      <span class="s-main"><span class="s-title">${esc(s.title || fileName(path))}</span><span class="s-sub">${esc([s.artist, s.album].filter(Boolean).join(' · '))}</span></span>
      <span class="s-time">${fmtDur(s.durationSec)}</span><span class="s-acts">${up}${down}${rm}</span></div>`;
  }).join('');
  const playable = p.paths.length > p.missing;
  libList.innerHTML = `<div class="pl-head">
      <button class="btn btn-sm btn-quiet" type="button" data-act="pl-back">${icon('chevron', 'i flip')}<span>${te('common.playlists')}</span></button>
      <h2>${esc(p.title || p.name)}</h2><p>${esc((yt ? 'YouTube Music · ' : '') + plSub(p))}</p>
      <div class="pl-acts">
        <button class="btn btn-primary btn-sm" type="button" data-act="pl-play"${playable ? '' : ' disabled'}>${icon('play')}<span>${te('common.playOnRondo')}</span></button>
        <button class="btn btn-sm" type="button" data-act="pl-shuffle"${playable ? '' : ' disabled'}>${icon('shuffle')}<span>${te('library.shuffle')}</span></button>
        ${yt ? (p.ytId ? `<button class="btn btn-sm" type="button" data-act="pl-update">${icon('refresh')}<span>${te('playlists.updateDownload')}</span></button>` : '')
    : `<button class="btn btn-sm" type="button" data-act="pl-add">${icon('plus')}<span>${te('playlists.addSongs')}</span></button>
        <button class="btn btn-sm" type="button" data-act="pl-rename">${te('common.rename')}</button>`}
        <button class="btn btn-sm btn-red-text" type="button" data-act="pl-delete">${icon('trash')}<span>${te('common.delete')}</span></button>
      </div></div>` + (rows || (yt ? `<div class="lib-empty"><h3>${te('playlists.noSongsYetYt')}</h3><p>${te('playlists.noSongsYetYtText')}</p></div>` : `<div class="lib-empty"><h3>${te('playlists.noSongsYet')}</h3><p>${te('playlists.noSongsYetText')}</p></div>`));
  $$('.art[data-art]:not([data-q])', libList).forEach((el) => { el.dataset.q = '1'; artIO.observe(el); });
}
async function playPlaylist(p, path) {
  const onCard = p.paths.filter((x) => S.libByPath.has(x));
  if (!onCard.length) return;
  try {
    const st = await api('POST', '/api/player/play', { path: path || onCard[0], queue: onCard });
    if (S.status) { S.status.player = st; S.statusAt = performance.now(); }
    renderMini();
    toast(t('common.playingOnRondo', { name: p.title || p.name }));
  } catch (e) { if (e.status !== 401) toast(t('common.couldntPlayIt', { reason: reason(e) }), 'err'); }
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
  if (act === 'pl-shuffle') return shufflePaths(p.paths, t('library.shufflingName', { name: p.title || p.name }));
  if (act === 'pl-song') return playPlaylist(p, row.dataset.path);
  if (act === 'pl-add') return addSongsSheet(p.name);
  if (act === 'pl-rename') {
    const next = await promptSheet({ title: t('playlists.renameTitle'), value: p.name, action: t('common.rename') });
    if (!next || next === p.name) return;
    const saved = await savePlaylist(p.name, { name: next });
    if (saved) { S.openPlaylist = saved.name; renderPlaylists(); }
    return;
  }
  if (act === 'pl-update') {
    try {
      await api('POST', `/api/downloads/playlist?id=${encodeURIComponent(p.ytId)}`, {});
      toast(t('playlists.updating', { name: p.title }));
    } catch (e2) { if (e2.status !== 401) toast(t('common.couldntUpdateIt', { reason: reason(e2) }), 'err'); }
    return;
  }
  if (act === 'pl-delete') {
    const title = p.title || p.name;
    // a downloaded one: its songs can go too
    const how = p.youtube ? await dlListSheet({ title }) : (await confirmSheet({ title: t('common.deleteName', { name: title }), text: te('playlists.deleteText'), action: t('common.delete') })) ? 'keep' : '';
    if (!how) return;
    try {
      await api('DELETE', `/api/playlists?name=${encodeURIComponent(p.name)}${how === 'songs' ? '&songs=1' : ''}`);
      S.playlists = S.playlists.filter((x) => x !== p);
      S.openPlaylist = null;
      if (how === 'songs') loadLibrary(true);
      renderPlaylists();
      toast(how === 'songs' ? t('playlists.deletedWithSongs', { name: title }) : t('common.deletedName', { name: title }));
    } catch (e2) { if (e2.status !== 401) toast(t('common.couldntDeleteIt', { reason: reason(e2) }), 'err'); }
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
        <button class="btn btn-sm${has ? '' : ' btn-primary'}" type="button" data-act="add"${has ? ' disabled' : ''}>${has ? te('common.added') : te('common.add')}</button></div>`;
    }).join('') : `<p class="pick-none">${q.trim() ? te('playlists.noSongsMatch') : te('playlists.noSongsOnCard')}</p>`;
  };
  openSheet(`<h2>${te('playlists.addToName', { name })}</h2><label class="search pick-search">${icon('search')}<input type="search" placeholder="${te('library.searchAll')}" autocomplete="off" aria-label="${te('library.searchSongs')}"></label><div class="pick-list"></div><div class="sheet-actions"><button class="btn" type="button" data-act="done">${te('common.done')}</button></div>`);
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
    b.textContent = t('common.adding');
    if (await savePlaylist(name, { paths: [...p.paths, path] })) draw(); else { b.disabled = false; b.textContent = t('common.add'); }
  };
  sheet.addEventListener('click', onClick);
  sheet.addEventListener('close', () => { sheet.removeEventListener('click', onClick); if (S.libKind === 'playlists') renderPlaylists(); }, { once: true });
  draw();
}
// from a song's details: pick a playlist, or start a new one with it
async function addToPlaylistSheet(it) {
  const all = await loadPlaylists();
  openSheet(`<h2>${te('playlists.addToA')}</h2><p>“${esc(it.title || fileName(it.path))}”</p><div class="pick-list">${all.filter((p) => !p.youtube).map((p) => {
    const has = p.paths.includes(it.path);
    return `<div class="pick" data-pl="${esc(p.name)}"><span class="s-main"><span class="s-title">${esc(p.name)}</span><span class="s-sub">${esc(plSub(p))}</span></span><button class="btn btn-sm${has ? '' : ' btn-primary'}" type="button" data-act="add"${has ? ' disabled' : ''}>${has ? te('common.added') : te('common.add')}</button></div>`;
  }).join('')}</div><div class="sheet-actions"><button class="btn" type="button" data-act="done">${te('common.done')}</button><button class="btn btn-primary" type="button" data-act="new">${icon('plus')}<span>${te('playlists.new')}</span></button></div>`);
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
      b.textContent = t('common.added');
      b.classList.remove('btn-primary');
      $('.s-sub', b.closest('.pick')).textContent = plSub(saved);
      toast(t('playlists.addedTo', { name: p.name }));
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
    if (e.status !== 401) toast(t('library.couldntScan', { reason: reason(e) }), 'err');
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
      const lib = await api('GET', '/api/library', undefined, { timeout: 45000 });
      if (lib.scanning) { scanTimer = setTimeout(check, 2000); return; }
      S.library = lib;
      prepLibrary();
      if (S.route === 'library') renderLibrary(); else renderLibrarySummary();
      if (S.uploads.length) renderUploads();
      if (S.libKind === 'books') loadBooks(true);  // new books from the scan
      const added = lib.songs.length + lib.videos.length - before;
      toast(added > 0 ? t('library.updated', { count: added }) : t('library.upToDate'));
      onDone?.();
    } catch (e) {
      if (e.status !== 401) scanTimer = setTimeout(check, 4000);
    }
  };
  scanTimer = setTimeout(check, 1500);
}

// ---------- audiobooks (Library › Audiobooks on the Rondo: shell/books.h) ----------
// the books with where each is, a book's chapters and bookmarks, its speed; uploads into Audiobooks/
async function loadBooks(quiet = false) {
  if (!quiet && !S.books) libList.innerHTML = '<div class="lib-skel"></div>'.repeat(3);
  try {
    S.books = await api('GET', '/api/books');
    if (S.openBook) S.bookDetail = await api('GET', `/api/books/detail?key=${encodeURIComponent(S.openBook)}`);
  } catch (e) {
    if (e.status === 401) return;
    if (!S.books) libList.innerHTML = `<div class="lib-empty"><h3>${te('books.couldntRead')}</h3><p>${esc(reason(e))}</p></div>`;
    return;
  }
  if (S.libKind === 'books') renderLibrary();
}
const bookSub = (b) => [b.author, b.status].filter(Boolean).join(' · ');
const bookProgress = (b) => (b.started && !b.finished && b.durationMs ? `<span class="bk-prog"><span style="--p:${Math.min(100, (b.positionMs / b.durationMs) * 100).toFixed(1)}%"></span></span>` : '');
function bookRowHtml(b) {
  return `<div class="song bk-row${b.playing ? ' is-playing' : ''}" role="listitem" data-book="${esc(b.key)}">
    <span class="art" data-art="${esc(b.cover)}">${icon('book')}</span>
    <span class="s-main"><span class="s-title">${esc(b.title)}${b.started ? '' : `<span class="bk-dot" title="${te('books.notStarted')}"></span>`}</span><span class="s-sub">${esc(bookSub(b))}</span>${bookProgress(b)}</span>
    <span class="s-acts"><button class="ib ib-sm" type="button" data-act="bk-play" aria-label="${te('common.playNameOnRondo', { name: b.title })}">${icon(b.playing && !b.paused ? 'pause' : 'play')}</button><button class="ib ib-sm" type="button" data-act="bk-open" aria-label="${te('books.openName', { name: b.title })}">${icon('chevron')}</button></span></div>`;
}
function renderBooks() {
  if (!S.books) { loadBooks(); return; }
  if (S.openBook) return renderBook();
  const all = S.books.books, q = fold(S.libQuery.trim());
  const shown = q ? all.filter((b) => q.split(/\s+/).every((w) => fold(`${b.title} ${b.author} ${b.narrator}`).includes(w))) : all;
  $('#libSummary').textContent = S.books.scanning ? t('library.scanning') : all.length ? t('books.summary', { count: all.length }) : t('books.summaryNone');
  const going = shown.filter((b) => b.started && !b.finished).sort((a, b) => b.lastPlayed - a.lastPlayed);
  const upload = `<div class="bk-upload"><button class="btn btn-sm" type="button" data-act="bk-files">${icon('upload')}<span>${te('books.uploadFiles')}</span></button><button class="btn btn-sm" type="button" data-act="bk-folder">${icon('folder')}<span>${te('books.uploadFolder')}</span></button></div>`;
  let html = upload;
  if (!all.length) html += `<div class="lib-empty"><h3>${te('books.none')}</h3><p>${te('books.noneText', { folder: fileName(S.books.folder) })}</p></div>`;
  else if (!shown.length) html += `<div class="lib-empty"><h3>${te('common.noMatches')}</h3><p>${te('books.noMatch', { query: S.libQuery.trim() })}</p></div>`;
  if (going.length) html += `<div class="lhdr" role="heading" aria-level="3">${te('books.continue')}</div>` + going.map(bookRowHtml).join('');
  if (shown.length) html += `<div class="lhdr" role="heading" aria-level="3">${te('books.all')}<small>${te('books.count', { count: shown.length })}</small></div>` + shown.map(bookRowHtml).join('');
  libList.innerHTML = html;
  $$('.art[data-art]:not([data-q])', libList).forEach((el) => { el.dataset.q = '1'; artIO.observe(el); });
}
function renderBook() {
  const b = S.bookDetail;
  if (!b || b.key !== S.openBook) { libList.innerHTML = '<div class="lib-skel"></div>'.repeat(3); return loadBooks(true); }
  $('#libSummary').textContent = t('books.onCard');
  const speeds = (S.books?.speeds || []).map((o) => `<option value="${o.value}"${o.value === b.speed ? ' selected' : ''}>${esc(o.value === 100 ? t('books.speedNormal', { speed: o.text }) : o.text)}</option>`).join('');
  const main = b.playing && !b.paused ? '' : `<button class="btn btn-primary btn-sm" type="button" data-act="bk-resume">${icon('play')}<span>${te(b.finished ? 'books.startAgain' : b.started ? 'books.resume' : 'books.start')}</span></button>`;
  const again = b.started && !b.finished ? `<button class="btn btn-sm" type="button" data-act="bk-start">${icon('refresh')}<span>${te('books.startAgain')}</span></button>` : '';
  const mark = b.playing ? `<button class="btn btn-sm" type="button" data-act="bk-mark">${icon('bookmark')}<span>${te('books.addBookmark')}</span></button>` : '';
  const done = `<button class="btn btn-sm" type="button" data-act="bk-finished">${icon('check')}<span>${te(b.finished ? 'books.markUnfinished' : 'books.markFinished')}</span></button>`;
  const facts = [b.author, b.narrator ? t('books.readBy', { name: b.narrator }) : '', b.chapters ? t('books.lengthChapters', { length: b.lengthText, count: b.chapters }) : b.lengthText].filter(Boolean).join(' · ');
  const chapters = (b.chapterList || []).map((c, i) => `<div class="song bk-ch${i === b.current ? ' is-current' : ''}" role="listitem" data-ch="${i}"><span class="art">${i + 1}</span>
      <span class="s-main"><span class="s-title">${esc(c.title)}</span><span class="s-sub">${fmtTime(c.startMs)}</span></span><span class="s-time">${fmtTime(c.lengthMs)}</span><span class="s-acts"><button class="ib ib-sm" type="button" data-act="bk-ch" aria-label="${te('books.playChapter', { name: c.title })}">${icon('play')}</button></span></div>`).join('');
  const marks = (b.bookmarkList || []).map((m) => `<div class="song bk-bm" role="listitem" data-created="${m.created}"><span class="art">${icon('bookmark')}</span>
      <span class="s-main"><span class="s-title">${esc(m.chapter ? t('books.bookmarkAt', { chapter: m.chapter, time: fmtTime(m.inChapterMs) }) : fmtTime(m.positionMs))}</span><span class="s-sub">${esc(new Date(m.created).toLocaleString(L.code, { dateStyle: 'medium', timeStyle: 'short' }))}</span></span>
      <span class="s-acts"><button class="ib ib-sm" type="button" data-act="bk-bm" aria-label="${te('books.playBookmark')}">${icon('play')}</button><button class="ib ib-sm" type="button" data-act="bk-bm-del" aria-label="${te('books.deleteBookmark')}">${icon('trash')}</button></span></div>`).join('');
  libList.innerHTML = `<div class="pl-head bk-head">
      <button class="btn btn-sm btn-quiet" type="button" data-act="bk-back" style="grid-column:1/-1;justify-self:start">${icon('chevron', 'i flip')}<span>${te('books.title')}</span></button>
      <span class="art bk-cover" data-art="${esc(b.cover)}">${icon('book')}</span>
      <div><h2>${esc(b.title)}</h2><p>${esc(facts)}</p><p>${esc(b.status)}${b.started && !b.finished && b.durationMs ? '' : ''}</p>${bookProgress(b)}</div>
      <div class="pl-acts bk-acts">${main}${again}${mark}<label class="select"><span class="sr-only">${te('books.speed')}</span><select data-act="bk-speed" aria-label="${te('books.speed')}">${speeds}</select>${icon('down')}</label>${done}</div></div>` +
    (chapters ? `<div class="lhdr" role="heading" aria-level="3">${te('books.chapters')}<small>${te('books.chapterCount', { count: b.chapterList.length })}</small></div>${chapters}` : '') +
    `<div class="lhdr" role="heading" aria-level="3">${te('books.bookmarks')}</div>` +
    (marks || `<div class="lib-empty"><p>${te(b.playing ? 'books.noBookmarksPlaying' : 'books.noBookmarks')}</p></div>`);
  $$('.art[data-art]:not([data-q])', libList).forEach((el) => { el.dataset.q = '1'; artIO.observe(el); });
}
async function bookAct(method, path, body, done) {
  try {
    const res = await api(method, path, body);
    if (res?.player !== undefined && S.status) { S.status.player = res.player; S.statusAt = performance.now(); renderMini(); }
    if (res?.book) S.bookDetail = res.book;
    else if (res?.key) S.bookDetail = res;
    if (done) toast(done);
    loadBooks(true);
  } catch (e) {
    if (e.status !== 401) toast(t('books.couldntDo', { reason: reason(e) }), 'err');
  }
}
function onBooksClick(e) {
  const act = e.target.closest('[data-act]')?.dataset.act;
  if (act === 'bk-files') return $('#bookFiles').click();
  if (act === 'bk-folder') return $('#bookFolder').click();
  if (act === 'bk-back') { S.openBook = null; S.bookDetail = null; return renderBooks(); }
  const row = e.target.closest('[data-book]');
  if (row) {
    const b = S.books.books.find((x) => x.key === row.dataset.book);
    if (act === 'bk-play') {
      if (b?.playing) return playerAction('toggle').then(() => loadBooks(true));
      return bookAct('POST', '/api/books/play', { key: row.dataset.book, from: 'resume' }, t('books.playing', { name: b?.title || '' }));
    }
    S.openBook = row.dataset.book;
    S.bookDetail = null;
    window.scrollTo({ top: Math.min(window.scrollY, libEl.offsetTop - 80) });
    return renderBook();
  }
  const b = S.bookDetail;
  if (!b) return;
  if (act === 'bk-resume') return bookAct('POST', '/api/books/play', { key: b.key, from: b.finished ? 'start' : 'resume' }, t('books.playing', { name: b.title }));
  if (act === 'bk-start') return bookAct('POST', '/api/books/play', { key: b.key, from: 'start' }, t('books.playing', { name: b.title }));
  if (act === 'bk-finished') return bookAct('PUT', '/api/books/finished', { key: b.key, finished: !b.finished });
  if (act === 'bk-mark') return bookAct('POST', '/api/books/bookmark', {}, t('books.bookmarkAdded'));
  const ch = e.target.closest('[data-ch]');
  if (ch) return bookAct('POST', '/api/books/play', { key: b.key, from: 'chapter', index: +ch.dataset.ch }, t('books.playing', { name: b.title }));
  const bm = e.target.closest('[data-created]');
  if (bm && act === 'bk-bm-del') return bookAct('DELETE', `/api/books/bookmark?key=${encodeURIComponent(b.key)}&created=${bm.dataset.created}`);
  if (bm) return bookAct('POST', '/api/books/play', { key: b.key, from: 'bookmark', created: +bm.dataset.created }, t('books.playing', { name: b.title }));
}
libList.addEventListener('change', (e) => {
  const sel = e.target.closest('[data-act="bk-speed"]');
  if (sel && S.bookDetail) bookAct('PUT', '/api/books/speed', { key: S.bookDetail.key, speed: +sel.value });
});
// a book's files: right into Audiobooks (an .m4b is a book by itself); a folder: into Audiobooks as it is
$('#bookFiles').addEventListener('change', (e) => { enqueue([...e.target.files], S.books?.folder); e.target.value = ''; });
$('#bookFolder').addEventListener('change', (e) => {
  const by = new Map();
  for (const f of e.target.files) {
    const rel = (f.webkitRelativePath || f.name).split('/').slice(0, -1).join('/');
    if (!by.has(rel)) by.set(rel, []);
    by.get(rel).push(f);
  }
  for (const [rel, files] of by) enqueue(files, rel ? `${S.books?.folder}/${rel}` : S.books?.folder);
  e.target.value = '';
});

// ---------- uploads ----------
let upId = 0;
const fileInput = $('#fileInput');
$('#uploadBtn').addEventListener('click', () => fileInput.click());
// the hint under the list, its "choose files" a button (in the page's language)
$('#dropHint > span').innerHTML = th('library.dropHint', { pick: html(`<button type="button" class="link" id="dropPick">${te('library.dropPick')}</button>`) });
$('#dropPick').addEventListener('click', () => fileInput.click());
fileInput.addEventListener('change', () => { enqueue([...fileInput.files]); fileInput.value = ''; });

// where an upload goes by its kind: Music, Videos (Organize files them into Videos/Shows and
// Videos/Movies, as Jellyfin keeps them), Pictures (else an existing Photos or DCIM folder), each made
// on the card when missing
function folderFor(ext) {
  const folders = S.library?.folders || [];
  const root = (folders[0] || '/storage/sdcard0/Music').split('/').slice(0, 3).join('/');
  const top = (re) => folders.find((f) => f.split('/').length === 4 && re.test(f.split('/')[3]));
  if (VIDEO.includes(ext)) return top(/^videos?$/i) || `${root}/Videos`;
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
  if (bad.length) toast(bad.length === 1 ? t('upload.skippedOne', { name: bad[0] }) : t('upload.skippedMany', { count: String(bad.length) }), 'err');
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
  if (status === 0) return t('upload.connectionLost');
  if (status === 415) return t('upload.cantPlay');
  if (status === 507) return t('upload.noSpace');
  if (status === 403) return t('upload.notAllowed');
  return code ? t('upload.failedCode', { code }) : t('upload.failedStatus', { status: String(status) });
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
  if (u.status === 'queued') return { text: t('upload.waiting', { size: fmtBytes(u.size) }), cls: '' };
  if (u.status === 'uploading') return { text: t('upload.progress', { percent: Math.floor((u.loaded / u.size) * 100), loaded: fmtBytes(u.loaded), size: fmtBytes(u.size) }), cls: '' };
  if (u.status === 'done') return { text: t('upload.copiedTo', { folder: fileName(u.folder), size: fmtBytes(u.size) }), cls: 'ok' };
  if (u.status === 'canceled') return { text: t('upload.canceled'), cls: '' };
  return { text: u.error || t('upload.failed'), cls: 'err' };
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
    title = t('upload.uploadingNth', { n: String(nth), total: String(list.filter((u) => u.status !== 'canceled').length) });
    const elapsed = cur ? (performance.now() - cur.started) / 1000 : 0;
    const speed = cur && elapsed > 1.5 ? cur.loaded / elapsed : 0;
    const left = active.reduce((n, u) => n + u.size - u.loaded, 0);
    const eta = speed ? left / speed : 0;
    const sizes = { sent: fmtBytes(sent), total: fmtBytes(totalBytes) };
    sub = eta > 5 ? t('upload.sentEta', { ...sizes, time: eta > 90 ? `${Math.round(eta / 60)} min` : `${Math.round(eta)} s` }) : t('upload.sent', sizes);
  } else {
    title = failed ? t('upload.copiedFailed', { count: done, failed: String(failed) }) : t('upload.copied', { count: done });
    sub = S.library?.scanning ? t('upload.adding') : done ? t('upload.inLibrary') : t('upload.nothing');
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
      ${h.active ? `<button class="btn btn-sm btn-quiet" type="button" data-up="cancel-all">${te('upload.cancelAll')}</button>` : `<button class="btn btn-sm" type="button" data-up="clear">${te('common.done')}</button>`}
    </div>
    ${free != null && pendingBytes > free ? `<div class="note err" style="margin:0 16px 14px">${icon('alert')}<span>${te('upload.tooBig', { need: fmtBytes(pendingBytes), free: fmtBytes(free) })}</span></div>` : ''}
    <div class="up-list">${S.uploads.map((u) => {
      const m = uploadMeta(u);
      const going = u.status === 'queued' || u.status === 'uploading';
      const btn = u.status === 'error'
        ? `<button class="ib ib-sm" type="button" data-up="retry" data-id="${u.id}" aria-label="${te('upload.retryName', { name: u.name })}">${icon('refresh')}</button>`
        : `<button class="ib ib-sm" type="button" data-up="${going ? 'cancel' : 'remove'}" data-id="${u.id}" aria-label="${going ? te('common.cancelName', { name: u.name }) : te('upload.removeName', { name: u.name })}">${icon('close')}</button>`;
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
  $$('[data-id="library"] .nav-badge').forEach((b) => (b.innerHTML = n ? `<span class="dot-badge" aria-label="${te('upload.badge', { count: n })}">${n}</span>` : ''));
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
  music: { tile: 'music', glyph: 'note', brand: 'music', t: ['#ff5a72', '#c8102e'], title: 'YouTube Music', logo: MARK.music('pv-glyph pv-mark') },
  videos: { tile: 'video', glyph: 'play', brand: 'video', t: ['#ff6a5c', '#d7001a'], title: 'YouTube', logo: MARK.video('pv-glyph pv-mark wide') },
  whatsapp: { tile: 'whatsapp', glyph: 'chat', brand: 'whatsapp', t: ['#63f08e', '#17a74c'], title: 'WhatsApp', logo: MARK.whatsapp('pv-glyph pv-mark wa') },
  device: { tile: 'device', glyph: 'library', t: ['#5fb7ff', '#1f73e0'], title: t('common.library') },
  nowPlaying: { tile: 'now', glyph: 'bars', t: ['#7be07b', '#26a83b'], title: t('overview.nothingPlaying') },
  radio: { tile: 'radio', glyph: 'radio', t: ['#ffb04d', '#f07a12'], title: t('nav.fmRadio') },
  settings: { tile: 'settings', glyph: 'gear', t: ['#b8bcc4', '#6f747d'], title: t('common.settings') },
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
    $('#applied').innerHTML = `${icon('alert')}<span>${te('theme.couldntLoad', { reason: reason(e) })}</span> <button class="link" type="button" data-act="reload-theme">${te('common.tryAgain')}</button>`;
  }
}
$('#applied').addEventListener('click', (e) => {
  if (e.target.closest('[data-act="reload-theme"]')) loadTheme();
  if (e.target.closest('[data-act="resend-theme"]')) {
    const cur = S.theme;
    themePending = { accent: cur.accent, appearance: cur.appearance, textScale: cur.textScale, menu: cur.menu.map(({ id, visible }) => ({ id, visible })) };
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
  const thm = S.theme, el = $('#preview');
  if (!thm || !el) return;
  const items = thm.menu.filter((m) => m.visible);
  if (!items.some((m) => m.id === S.pvSel)) S.pvSel = items[0]?.id;
  const st = S.status || {}, p = st.player;
  const level = st.battery?.level ?? 80;
  const rows = items.map((m) => {
    const d = MENU[m.id] || MENU.settings;
    return `<div class="pv-row${m.id === S.pvSel ? ' sel' : ''}" data-id="${m.id}">${d.brand ? brandTile(d.brand, 'pv-tile') : `<span class="pv-tile" style="--t1:${d.t[0]};--t2:${d.t[1]}"><svg aria-hidden="true"><use href="#i-${d.glyph}"/></svg></span>`}<span class="pv-label">${esc(m.label)}</span><svg class="pv-chev" aria-hidden="true"><use href="#i-chevron"/></svg></div>`;
  }).join('');
  el.setAttribute('aria-label', t('theme.previewLong', { items: items.map((m) => m.label).join(', '), appearance: thm.appearance === 'dark' ? t('theme.dark') : t('theme.light'), size: textScaleName(thm.textScale) }));
  el.innerHTML = `<div class="pv${thm.appearance === 'dark' ? ' dark' : ''}" style="--s:${thm.textScale}">
    <div class="pv-bar">${p?.playing ? '<svg class="pv-play" aria-hidden="true"><use href="#i-play"/></svg>' : ''}<b>Rondo</b>
      <span class="pv-right"><svg class="pv-wifi" style="opacity:${st.wifi ? 1 : 0.25}" aria-hidden="true"><use href="#i-wifi"/></svg><span class="pv-pct">${level}%</span>${battSvg(level, 'pv-batt')}</span></div>
    <div class="pv-body">${items.length ? `<div class="pv-list">${rows}</div><div class="pv-div"></div>${paneHtml(S.pvSel)}` : `<div class="pv-empty">${te('theme.nothingInMenu')}</div>`}</div></div>`;
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
    ? `<div class="pv-visual boxed">${d.logo}</div>`
    : `<div class="pv-visual boxed"><svg class="pv-glyph" aria-hidden="true"><use href="#i-${d.glyph}"/></svg></div>`;
  // like the device: the title only, no hint line under it
  return `<div class="pv-pane" style="--p1:${d.t[0]};--p2:${d.t[1]};--p3:${darker(d.t[1], 0.7)}">${visual}<div class="pv-pt">${esc(d.title)}</div></div>`;
}
$('#preview').addEventListener('click', (e) => {
  const r = e.target.closest('.pv-row');
  if (r) { S.pvSel = r.dataset.id; renderPreview(); }
});
const textScaleName = (s) => (s < 1.3 ? t('theme.sizeSmall') : s > 1.5 ? t('theme.sizeLarge') : t('theme.sizeMedium'));
function renderSwatches() {
  const thm = S.theme, cur = (thm.accent || '').toLowerCase();
  const presets = thm.presets?.length ? thm.presets : [{ name: t('theme.ipodBlue'), accent: '#1c5ccc' }];
  const isPreset = presets.some((p) => p.accent.toLowerCase() === cur);
  $('#swatches').innerHTML = presets.map((p) => `<button class="sw" type="button" role="radio" aria-checked="${p.accent.toLowerCase() === cur}" data-accent="${esc(p.accent)}" style="--c:${esc(p.accent)};--top:${accentTop(p.accent)}"><span class="sw-dot">${icon('check')}</span><span>${esc(p.name)}</span></button>`).join('')
    + `<div class="sw sw-custom" role="radio" aria-checked="${!isPreset}" style="--c:${esc(cur)};--top:${accentTop(cur)}"><span class="sw-dot">${icon('check')}</span><span>${isPreset ? te('theme.custom') : esc(cur)}</span><input type="color" value="${esc(cur)}" aria-label="${te('theme.customColor')}"></div>`;
}
function syncSwatches() { // while dragging the color picker: no re-render, just the marks
  const cur = S.theme.accent.toLowerCase();
  let preset = false;
  $$('#swatches .sw[data-accent]').forEach((b) => { const on = b.dataset.accent.toLowerCase() === cur; preset ||= on; b.setAttribute('aria-checked', String(on)); });
  const c = $('#swatches .sw-custom');
  c.setAttribute('aria-checked', String(!preset));
  c.style.setProperty('--c', cur);
  c.style.setProperty('--top', accentTop(cur));
  $('span:not(.sw-dot)', c).textContent = preset ? t('theme.custom') : cur;
}
$('#swatches').addEventListener('click', (e) => {
  const b = e.target.closest('.sw[data-accent]');
  if (b) changeTheme({ accent: b.dataset.accent.toLowerCase() });
});
$('#swatches').addEventListener('input', (e) => { if (e.target.type === 'color') changeTheme({ accent: e.target.value.toLowerCase() }, 350); });
$('#swatches').addEventListener('change', (e) => { if (e.target.type === 'color') changeTheme({ accent: e.target.value.toLowerCase() }, 0); });
function renderThemeSegs() {
  const thm = S.theme;
  $$('#appearance button').forEach((b) => b.setAttribute('aria-checked', String(b.dataset.v === thm.appearance)));
  $$('#textScale button').forEach((b) => b.setAttribute('aria-checked', String(Math.abs(+b.dataset.v - thm.textScale) < 0.01)));
}
$('#appearance').addEventListener('click', (e) => { const b = e.target.closest('[data-v]'); if (b) changeTheme({ appearance: b.dataset.v }); });
$('#textScale').addEventListener('click', (e) => { const b = e.target.closest('[data-v]'); if (b) changeTheme({ textScale: +b.dataset.v }); });
function renderMenuItems(focus) {
  const menu = S.theme.menu;
  $('#menuItems').innerHTML = menu.map((m, i) => {
    const d = MENU[m.id] || MENU.settings;
    return `<div class="row menu-row${m.visible ? '' : ' is-off'}" data-id="${m.id}">
      ${d.brand ? brandTile(d.brand) : tileHtml(d.tile, d.glyph)}
      <div class="row-main"><div class="row-title">${esc(m.label)}</div>${m.visible ? '' : `<div class="row-sub">${te('theme.hidden')}</div>`}</div>
      <div class="row-ctl">
        <button class="ib ib-sm" type="button" data-move="-1" aria-label="${te('theme.moveUp', { name: m.label })}"${i === 0 ? ' disabled' : ''}>${icon('up')}</button>
        <button class="ib ib-sm" type="button" data-move="1" aria-label="${te('theme.moveDown', { name: m.label })}"${i === menu.length - 1 ? ' disabled' : ''}>${icon('down')}</button>
        ${m.locked ? `<span class="locked" title="${te('theme.locked')}">${icon('lock')}<span class="sr-only">${te('theme.alwaysShown')}</span></span>` : `<button class="switch" type="button" role="switch" aria-checked="${m.visible}" aria-label="${te('theme.show', { name: m.label })}"></button>`}
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
    const res = await api('PUT', '/api/theme', body);
    if (!Object.keys(themePending).length) {
      const same = res.accent?.toLowerCase() === S.theme.accent?.toLowerCase();
      S.theme = res;
      applyAccent(res.accent);
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
  if (state === 'busy') { el.className = 'applied'; el.innerHTML = `<span>${te('theme.applying')}</span>`; return; }
  if (state === 'ok') {
    el.className = 'applied ok';
    el.innerHTML = `${icon('check')}<span>${te('theme.applied')}</span>`;
    appliedTimer = setTimeout(() => el.classList.add('fade'), 2600);
    return;
  }
  el.className = 'applied err';
  el.innerHTML = `${icon('alert')}<span>${te('theme.notApplied', { reason: reason(e) })}</span> <button class="link" type="button" data-act="resend-theme">${te('common.tryAgain')}</button>`;
}

// ---------- settings ----------
const QUALITY = [[null, t('youtube.qualityAuto')], [720, '720p'], [480, '480p'], [360, '360p']];
const TIMEOUTS = [[15, '15 s'], [30, '30 s'], [60, '1 min'], [120, '2 min'], [300, '5 min'], [600, '10 min']];
const WHEEL = [['off', t('common.off')], ['normal', t('settings.wheel.normal')], ['fast', t('settings.wheel.fast')]];
const BUZZ = [['off', t('common.off')], ['light', t('settings.buzz.light')], ['normal', t('settings.buzz.normal')], ['strong', t('settings.buzz.strong')]];
async function loadSettings() {
  if (!S.settings) $('#settingsRows').innerHTML = '<div class="lib-skel"></div>'.repeat(5);
  try {
    S.settings = await api('GET', '/api/settings');
    renderSettings();
  } catch (e) {
    if (e.status !== 401 && !S.settings) $('#settingsRows').innerHTML = `<div class="row-empty">${te('settings.couldntLoad', { reason: reason(e) })} <button class="link" type="button" data-act="reload">${te('common.tryAgain')}</button></div>`;
  }
}
function segHtml(key, options, cur, label) {
  // the value as JSON, escaped: a string's quotes ("off") had ended the attribute, so the Wheel's
  // choices read as nothing
  return `<div class="seg seg-wide" role="radiogroup" aria-label="${esc(label)}" data-key="${key}">${options.map(([v, l]) => `<button type="button" role="radio" aria-checked="${v === cur}" data-v="${esc(JSON.stringify(v))}">${esc(l)}</button>`).join('')}</div>`;
}
function renderSettings() {
  const s = S.settings;
  if (S.server) renderQuality();
  // the Rondo's language, and so this page's, each language in its own name (as Settings › Language on the Y1)
  const langs = L.languages.length ? L.languages : [{ code: L.code, name: L.name }];
  const curLang = s.language || L.code;
  $('#langRows').innerHTML = `
    <div class="row set-row"><span class="tile t-language">${icon('globe')}</span>
      <div class="row-main"><div class="row-title">${te('settings.language')}</div><div class="row-sub">${te('settings.languageSub')}</div></div>
      <div class="row-control"><div class="select"><select id="langSel" aria-label="${te('settings.language')}">${langs.map((l) => `<option value="${esc(l.code)}"${l.code === curLang ? ' selected' : ''}>${esc(l.name)}</option>`).join('')}</select>${icon('down')}</div></div></div>`;
  $('#settingsRows').innerHTML = `
    <div class="row row-wide set-row"><span class="tile t-brightness">${icon('sun')}</span>
      <div class="row-main"><div class="row-title">${te('settings.brightness')}</div><div class="row-sub">${te('settings.brightnessSub')}</div></div>
      <div class="row-control"><div class="range-row">${icon('sun')}<input type="range" id="brightness" min="10" max="100" step="5" value="${s.brightness}" aria-label="${te('settings.brightness')}"><output id="brightnessOut">${s.brightness}%</output></div></div></div>
    <div class="row row-wide set-row"><span class="tile t-timer">${icon('timer')}</span>
      <div class="row-main"><div class="row-title">${te('settings.screenOff')}</div><div class="row-sub">${te('settings.screenOffSub')}</div></div>
      <div class="row-control">${segHtml('screenTimeoutSec', TIMEOUTS, s.screenTimeoutSec, t('settings.screenOff'))}</div></div>
    <div class="row set-row"><span class="tile t-lyrics">${icon('mic')}</span>
      <div class="row-main"><div class="row-title">${te('settings.lyrics')}</div><div class="row-sub">${te('settings.lyricsSub')}</div></div>
      <button class="switch" type="button" role="switch" data-key="lyricsOnOpen" aria-checked="${!!s.lyricsOnOpen}" aria-label="${te('settings.lyrics')}"></button></div>`;
  $('#controlRows').innerHTML = `
    <div class="row row-wide set-row"><span class="tile t-accel">${icon('gauge')}</span>
      <div class="row-main"><div class="row-title">${te('settings.wheelAccel')}</div><div class="row-sub">${te('settings.wheelAccelSub')}</div></div>
      <div class="row-control">${segHtml('wheelAcceleration', WHEEL, s.wheelAcceleration, t('settings.wheelAccel'))}</div></div>
    <div class="row row-wide set-row"><span class="tile t-vibrate">${icon('vibrate')}</span>
      <div class="row-main"><div class="row-title">${te('settings.wheelVibration')}</div><div class="row-sub">${te('settings.wheelVibrationSub')}</div></div>
      <div class="row-control">${segHtml('wheelVibration', BUZZ, s.wheelVibration ?? 'normal', t('settings.wheelVibration'))}</div></div>
    <div class="row set-row"><span class="tile t-click">${icon('volume')}</span>
      <div class="row-main"><div class="row-title">${te('settings.clickSound')}</div><div class="row-sub">${te('settings.clickSoundSub')}</div></div>
      <button class="switch" type="button" role="switch" data-key="clickSound" aria-checked="${s.clickSound !== false}" aria-label="${te('settings.clickSound')}"></button></div>`;
  setFill($('#brightness'));
  renderSubtitles();
}
// Subtitles: the same choices as Settings › Display › Subtitles on the Y1, by index
const SUB_OPACITY = [[25, '25%'], [50, '50%'], [75, '75%'], [100, '100%']];
const SUB_BACK_OPACITY = [[0, t('settings.subtitles.none')], [25, '25%'], [50, '50%'], [75, '75%'], [100, '100%']];
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
    <div class="sub-preview" aria-hidden="true"><span class="sub-line" id="subLine" style="${subLineStyle(s)}">${te('settings.subtitles.sample')}</span></div>
    <div class="row set-row"><span class="tile t-subtitles">${icon('captions')}</span>
      <div class="row-main"><div class="row-title">${te('settings.subtitles.show')}</div><div class="row-sub">${te('settings.subtitles.showSub')}</div></div>
      <button class="switch" type="button" role="switch" data-key="subtitles" aria-checked="${!!s.subtitles}" aria-label="${te('settings.subtitles.show')}"></button></div>
    <div class="row set-row"><div class="row-main"><div class="row-title">${te('settings.subtitles.language')}</div><div class="row-sub">${te('settings.subtitles.languageSub')}</div></div>
      <div class="row-control">${sel('subLang', ch.languages.map((l) => [l.code, l.name]), s.subLang || '', t('settings.subtitles.language'))}</div></div>
    <div class="row row-wide set-row"><div class="row-main"><div class="row-title">${te('settings.subtitles.size')}</div></div>
      <div class="row-control">${segHtml('subSize', ch.sizes.map((n, i) => [i, n]), s.subSize, t('settings.subtitles.size'))}</div></div>
    <div class="row set-row"><div class="row-main"><div class="row-title">${te('settings.subtitles.textColor')}</div></div>
      <div class="row-control">${sel('subColor', colors, s.subColor, t('settings.subtitles.textColor'))}</div></div>
    <div class="row row-wide set-row"><div class="row-main"><div class="row-title">${te('settings.subtitles.textOpacity')}</div></div>
      <div class="row-control">${segHtml('subOpacity', SUB_OPACITY, s.subOpacity, t('settings.subtitles.textOpacity'))}</div></div>
    <div class="row set-row"><div class="row-main"><div class="row-title">${te('settings.subtitles.background')}</div></div>
      <div class="row-control">${sel('subBack', colors, s.subBack, t('settings.subtitles.background'))}</div></div>
    <div class="row row-wide set-row"><div class="row-main"><div class="row-title">${te('settings.subtitles.backgroundOpacity')}</div></div>
      <div class="row-control">${segHtml('subBackOpacity', SUB_BACK_OPACITY, s.subBackOpacity, t('settings.subtitles.backgroundOpacity'))}</div></div>
    <div class="row set-row"><div class="row-main"><div class="row-title">${te('settings.subtitles.edges')}</div></div>
      <div class="row-control">${sel('subEdge', ch.edges.map((n, i) => [i, n]), s.subEdge, t('settings.subtitles.edges'))}</div></div>
    <div class="sub-actions"><button class="btn btn-sm" type="button" data-act="sub-reset">${te('settings.subtitles.youtubeLook')}</button></div>`;
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
  if (e.target.id === 'langSel') return setLanguage(e.target);
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
    if (!quiet) toast(t('common.savedOnRondo'));
  } catch (e) {
    if (e.status === 401) return;
    toast(t('common.notSaved', { reason: reason(e) }), 'err');
    if (S.settings) renderSettings();
  }
}
// the Rondo's language, and so this page's: saved there, then the page loads again in it
async function setLanguage(sel) {
  sel.disabled = true;
  try {
    await api('PUT', '/api/settings', { language: sel.value });
    location.reload();
  } catch (e) {
    sel.disabled = false;
    sel.value = S.settings?.language || L.code;
    if (e.status !== 401) toast(t('common.notSaved', { reason: reason(e) }), 'err');
  }
}

// ---------- FM radio (the Rondo's radio: player/FmRadio.kt) ----------
// Refreshes every 1.5 s while the page is open (station names and signal change on their own).
// The page is rebuilt only when its shape changes (power, station, lists, settings); signal, names
// and mute are updated in place, so a click is never lost to a rebuild under the pointer.
let radioTimer = 0;
const radioShape = (r) => JSON.stringify([r.headphones, r.error, r.on, r.starting, r.seeking, r.scanning, r.frequency, r.band, r.speaker, r.presets, r.found]);
function radioState(r) {
  const busy = r.starting ? t('common.turningOn') : r.scanning ? t('radio.scanning') : r.seeking ? t('common.searching') : '';
  return !r.on ? (busy || t('radio.off')) : [busy || (r.muted ? t('radio.muted') : r.stereo ? t('radio.stereo') : t('radio.mono')), r.speaker ? t('radio.speaker') : t('radio.headphones')].join(' · ');
}
function updateHero(r) {
  const set = (id, text) => { const el = document.getElementById(id); if (el && el.textContent !== text) el.textContent = text; };
  set('fmStateText', radioState(r));
  set('fmName', r.name || '');
  set('fmText', r.text || '');
  set('fmMute', r.muted ? t('radio.unmute') : t('radio.mute'));
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
    if (e.status !== 401 && !S.radio) body.innerHTML = `<div class="group"><div class="row-empty">${te('radio.couldntLoad', { reason: reason(e) })}</div></div>`;
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
        <button class="switch" type="button" role="switch" data-fm="power" aria-checked="${r.on || r.starting}" aria-label="${te('common.radio')}"${r.starting ? ' disabled' : ''}></button></div>
      <div class="fm-freq" id="fmFreq">${fmName(r.frequency)}<small>MHz</small></div>
      <div class="fm-name" id="fmName">${esc(r.name || '')}</div>
      <div class="fm-text" id="fmText">${esc(r.text || '')}</div>
      <input class="fm-dial" type="range" id="fmDial" min="${r.min}" max="${r.max}" step="1" value="${r.frequency}" aria-label="${te('radio.frequency')}"${r.on ? '' : ' disabled'}>
      <div class="fm-scale">${labels.join('')}</div>
      <div class="fm-controls">
        <button class="ib" type="button" data-fm="seek" data-up="0" aria-label="${te('radio.prevStation')}"${r.on ? '' : ' disabled'}>${icon('prev')}</button>
        ${step(-1)}
        <form class="fm-tune" id="fmTune"><input type="text" inputmode="decimal" id="fmTuneIn" value="${fmName(r.frequency)}" aria-label="${te('radio.frequencyMhz')}"${r.on ? '' : ' disabled'}><button class="btn btn-sm" type="submit"${r.on ? '' : ' disabled'}>${te('radio.tune')}</button></form>
        ${step(1)}
        <button class="ib" type="button" data-fm="seek" data-up="1" aria-label="${te('radio.nextStation')}"${r.on ? '' : ' disabled'}>${icon('next')}</button>
      </div>
      <div class="fm-volume">${icon('volume')}<input type="range" id="fmVol" min="0" max="${r.maxVolume}" step="1" value="${r.volume}" aria-label="${te('radio.volume')}"><output id="fmVolOut">${r.volume}</output></div>
      <div class="fm-controls">
        <button class="btn btn-sm" type="button" id="fmMute" data-fm="mute"${r.on ? '' : ' disabled'}>${r.muted ? te('radio.unmute') : te('radio.mute')}</button>
        <button class="btn btn-sm" type="button" data-fm="save"${saved ? ' disabled' : ''}>${saved ? te('radio.saved') : te('radio.saveStation')}</button>
      </div>
    </div>`;
}
function renderRadio() {
  const r = S.radio, body = $('#radioBody');
  const editing = body.contains(document.activeElement) && document.activeElement.tagName === 'INPUT' && document.activeElement.type !== 'range';
  const notes = [];
  if (!r.headphones) notes.push(`<div class="note">${icon('alert')}<span>${te('radio.plugHeadphones')}</span></div>`);
  else if (r.error === 'chip') notes.push(`<div class="note err">${icon('alert')}<span>${te('radio.chip')}</span></div>`);
  if (editing && $('.fm-hero', body)) {
    // keep the fields being typed in: only the top part follows the radio
    const hero = $('.fm-hero', body);
    hero.outerHTML = fmHero(r);
    setFill($('#fmDial'));
    return;
  }
  const presets = (r.presets || []).map((p, i, all) => `<div class="row fm-preset" data-f="${p.frequency}">
      <div class="row-main"><button class="btn btn-sm btn-quiet fm-row-freq" type="button" data-fm="play" data-f="${p.frequency}">${fmName(p.frequency)}</button>
        <input type="text" class="fm-name-input" data-fm="name" data-f="${p.frequency}" value="${esc(p.name)}" placeholder="${te('common.name')}" maxlength="40" aria-label="${te('radio.nameFor', { frequency: fmName(p.frequency) })}">
        ${p.frequency === r.frequency && r.on ? `<span class="fm-playing">${te('common.playing')}</span>` : ''}</div>
      <div class="bt-actions">
        <button class="ib ib-sm" type="button" data-fm="move" data-i="${i}" data-d="-1" aria-label="${te('common.moveUp')}"${i === 0 ? ' disabled' : ''}>${icon('up')}</button>
        <button class="ib ib-sm" type="button" data-fm="move" data-i="${i}" data-d="1" aria-label="${te('common.moveDown')}"${i === all.length - 1 ? ' disabled' : ''}>${icon('down')}</button>
        <button class="ib ib-sm danger" type="button" data-fm="remove" data-i="${i}" aria-label="${te('radio.remove', { frequency: fmName(p.frequency) })}">${icon('trash')}</button>
      </div></div>`).join('') || `<div class="row-empty">${te('radio.noSaved')}</div>`;
  const found = (r.found || []).map((s) => `<div class="row">
      <div class="row-main"><div class="row-title">${fmName(s.frequency)} MHz${s.frequency === r.frequency && r.on ? ` <span class="fm-playing">${te('common.playing')}</span>` : ''}</div>${s.name ? `<div class="row-sub">${esc(s.name)}</div>` : ''}</div>
      <div class="bt-actions"><button class="btn btn-sm" type="button" data-fm="play" data-f="${s.frequency}"${r.on ? '' : ' disabled'}>${te('common.play')}</button>
        <button class="btn btn-sm btn-quiet" type="button" data-fm="add" data-f="${s.frequency}">${te('common.save')}</button></div></div>`).join('')
    || `<div class="row-empty">${r.scanning ? te('radio.scanningLong') : te('radio.scanHint')}</div>`;
  const segs = (key, options, cur) => `<div class="seg" role="radiogroup" data-fm="${key}">${options.map(([v, l]) => `<button type="button" role="radio" aria-checked="${v === cur}" data-v="${v}">${esc(l)}</button>`).join('')}</div>`;
  body.innerHTML = `${notes.join('')}${fmHero(r)}
    <h2 class="gh">${te('radio.savedStations')}</h2><div class="group">${presets}</div>
    <h2 class="gh" style="display:flex;align-items:center;justify-content:space-between">${te('radio.found')} <button class="btn btn-sm" type="button" data-fm="scan"${r.on && !r.scanning ? '' : ' disabled'}>${r.scanning ? te('radio.scanning') : te('radio.scan')}</button></h2>
    <div class="group">${found}</div>
    <h2 class="gh">${te('radio.outputRegion')}</h2>
    <div class="group">
      <div class="row row-wide set-row"><div class="row-main"><div class="row-title">${te('sound.output')}</div><div class="row-sub">${r.speaker ? te('radio.outputSubBoth') : te('radio.outputSub')}</div></div>
        <div class="row-control">${segs('speaker', [['0', t('radio.headphones')], ['1', t('radio.speaker')]], r.speaker ? '1' : '0')}</div></div>
      <div class="row row-wide set-row"><div class="row-main"><div class="row-title">${te('radio.region')}</div><div class="row-sub">${te('radio.regionSub')}</div></div>
        <div class="row-control">${segs('band', [['world', t('radio.world')], ['japan', t('radio.japan')]], r.band)}</div></div>
    </div>`;
  setFill($('#fmDial'));
  setFill($('#fmVol'));
}
async function radioPut(path, body, method = 'PUT') {
  try {
    S.radio = await api(method, `/api/radio${path}`, body);
    renderRadio();
  } catch (e) {
    if (e.status !== 401) toast(t('common.didntWork', { reason: reason(e) }), 'err');
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
  if (!(f >= r.min && f <= r.max)) return toast(t('radio.pickFrequency', { min: fmName(r.min), max: fmName(r.max) }), 'err');
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
    if (e.status !== 401 && !S.bt) body.innerHTML = `<div class="group"><div class="row-empty">${te('bluetooth.couldntLoad', { reason: reason(e) })}</div></div>`;
  }
  if (S.route === 'bluetooth' && !document.hidden) btTimer = setTimeout(() => loadBluetooth(true), 2000);
}
function btDeviceRow(d) {
  const connected = d.status === 'connected', connecting = d.status === 'connecting';
  const sub = connected ? t('bluetooth.musicHere') : connecting ? t('common.connecting') : d.audio ? t('bluetooth.kindNotConnected', { kind: d.kind }) : d.kind;
  const main = connecting ? `<span class="joining">${spinner()}${te('common.connecting')}</span>`
    : connected ? `<button class="btn btn-sm" type="button" data-bt="disconnect" data-addr="${esc(d.address)}">${te('bluetooth.disconnect')}</button>`
    : `<button class="btn btn-sm btn-primary" type="button" data-bt="connect" data-addr="${esc(d.address)}">${te('common.connect')}</button>`;
  return `<div class="row"><div class="row-main"><div class="row-title">${esc(d.name)}${connected ? `<span class="tag ok">${te('common.connected')}</span>` : ''}</div><div class="row-sub">${esc(sub)}</div></div>
    <div class="bt-actions">${main}<button class="btn btn-sm btn-red-text" type="button" data-bt="forget" data-addr="${esc(d.address)}" data-name="${esc(d.name)}">${te('common.forget')}</button></div></div>`;
}
function btOtherRow(d) {
  const action = d.pairing ? `<span class="joining">${spinner()}${te('bluetooth.pairing')}</span>`
    : `<button class="btn btn-sm" type="button" data-bt="connect" data-addr="${esc(d.address)}">${te('bluetooth.pair')}</button>`;
  return `<div class="row"><div class="row-main"><div class="row-title">${esc(d.name)}</div><div class="row-sub">${esc(d.kind)}</div></div><div class="bt-actions">${action}</div></div>`;
}
function renderBluetooth() {
  const b = S.bt, body = $('#btBody');
  $('#btSearch').hidden = !(b.available && b.state === 'on');
  if (!b.available) {
    body.innerHTML = `<div class="note">${icon('alert')}<span>${te('bluetooth.none')}</span></div>`;
    return;
  }
  const on = b.state === 'on', turning = b.state.startsWith('turning');
  const stateText = { on: te('common.on'), off: te('common.off'), turning_on: te('common.turningOn'), turning_off: te('common.turningOff') }[b.state] || '';
  const power = `<div class="group"><div class="row wifi-current"><span class="tile t-bluetooth">${icon('bluetooth')}</span>
      <div class="row-main"><div class="row-title">Bluetooth</div><div class="row-sub">${stateText}</div></div>
      <button class="switch" type="button" role="switch" data-bt="power" aria-checked="${on || b.state === 'turning_on'}" aria-label="Bluetooth"${turning ? ' disabled' : ''}></button></div></div>`;
  if (!on) {
    body.innerHTML = power;
    return;
  }
  const mine = (b.mine || []).map(btDeviceRow).join('') || `<div class="row-empty">${te('bluetooth.noPaired')}</div>`;
  const others = (b.others || []).map(btOtherRow).join('')
    || `<div class="row-empty">${b.searching ? te('bluetooth.looking') : te('bluetooth.noneFound')}</div>`;
  body.innerHTML = `${power}
    <h2 class="gh">${te('bluetooth.mine')}</h2><div class="group">${mine}</div>
    <h2 class="gh">${te('bluetooth.others')}${b.searching ? ` <span class="joining">${spinner()}${te('common.searching')}</span>` : ''}</h2><div class="group">${others}</div>
    <div class="note info">${icon('info')}<span>${te('bluetooth.pinNote')}</span></div>`;
}
$('#btSearch').addEventListener('click', async () => {
  try { S.bt = await api('POST', '/api/bluetooth/search'); renderBluetooth(); } catch (e) { if (e.status !== 401) toast(t('bluetooth.couldntSearch', { reason: reason(e) }), 'err'); }
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
      if (act === 'forget' && !(await confirmSheet({ title: t('common.forgetName', { name: b.dataset.name }), text: te('bluetooth.forgetText'), action: t('common.forget') }))) return;
      S.bt = await api('POST', `/api/bluetooth/${act}`, { address: b.dataset.addr });
    }
    renderBluetooth();
  } catch (err) {
    if (err.status !== 401) toast(t('common.didntWork', { reason: reason(err) }), 'err');
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
    if (e.status !== 401 && !S.wifi) body.innerHTML = `<div class="group"><div class="row-empty">${te('wifi.couldntLoad', { reason: reason(e) })}</div></div>`;
  }
  btn.classList.remove('is-busy');
}
$('#wifiRefresh').addEventListener('click', () => loadWifi(true));
function renderWifi() {
  const w = S.wifi, body = $('#wifiBody');
  if (!w.enabled) {
    body.innerHTML = `<div class="note">${icon('alert')}<span>${te('wifi.off')}</span></div>`;
    return;
  }
  const cur = w.current;
  const current = `<div class="group"><div class="row wifi-current">
      <span class="tile t-wifi">${icon('wifi')}</span>
      <div class="row-main"><div class="row-title">${cur ? esc(cur.ssid) : te('common.notConnected')}</div><div class="row-sub">${cur ? te('wifi.connectedIp', { ip: cur.ip || '' }) : te('wifi.pick')}</div></div>
      ${cur ? `<span class="row-value" aria-label="${te('common.signal', { signal: cur.signal })}">${sigSvg(cur.signal ?? 0)}</span>` : ''}
    </div></div>`;
  const saved = w.saved.length
    ? w.saved.map((s) => `<div class="row row-flat"><div class="row-main"><div class="row-title">${esc(s.ssid)}${cur?.ssid === s.ssid ? `<span class="tag ok">${te('common.connected')}</span>` : ''}</div></div>
        <button class="btn btn-sm btn-red-text" type="button" data-forget="${s.id}">${te('common.forget')}</button></div>`).join('')
    : `<div class="row-empty">${te('wifi.noSaved')}</div>`;
  const nearby = [...w.nearby].sort((a, b) => (b.ssid === cur?.ssid) - (a.ssid === cur?.ssid) || b.signal - a.signal || collator.compare(a.ssid, b.ssid));
  const near = nearby.length ? nearby.map((n) => netRow(n, cur)).join('') : `<div class="row-empty">${te('wifi.noneFound')}</div>`;
  body.innerHTML = `${current}<h2 class="gh">${te('wifi.saved')}</h2><div class="group">${saved}</div><h2 class="gh">${te('wifi.nearby')}</h2><div class="group" id="nearby">${near}</div>`;
  if (wifiOpen) $('#nearby .pw input')?.focus();
}
function netRow(n, cur) {
  const connected = cur?.ssid === n.ssid;
  const open = wifiOpen === n.ssid;
  const joining = S.joining?.ssid === n.ssid;
  const secured = n.security !== 'open';
  const label = secured ? t('wifi.netLabelSecured', { name: n.ssid, signal: n.signal }) : t('wifi.netLabel', { name: n.ssid, signal: n.signal });
  const row = `<button class="row row-flat net" type="button" data-ssid="${esc(n.ssid)}" aria-expanded="${open}" aria-label="${esc(label)}"${connected ? ' disabled' : ''}>
      <span class="ico">${secured ? icon('lock') : ''}</span>
      <span class="row-main"><span class="row-title" style="display:block">${esc(n.ssid)}${connected ? `<span class="tag ok">${te('common.connected')}</span>` : n.saved ? `<span class="tag">${te('wifi.savedTag')}</span>` : ''}</span></span>
      ${joining ? `<span class="joining">${spinner()}${te('wifi.joining')}</span>` : ''}
      ${sigSvg(n.signal)}
    </button>`;
  if (!open) return row;
  if (n.security === 'eap') return row + `<div class="join"><div class="note">${icon('info')}<span>${te('wifi.enterprise')}</span></div></div>`;
  const needPw = secured && !n.saved;
  const warn = cur && !connected
    ? `<div class="note">${icon('alert')}<span>${te('wifi.leaveWarn', { current: cur.ssid, name: n.ssid })}</span></div>` : '';
  return row + `<form class="join" data-join="${esc(n.ssid)}">
      ${needPw ? `<div><label class="field-label" for="wifiPw">${te('wifi.passwordFor', { name: n.ssid })}</label><div class="pw"><input type="password" id="wifiPw" autocomplete="off" autocapitalize="off" spellcheck="false" minlength="${n.security === 'psk' ? 8 : 1}" required><button class="btn btn-sm btn-quiet" type="button" data-act="reveal">${te('wifi.show')}</button></div></div>` : ''}
      ${warn}
      <p class="field-err" id="joinErr" hidden></p>
      <div class="join-actions"><button class="btn" type="button" data-act="cancel">${te('common.cancel')}</button><button class="btn btn-primary" type="submit">${te('wifi.join')}</button></div>
    </form>`;
}
$('#wifiBody').addEventListener('click', async (e) => {
  const forget = e.target.closest('[data-forget]');
  if (forget) return forgetNet(+forget.dataset.forget);
  if (e.target.closest('[data-act="reveal"]')) {
    const input = $('#wifiPw'), b = e.target.closest('[data-act="reveal"]');
    input.type = input.type === 'password' ? 'text' : 'password';
    b.textContent = input.type === 'password' ? t('wifi.show') : t('common.hide');
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
    err.textContent = pw.minLength > 1 ? t('wifi.pwShort') : t('wifi.pwType');
    pw.classList.add('is-invalid');
    return pw.focus();
  }
  const submit = $('[type="submit"]', form);
  submit.disabled = true;
  submit.textContent = t('wifi.joining');
  try {
    await api('POST', '/api/wifi/join', pw ? { ssid, password: pw.value } : { ssid });
  } catch (e2) {
    if (e2.status === 401) return;
    submit.disabled = false;
    submit.textContent = t('wifi.join');
    err.hidden = false;
    err.textContent = t('wifi.couldntJoin', { reason: reason(e2) });
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
        toast(t('wifi.connectedTo', { name: j.ssid }));
        poll();
      } else if (S.wifi.lastError === 'auth' && S.wifi.joining === j.ssid) {
        // the Rondo reports the network refused the password: say so at once and reopen the form
        S.joining = null;
        wifiOpen = j.ssid;
        toast(t('wifi.wrongPassword', { name: j.ssid }), 'err');
      } else if (Date.now() > j.until) {
        S.joining = null;
        toast(t('wifi.couldntJoinName', { name: j.ssid }), 'err');
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
    title: t('common.forgetName', { name: net.ssid }),
    text: isCurrent ? te('wifi.forgetTextCurrent') : te('wifi.forgetText'),
    action: t('common.forget'),
  });
  if (!ok) return;
  try {
    await api('DELETE', `/api/wifi/saved?id=${encodeURIComponent(id)}`);
    toast(t('wifi.forgot', { name: net.ssid }));
    loadWifi(true);
  } catch (e) { if (e.status !== 401) toast(t('wifi.couldntForget', { reason: reason(e) }), 'err'); }
}

// ---------- YouTube › Download ----------
// The form (a link or an id: a video at the height chosen, a YouTube Music song saved to Music/YouTube
// Music with its details and cover, or a YouTube Music playlist: all its songs, and a playlist of them
// in Library › Playlists), Pause all / Resume all, the downloaded playlists (update, delete), what's on
// its way (progress, pause, resume, cancel), then the videos and songs on the card (save, delete).
// Asked again every 2 s while one is on its way. One queue on the Rondo: one at a time, videos and
// songs; a paused one keeps what it has and goes on from there, after a restart too.
let dlTimer = 0;
const DL_QUALITY = [[720, '720p'], [480, '480p'], [360, '360p']];
const DL_KIND = [['video', t('downloads.kind.video')], ['song', t('downloads.kind.song')], ['playlist', t('downloads.kind.playlist')]];
const dlGoing = (j) => j.state === 'waiting' || j.state === 'running';
async function loadDownloads() {
  clearTimeout(dlTimer);
  try {
    S.downloads = await api('GET', '/api/downloads');
    renderDownloads();
  } catch (e) {
    if (e.status !== 401 && !S.downloads) $('#dlBody').innerHTML = `<div class="group"><div class="row-empty">${te('downloads.couldntLoad', { reason: reason(e) })}</div></div>`;
  }
  const busy = S.downloads && (S.downloads.jobs.some(dlGoing) || (S.downloads.playlists || []).some((p) => p.loading));
  if (S.route === 'youtube' && busy) dlTimer = setTimeout(loadDownloads, 2000);
}
// what a row on its way says (as the Y1's rows do)
function dlState(j, what) {
  const tail = what ? ` · ${what}` : '';
  // the percent only while a part comes in, as on the Y1 (fetching: the Y1 says so, its stage is in its language)
  const part = !j.stage || j.fetching;
  if (j.state === 'running') return `${j.stage || t('common.downloading')}${part ? ` · ${j.percent}%` : ''}${tail}`;
  if (j.state === 'waiting') return `${S.downloads.held || t('downloads.waiting')}${j.percent ? ` · ${j.percent}%` : ''}${tail}`;
  if (j.state === 'paused') return `${t('common.paused')}${j.percent ? ` · ${j.percent}%` : ''}${tail}`;
  return t('downloads.failed', { error: j.error });
}
function dlPlaylistSub(p) {
  const n = { count: p.total, done: String(p.onCard) };
  if (p.loading) return t('downloads.gettingSongs');
  if (p.going) return t('downloads.listProgress', { ...n, state: p.paused === p.going ? t('common.paused') : t('common.downloading') });
  if (p.onCard < p.total) return t('downloads.listPartial', n);
  return p.mix ? t('downloads.listDoneMix', n) : t('downloads.listDone', n);
}
function renderDownloads() {
  const d = S.downloads, el = $('#dlBody');
  if (!el || !d) return;
  if (!$('#dlForm')) {
    el.innerHTML = `<div class="group"><form class="dl-form" id="dlForm">
        <input type="text" id="dlVideo" placeholder="${te('downloads.placeholder')}" aria-label="${te('downloads.inputLabel')}" autocomplete="off" spellcheck="false">
        ${segHtml('dlKind', DL_KIND, S.dlKind || 'video', t('downloads.as')).replace('seg seg-wide', 'seg')}
        ${segHtml('dlQuality', DL_QUALITY, S.dlQuality || 480, t('downloads.quality')).replace('seg seg-wide', 'seg')}
        <button class="btn btn-primary" type="submit">${icon('download')}<span>${te('common.download')}</span></button></form></div>
      <div id="dlRows"></div>`;
    syncDlKind();
  }
  const jobs = d.jobs.filter((j) => j.state !== 'done').reverse();
  const lists = d.playlists || [];
  const going = jobs.some(dlGoing), paused = jobs.some((j) => j.state === 'paused');
  const group = (title, rows) => rows.length ? `<h3 class="dl-h">${esc(title)}</h3><div class="group">${rows.join('')}</div>` : '';
  const bar = going || paused ? `<div class="group"><div class="row dl-row dl-all">
      <span class="tile ${going ? 't-pause' : 't-resume'}">${icon(going ? 'pause' : 'play')}</span>
      <div class="row-main"><div class="row-title">${going ? te('common.downloading') : te('common.paused')}</div><div class="row-sub">${esc(d.held || (going ? t('downloads.oneAtATime') : t('downloads.eachGoesOn')))}</div></div>
      <button class="btn btn-sm${going ? '' : ' btn-primary'}" type="button" data-dl-all="${going ? 'pause' : 'resume'}">${going ? te('downloads.pauseAll') : te('downloads.resumeAll')}</button></div></div>` : '';
  const listRows = lists.map((p) => `<div class="row dl-row">${p.cover ? `<span class="dl-cover"><img src="${photoUrl(p.cover, 96)}" alt="" loading="lazy"></span>` : `<span class="tile t-playlists">${icon('playlist')}</span>`}
      <div class="row-main"><div class="row-title">${esc(p.title)}</div><div class="row-sub">${esc(dlPlaylistSub(p))}</div>
        ${p.going && p.total ? `<div class="dl-bar"><span style="width:${Math.round((100 * p.onCard) / p.total)}%"></span></div>` : ''}</div>
      ${p.loading ? '' : `<button class="btn btn-sm" type="button" data-dl-update="${esc(p.id)}" aria-label="${te('downloads.updateName', { name: p.title })}">${icon('refresh')}<span class="dl-lbl">${te('downloads.update')}</span></button>`}
      <button class="btn btn-sm btn-red-text" type="button" data-dl-list="${esc(p.id)}" aria-label="${te('downloads.deleteName', { name: p.title })}">${icon('trash')}</button></div>`);
  const jobRows = jobs.map((j) => {
    const what = j.kind === 'song' ? t('downloads.kind.song') : `${j.quality}p`;
    const acts = j.state === 'failed'
      ? `<button class="btn btn-sm" type="button" data-dl-retry="${esc(j.id)}">${te('common.tryAgain')}</button>
         <button class="btn btn-sm btn-quiet" type="button" data-dl-cancel="${esc(j.id)}" aria-label="${te('downloads.takeOff', { name: j.title })}">${icon('close')}</button>`
      : `<button class="btn btn-sm" type="button" data-dl-${j.state === 'paused' ? 'resume' : 'pause'}="${esc(j.id)}">${j.state === 'paused' ? te('downloads.resume') : te('common.pause')}</button>
         <button class="btn btn-sm btn-quiet" type="button" data-dl-cancel="${esc(j.id)}" aria-label="${te('common.cancelName', { name: j.title })}">${icon('close')}</button>`;
    return `<div class="row dl-row"><span class="tile t-downloads">${icon('download')}</span>
      <div class="row-main"><div class="row-title">${esc(j.title)}</div><div class="row-sub">${esc(dlState(j, what))}</div>
        ${j.state === 'running' || (j.state !== 'failed' && j.percent) ? `<div class="dl-bar${j.state === 'running' ? '' : ' is-still'}"><span style="width:${j.percent}%"></span></div>` : ''}</div>
      ${acts}</div>`;
  });
  const cardRows = [
    ...(d.songs || []).map((x) => `<div class="row dl-row"><span class="tile t-music">${icon('note')}</span>
        <div class="row-main"><div class="row-title">${esc(x.title)}</div><div class="row-sub">${esc(x.subtitle)}</div></div>
        <a class="btn btn-sm" href="/api/files/download?path=${encodeURIComponent(x.file)}" download>${te('common.save')}</a>
        <button class="btn btn-sm btn-red-text" type="button" data-dl-delete="${esc(x.file)}" data-song="1" aria-label="${te('downloads.deleteName', { name: x.title })}">${icon('trash')}</button></div>`),
    ...d.files.map((x) => `<div class="row dl-row"><span class="tile t-videos">${icon('film')}</span>
        <div class="row-main"><div class="row-title">${esc(x.title)}</div><div class="row-sub">${esc(x.subtitle)}</div></div>
        <a class="btn btn-sm" href="/api/files/download?path=${encodeURIComponent(x.file)}" download>${te('common.save')}</a>
        <button class="btn btn-sm btn-red-text" type="button" data-dl-delete="${esc(x.file)}" aria-label="${te('downloads.deleteName', { name: x.title })}">${icon('trash')}</button></div>`),
  ];
  const rowsHtml = bar + group(t('common.playlists'), listRows) + group(t('downloads.onTheWay'), jobRows) + group(t('common.onCard'), cardRows);
  $('#dlRows').innerHTML = rowsHtml || `<div class="group"><div class="row-empty">${te('downloads.nothing')}</div></div>`;
}
// a song or a playlist has no height to pick
function syncDlKind() {
  const q = $('.seg[data-key="dlQuality"]');
  if (q) q.hidden = (S.dlKind || 'video') !== 'video';
}
function setDlKind(kind) {
  S.dlKind = kind;
  $$('button', $('.seg[data-key="dlKind"]')).forEach((x) => x.setAttribute('aria-checked', String(JSON.parse(x.dataset.v) === kind)));
  syncDlKind();
}
$('#dlBody').addEventListener('input', (e) => {
  if (e.target.id !== 'dlVideo') return;
  const v = e.target.value;
  // a playlist's link: the playlist; a YouTube Music link: a song
  if (/[?&]list=/.test(v) && !/[?&]v=/.test(v) || /\/browse\/VL/.test(v)) setDlKind('playlist');
  else if (/music\.youtube\.com/i.test(v)) setDlKind('song');
});
// delete a downloaded playlist: its songs stay, or go too
function dlListSheet(p) {
  return new Promise((resolve) => {
    openSheet(`<h2>${te('common.deleteName', { name: p.title })}</h2><p>${te('downloads.deleteListText')}</p>
      <div class="sheet-actions sheet-actions-3"><button class="btn" type="button" data-r="">${te('common.cancel')}</button><button class="btn btn-danger" type="button" data-r="songs">${te('downloads.withSongs')}</button><button class="btn btn-primary" type="button" data-r="keep">${te('downloads.keepSongs')}</button></div>`);
    const onClick = (e) => { const b = e.target.closest('[data-r]'); if (b) finish(b.dataset.r); };
    const finish = (v) => { sheet.removeEventListener('click', onClick); sheet.removeEventListener('close', onClose); if (sheet.open) sheet.close(); resolve(v); };
    const onClose = () => finish('');
    sheet.addEventListener('click', onClick);
    sheet.addEventListener('close', onClose);
  });
}
$('#dlBody').addEventListener('click', async (e) => {
  const seg = e.target.closest('.seg[data-key="dlQuality"], .seg[data-key="dlKind"]'), b = e.target.closest('[data-v]');
  if (seg && b) {
    $$('button', seg).forEach((x) => x.setAttribute('aria-checked', String(x === b)));
    if (seg.dataset.key === 'dlKind') S.dlKind = JSON.parse(b.dataset.v), syncDlKind();
    else S.dlQuality = JSON.parse(b.dataset.v);
    return;
  }
  const hit = (k) => e.target.closest(`[data-dl-${k}]`);
  const cancel = hit('cancel'), retry = hit('retry'), del = hit('delete'), pause = hit('pause'), resume = hit('resume'), all = hit('all'), update = hit('update'), list = hit('list');
  try {
    if (cancel) S.downloads = await api('POST', '/api/downloads/cancel', { id: cancel.dataset.dlCancel });
    else if (retry) S.downloads = await api('POST', '/api/downloads/retry', { id: retry.dataset.dlRetry });
    else if (pause) S.downloads = await api('POST', '/api/downloads/pause', { id: pause.dataset.dlPause });
    else if (resume) S.downloads = await api('POST', '/api/downloads/resume', { id: resume.dataset.dlResume });
    else if (all) S.downloads = await api('POST', `/api/downloads/${all.dataset.dlAll}`, {});
    else if (update) {
      S.downloads = await api('POST', `/api/downloads/playlist?id=${encodeURIComponent(update.dataset.dlUpdate)}`, {});
      toast(t('downloads.updating'));
    } else if (list) {
      const p = (S.downloads.playlists || []).find((x) => x.id === list.dataset.dlList);
      const how = p && await dlListSheet(p);
      if (!how) return;
      S.downloads = await api('DELETE', `/api/downloads/playlist?id=${encodeURIComponent(p.id)}${how === 'songs' ? '&songs=1' : ''}`);
      S.playlists = null;  // Library's playlists changed
      toast(how === 'songs' ? t('downloads.deletedWithSongs') : t('downloads.deletedKeepSongs'));
    } else if (del) {
      const name = del.closest('.row').querySelector('.row-title').textContent;
      if (!await confirmSheet({ title: t('common.deleteName', { name }), text: del.dataset.song ? te('downloads.deleteSongText') : te('downloads.deleteVideoText'), action: t('common.delete') })) return;
      S.downloads = await api('DELETE', `/api/downloads?file=${encodeURIComponent(del.dataset.dlDelete)}`);
      toast(t('downloads.deleted'));
    } else return;
    renderDownloads();
    loadDownloads();
  } catch (err) { if (err.status !== 401) toast(reason(err), 'err'); }
});
$('#dlBody').addEventListener('submit', async (e) => {
  e.preventDefault();
  const input = $('#dlVideo'), video = input.value.trim(), kind = S.dlKind || 'video';
  if (!video) return input.focus();
  try {
    S.downloads = await api('POST', '/api/downloads', { video, quality: S.dlQuality || 480, song: kind === 'song', playlist: kind === 'playlist' }, { timeout: 70000 });
    input.value = '';
    toast(kind === 'playlist' ? t('downloads.startedPlaylist') : t('downloads.started'));
    renderDownloads();
    loadDownloads();
  } catch (err) { if (err.status !== 401) toast(reason(err), 'err'); }
});

// ---------- the Rondo server, and where YouTube comes from ----------
async function loadServer() {
  const skel = '<div class="group"><div class="lib-skel"></div><div class="lib-skel"></div></div>';
  if (!S.server) $('#serverBody').innerHTML = skel, $('#ytBody').innerHTML = skel;
  try {
    S.server = await api('GET', '/api/server');
    renderServer();
    renderYouTube();
  } catch (e) {
    const msg = `<div class="group"><div class="row-empty">${te('settings.couldntLoad', { reason: reason(e) })}</div></div>`;
    if (e.status !== 401 && !S.server) $('#serverBody').innerHTML = msg, $('#ytBody').innerHTML = msg;
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
    cd.textContent = left ? t('youtube.expiresIn', { time: `${Math.floor(left / 60)}:${String(left % 60).padStart(2, '0')}` }) : t('youtube.codeExpired');
  }, 1000);
  tvTimer = setTimeout(async () => {
    if (S.route !== 'youtube') return;
    try {
      const was = S.ytTv;
      S.ytTv = await api('GET', '/api/youtube/tv', undefined, { timeout: 30000 });
      if (!was?.signedIn && S.ytTv.signedIn) toast(S.ytTv.name ? t('common.signedInAs', { name: S.ytTv.name }) : t('common.signedIn'));
      if (S.ytTv.error && !S.ytTv.pending && was?.pending) toast(TV_ERRORS[S.ytTv.error] || t('youtube.signInFailed'), 'err');
      renderAccount();
    } catch (e) { if (e.status === 401) return; }
    watchTv();
  }, 3000);
}
const TV_ERRORS = {
  denied: t('youtube.tv.denied'),
  expired: t('youtube.tv.expired'),
  revoked: t('youtube.tv.revoked'),
  failed: t('youtube.tv.failed'),
};
async function tvAct(action) {
  try {
    S.ytTv = await api('POST', `/api/youtube/tv/${action}`, {}, { timeout: 45000 });
    renderAccount();
    watchTv();
    if (action === 'signout') toast(t('common.signedOutOnRondo'));
  } catch (e) { if (e.status !== 401) toast(reason(e), 'err'); }
}
function tvHtml() {
  const tv = S.ytTv;
  if (!tv) return `<div class="row set-row"><span class="tile t-account">${icon('key')}</span><div class="row-main"><div class="row-title">${te('youtube.signInCode')}</div><div class="row-sub">${te('common.checking')}</div></div></div>`;
  if (tv.pending) {
    const link = `<a class="link" href="${esc(tv.pending.url)}" target="_blank" rel="noopener noreferrer"><b>${esc(tv.pending.url.replace(/^https?:\/\//, ''))}</b></a>`;
    return `<div class="row row-wide set-row tv-pending"><span class="tile t-account">${icon('key')}</span>
      <div class="row-main"><div class="row-title">${te('youtube.enterCode')}</div>
        <div class="tv-code" aria-label="${te('youtube.theCode')}">${esc(tv.pending.code)}</div>
        <ol class="steps tv-steps">
          <li>${th('youtube.step1', { link: html(link) })}</li>
          <li>${te('youtube.step2')}</li>
          <li>${te('youtube.step3')}</li>
        </ol>
        <div class="row-sub"><span id="tvCountdown"></span> · ${te('youtube.waitingGoogle')}</div></div>
      <div class="row-control acct-actions"><button class="btn btn-sm" type="button" data-act="tv-copy">${te('youtube.copyCode')}</button><button class="btn btn-sm" type="button" data-act="tv-cancel">${te('common.cancel')}</button></div></div>`;
  }
  if (tv.signedIn) {
    return `<div class="row row-wide set-row"><span class="tile t-account">${icon('key')}</span>
      <div class="row-main"><div class="row-title">${tv.name ? te('common.signedInAs', { name: tv.name }) : te('common.signedIn')}</div><div class="row-sub">${tv.handle ? `${esc(tv.handle)} · ` : ''}${te('youtube.signedInFeeds')}</div></div>
      <div class="row-control acct-actions"><button class="btn btn-sm btn-red-text" type="button" data-act="tv-signout">${te('common.signOut')}</button></div></div>`;
  }
  const err = tv.error && TV_ERRORS[tv.error] ? `<div class="row-sub tv-err">${esc(TV_ERRORS[tv.error])}</div>` : '';
  return `<div class="row row-wide set-row"><span class="tile t-account">${icon('key')}</span>
      <div class="row-main"><div class="row-title">${te('youtube.signInCode')}</div><div class="row-sub">${te('youtube.signInCodeSub')}</div>${err}</div>
      <div class="row-control acct-actions"><button class="btn btn-primary btn-sm" type="button" data-act="tv-begin">${icon('key')}<span>${te('youtube.getCode')}</span></button></div></div>`;
}
function renderAccount() {
  const el = $('#ytAccount');
  if (!el) return;
  const a = S.ytAccount;
  if (!S.server?.direct) {
    el.innerHTML = `<div class="group"><div class="row set-row"><span class="tile t-account">${icon('key')}</span>
      <div class="row-main"><div class="row-title">${te('youtube.account')}</div><div class="row-sub">${S.server?.url ? th('youtube.accountServerLink', { link: html(`<a class="link" href="${esc(S.server.url.replace(/\/+$/, ''))}/admin" target="_blank" rel="noopener">${te('common.openIt')}</a>`) }) : te('youtube.accountServer')}</div></div></div></div>`;
    return;
  }
  const state = !a ? te('common.checking') : a.signedIn ? `${a.name ? th('youtube.cookiesSignedInAs', { name: a.name }) : te('common.signedIn')}${a.handle ? ` (${esc(a.handle)})` : ''}${a.unchecked ? ` ${te('youtube.notChecked')}` : ''}`
    : a.expired ? te('youtube.cookiesExpired') : a.unreachable ? te('youtube.couldntAsk') : te('common.signedOut');
  el.innerHTML = `<h2 class="gh">${te('youtube.account')}</h2><div class="group">${tvHtml()}</div>
    <h2 class="gh">${te('youtube.musicHome')}</h2>
    <div class="group">
      <div class="row row-wide set-row"><span class="tile t-cookies">${icon('file')}</span>
        <div class="row-main"><div class="row-title">cookies.txt</div><div class="row-sub">${state}</div>
          <div class="row-sub">${te('youtube.cookiesOptional')}</div></div>
        <div class="row-control acct-actions">
          <button class="btn btn-primary btn-sm" type="button" data-act="cookies">${icon('upload')}<span>${a?.signedIn ? te('youtube.replaceCookies') : te('youtube.uploadCookies')}</span></button>
          ${a?.signedIn || a?.expired ? `<button class="btn btn-sm" type="button" data-act="signout">${te('common.signOut')}</button>` : ''}
          <input type="file" id="cookiesFile" accept=".txt,text/plain" hidden></div></div>
    </div>
    <h2 class="gh">${te('youtube.exportTitle')}</h2>
    <div class="group"><ol class="steps">
      <li>${th('youtube.export1')}</li>
      <li>${th('youtube.export2')}</li>
      <li>${th('youtube.export3')}</li>
      <li>${th('youtube.export4')}</li>
    </ol><p class="row-sub steps-note">${te('youtube.exportNote')}</p></div>`;
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
  if (file.size > 4 * 1024 * 1024) return toast(t('youtube.cookiesTooBig'), 'err');
  const text = youtubeCookies(await file.text());
  try {
    S.ytAccount = await api('PUT', '/api/youtube/cookies', { cookies: text }, { timeout: 40000 });
    renderAccount();
    toast(S.ytAccount.signedIn ? (S.ytAccount.name ? t('common.signedInAs', { name: S.ytAccount.name }) : t('common.signedIn')) : t('youtube.cookiesRefused'), S.ytAccount.signedIn ? undefined : 'err');
  } catch (e) { if (e.status !== 401) toast(reason(e), 'err'); }
}
function renderQuality() {
  const el = $('#ytQuality');
  if (!el || !S.settings) return;
  el.innerHTML = `<div class="group"><div class="row row-wide set-row"><span class="tile t-quality">${icon('diamond')}</span>
      <div class="row-main"><div class="row-title">${te('youtube.quality')}</div><div class="row-sub">${te('youtube.qualitySub')}</div></div>
      <div class="row-control">${segHtml('videoQuality', QUALITY, S.settings.videoQuality ?? null, t('youtube.quality'))}</div></div></div>`;
}
// YouTube's page: where it comes from (directly, or through the server when one is connected), the
// account, playback
function renderYouTube() {
  const s = S.server, el = $('#ytBody');
  if (!el || !s) return;
  el.innerHTML = `
    <div class="group">
      <div class="row row-wide set-row">${brandTile('video')}
        <div class="row-main"><div class="row-title">${te('youtube.connectTo')}</div><div class="row-sub">${s.direct ? (s.url ? te('youtube.direct') : th('youtube.directNoServer', { link: html(`<a class="link" href="#server">${te('nav.server')}</a>`) })) : te('youtube.viaServer')}</div></div>
        <div class="row-control"><div class="seg seg-wide" role="radiogroup" aria-label="${te('youtube.connectTo')}" id="srvMode">
          <button type="button" role="radio" aria-checked="${!!s.direct}" data-direct="1">${te('youtube.directly')}</button>
          ${s.url ? `<button type="button" role="radio" aria-checked="${!s.direct}" data-direct="0">${te('youtube.myServer')}</button>` : ''}</div></div></div>
    </div>
    <div id="ytAccount"></div>
    <h2 class="gh">${te('youtube.playback')}</h2>
    <div id="ytQuality"></div>`;
  renderAccount();
  renderQuality();
}
// the Server page: the server, connecting to one, the test, and what uses it
function renderServer() {
  const s = S.server;
  const admin = s.url ? `${s.url.replace(/\/+$/, '')}/admin` : '';
  $('#serverBody').innerHTML = `
    <div class="group">
      <div class="srv-field">
        <div class="row-title">${te('server.yours')}</div>
        <div class="row-sub srv-now"><span class="srv-url">${esc(s.url || t('server.none'))}</span>
          ${s.url ? `<span class="tag ${insecure(s.url) ? 'warn' : 'ok'}">${insecure(s.url) ? te('server.notEncrypted') : te('server.encrypted')}</span>` : ''}
          <span class="tag ${s.tokenSet ? 'ok' : 'warn'}">${s.tokenSet ? te('common.connected') : te('common.notConnected')}</span></div>
        ${insecure(s.url) ? `<p class="row-sub srv-warn">${te('server.plainHttp')}</p>` : ''}
        ${admin ? `<p class="row-sub"><a class="link" href="${esc(admin)}" target="_blank" rel="noopener">${te('server.openAdmin')}</a></p>` : ''}
      </div>
    </div>
    <div class="group">
      <form class="srv-field" id="connectForm">
        <div class="row-title">${te('server.connectTo')}</div>
        <p class="row-sub">${te('server.connectHelp')}</p>
        <label class="field-label" for="srvUrl" style="margin-top:12px">${te('server.address')}</label>
        <div class="field-row"><input type="url" id="srvUrl" value="${esc(s.url || '')}" placeholder="${te('server.addressPlaceholder')}" inputmode="url" autocapitalize="off" autocomplete="url" spellcheck="false"></div>
        <p class="row-sub srv-warn" id="urlWarn" ${insecure(s.url) ? '' : 'hidden'}>${te('server.plainHttpWarn')}</p>
        <label class="field-label" for="srvCode" style="margin-top:12px">${te('server.code')}</label>
        <div class="field-row"><input type="text" id="srvCode" inputmode="numeric" autocomplete="one-time-code" placeholder="123 456" spellcheck="false"></div>
        <p class="field-err" id="urlErr" hidden></p>
        <div class="join-actions" style="margin-top:12px"><button class="btn" type="button" id="urlOnly">${te('server.saveAddress')}</button><button class="btn btn-primary" type="submit" id="connectBtn">${te('common.connect')}</button></div>
      </form>
    </div>
    <div class="group">
      <div class="srv-field">
        <div class="srv-token">
          <div class="row-main"><div class="row-title">${te('server.token')} <span class="tag ${s.tokenSet ? 'ok' : 'warn'}">${s.tokenSet ? te('server.set') : te('server.notSet')}</span></div>
            <div class="row-sub">${te('server.tokenHelp')}</div></div>
          <button class="btn btn-sm" type="button" id="tokenEdit">${s.tokenSet ? te('server.replaceToken') : te('server.addToken')}</button>
        </div>
        <form id="tokenForm" hidden style="margin-top:14px">
          <label class="field-label" for="srvToken">${te('server.newToken')}</label>
          <div class="field-row"><input type="password" id="srvToken" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="${te('server.tokenPlaceholder')}"></div>
          <p class="field-err" id="tokenErr" hidden></p>
          <div class="join-actions" style="margin-top:12px"><button class="btn" type="button" id="tokenCancel">${te('common.cancel')}</button><button class="btn btn-primary" type="submit">${te('server.saveToken')}</button></div>
        </form>
      </div>
    </div>
    <div class="group test">
      <div class="test-head"><div class="row-main"><div class="row-title">${te('server.test')}</div><div class="row-sub">${te('server.testSub')}</div></div>
        <button class="btn btn-primary" type="button" id="srvTest">${icon('refresh')}<span>${te('server.testButton')}</span></button></div>
      <div class="test-res" id="testRes" hidden></div>
    </div>
    <h2 class="gh">${te('server.usedFor')}</h2>
    <div class="group">
      <a class="row more-row" href="#youtube">${brandTile('video')}
        <div class="row-main"><div class="row-title">YouTube</div><div class="row-sub">${!s.url ? te('server.ytDirect') : s.direct ? te('server.ytDirectSwitch') : te('server.ytVia')}</div></div>${icon('chevron', 'chev')}</a>
      <div class="row set-row">${brandTile('whatsapp')}
        <div class="row-main"><div class="row-title">WhatsApp</div><div class="row-sub">${s.url ? (admin ? th('server.waLinkOpen', { link: html(`<a class="link" href="${esc(admin)}#whatsapp" target="_blank" rel="noopener">${te('common.openIt')}</a>`) }) : te('server.waLink')) : te('server.waNone')}</div></div></div>
    </div>`;
}
const serverBody = $('#serverBody'), ytBody = $('#ytBody');
ytBody.addEventListener('change', (e) => { if (e.target.id === 'cookiesFile' && e.target.files[0]) sendCookies(e.target.files[0]); });
ytBody.addEventListener('click', async (e) => {
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
    try { await navigator.clipboard.writeText(S.ytTv?.pending?.code || ''); toast(t('youtube.codeCopied')); } catch { toast(t('youtube.couldntCopy'), 'err'); }
    return;
  }
  if (e.target.closest('[data-act="signout"]')) {
    try {
      S.ytAccount = await api('DELETE', '/api/youtube/cookies', undefined, { timeout: 30000 });
      renderAccount();
      toast(t('common.signedOutOnRondo'));
    } catch (err) { if (err.status !== 401) toast(reason(err), 'err'); }
    return;
  }
  const b = e.target.closest('#srvMode [data-direct]');
  if (!b || b.disabled || b.getAttribute('aria-checked') === 'true') return;
  try {
    S.server = await api('PUT', '/api/server', { direct: b.dataset.direct === '1' });
    renderServer();
    renderYouTube();
    loadYtAccount();
    toast(t('common.savedOnRondo'));
  } catch (err) { if (err.status !== 401) toast(t('common.notSaved', { reason: reason(err) }), 'err'); }
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
  err.textContent = t('server.badUrl');
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
      toast(t('server.addressSaved'));
    } catch (e2) { if (e2.status !== 401) { $('#urlErr').hidden = false; $('#urlErr').textContent = t('common.notSaved', { reason: reason(e2) }); } }
  }
});
serverBody.addEventListener('submit', async (e) => {
  e.preventDefault();
  if (e.target.id === 'connectForm') {
    const url = serverUrl();
    if (!url) return;
    const codeInput = $('#srvCode'), err = $('#urlErr'), btn = $('#connectBtn');
    const code = codeInput.value.replace(/\D/g, '');
    if (code.length !== 6) { codeInput.classList.add('is-invalid'); err.hidden = false; err.textContent = t('server.codeNeeded'); return codeInput.focus(); }
    btn.disabled = true;
    btn.textContent = t('common.connecting');
    try {
      S.server = await api('PUT', '/api/server', { url, code });
      renderServer();
      toast(t('server.connected'));
    } catch (e2) {
      if (e2.status === 401) return;
      btn.disabled = false;
      btn.textContent = t('common.connect');
      err.hidden = false;
      err.textContent = e2.status === 403 ? t('server.wrongCode')
        : e2.code === 'unreachable' ? t('server.unreachable')
        : t('server.couldntConnect', { reason: reason(e2) });
    }
  }
  if (e.target.id === 'tokenForm') {
    const input = $('#srvToken'), err = $('#tokenErr');
    const v = input.value.trim();
    if (!v) { err.hidden = false; err.textContent = t('server.pasteFirst'); return input.focus(); }
    try {
      S.server = await api('PUT', '/api/server', { token: v });
      renderServer();
      toast(t('server.tokenSaved'));
    } catch (e2) { if (e2.status !== 401) { err.hidden = false; err.textContent = t('common.notSaved', { reason: reason(e2) }); } }
  }
});
// one fact of the connection test: a check (or an alert when not ok), title, note, value
function testRow(ok, title, sub, value, detail = '', muted = false) {
  const ico = ok ? (muted ? '<svg class="i" aria-hidden="true"><use href="#i-info"/></svg>' : '<svg class="i ok-ico" aria-hidden="true"><use href="#i-check"/></svg>')
    : '<svg class="i" aria-hidden="true" style="color:var(--amber)"><use href="#i-alert"/></svg>';
  const subs = [sub, detail].filter(Boolean).map((x) => `<div class="row-sub">${esc(x)}</div>`).join('');
  return `<div class="row">${ico}<div class="row-main"><div class="row-title">${esc(title)}</div>${subs}</div><div class="row-value">${esc(value)}</div></div>`;
}
async function testServer() {
  const btn = $('#srvTest'), res = $('#testRes');
  btn.disabled = true;
  btn.classList.add('is-busy');
  $('span', btn).textContent = t('server.testing');
  let r;
  try { r = await api('POST', '/api/server/test', undefined, { timeout: 30000 }); } catch (e) {
    if (e.status === 401) return;
    r = { ok: false, error: e.status === 0 ? 'rondo' : e.code || t('server.errorStatus', { status: String(e.status) }) };
  }
  btn.disabled = false;
  btn.classList.remove('is-busy');
  $('span', btn).textContent = t('server.testAgain');
  res.hidden = false;
  if (r.ok) {
    res.innerHTML = `
      <div class="row"><svg class="i ok-ico" aria-hidden="true"><use href="#i-check"/></svg><div class="row-main"><div class="row-title">${te('common.connected')}</div></div><div class="row-value strong">${r.latencyMs} ms</div></div>
      ${r.versionOk === false ? testRow(false, t('server.needsUpdate'), t('server.needsUpdateText', { version: String(r.versionNeeded) }), r.version || t('server.olderThan', { version: '3.1.0' })) : testRow(true, t('nav.server'), '', r.version ? t('server.version', { version: String(r.version) }) : '')}
      ${testRow(r.playbackOk !== false, t('youtube.playback'), r.playbackOk === false ? t('server.playbackFailed') : '', r.playbackOk === false ? t('server.failing') : r.playbackOk ? t('server.working') : t('server.notChecked'), r.ytdlp ? `yt-dlp ${r.ytdlp}` : '')}
      ${testRow(true, t('youtube.account'), r.signedIn ? '' : t('server.noAccount'), r.signedIn ? t('common.signedIn') : t('common.signedOut'), '', !r.signedIn)}`;
  } else {
    const msg = {
      unreachable: [t('server.cantReach'), t('server.cantReachText')],
      unauthorized: [t('server.refused'), t('server.refusedText')],
      rondo: [t('server.rondoNoAnswer'), t('server.rondoNoAnswerText')],
      none: [t('server.noServer'), t('server.noServerText')],
    }[r.error] || [t('server.testFailed'), r.error || t('server.noDetails')];
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
    if (e.status !== 401 && !S.dateTime) $('#dateRows').innerHTML = `<div class="row-empty">${te('settings.date.couldntLoad', { reason: reason(e) })}</div>`;
  }
}
// the Rondo's clock now, as a datetime-local value in its own time zone
function rondoLocal(d) {
  const at = new Date((d.now + d.offsetSec) * 1000 + (Date.now() - S.dateTimeAt));
  return at.toISOString().slice(0, 16);
}
function rondoClock(d) {
  const at = new Date((d.now + d.offsetSec) * 1000 + (Date.now() - S.dateTimeAt));
  return at.toLocaleString(dateLocale(), { timeZone: 'UTC', dateStyle: 'medium', timeStyle: 'short', hour12: !d.clock24 });
}
function renderDateTime() {
  const d = S.dateTime;
  if (!d) return;
  const zones = d.zones.map((z) => `<option value="${esc(z.id)}"${z.id === d.zone ? ' selected' : ''}>${esc(z.gmt)} · ${esc(z.city)}</option>`).join('');
  $('#dateRows').innerHTML = `
    <div class="row set-row"><span class="tile t-sync">${icon('sync')}</span>
      <div class="row-main"><div class="row-title">${te('settings.date.auto')}</div><div class="row-sub">${th('settings.date.autoSub', { now: html(`<b id="dtNow">${esc(rondoClock(d))}</b>`) })}</div></div>
      <button class="switch" type="button" role="switch" data-dt="autoTime" aria-checked="${d.autoTime}" aria-label="${te('settings.date.autoLabel')}"></button></div>
    ${d.autoTime ? '' : `<div class="row row-wide set-row"><span class="tile t-clock">${icon('clock')}</span>
      <div class="row-main"><div class="row-title">${te('settings.date.dateTime')}</div><div class="row-sub">${te('settings.date.dateTimeSub')}</div></div>
      <div class="row-control date-set"><input type="datetime-local" id="dtValue" value="${esc(rondoLocal(d))}" aria-label="${te('settings.date.dateTime')}">
        <button class="btn btn-sm" type="button" id="dtFromHere">${te('settings.date.fromHere')}</button><button class="btn btn-sm btn-primary" type="button" id="dtSet">${te('settings.date.set')}</button></div></div>`}
    <div class="row set-row"><span class="tile t-zone">${icon('broadcast')}</span>
      <div class="row-main"><div class="row-title">${te('settings.date.autoZone')}</div><div class="row-sub">${te('settings.date.autoZoneSub', { city: d.city, zone: d.zone })}</div></div>
      <button class="switch" type="button" role="switch" data-dt="autoZone" aria-checked="${d.autoZone}" aria-label="${te('settings.date.autoZoneLabel')}"></button></div>
    ${d.autoZone ? '' : `<div class="row row-wide set-row"><span class="tile t-globe">${icon('globe')}</span>
      <div class="row-main"><div class="row-title">${te('settings.date.zone')}</div></div>
      <div class="row-control"><div class="select"><select id="dtZone" aria-label="${te('settings.date.zone')}">${zones}</select>${icon('down')}</div></div></div>`}
    <div class="row set-row"><span class="tile t-timer">${icon('timer')}</span>
      <div class="row-main"><div class="row-title">${te('settings.date.clock24')}</div><div class="row-sub">${te('settings.date.clock24Sub')}</div></div>
      <button class="switch" type="button" role="switch" data-dt="clock24" aria-checked="${d.clock24}" aria-label="${te('settings.date.clock24')}"></button></div>`;
}
async function putDateTime(body) {
  try {
    S.dateTime = await api('PUT', '/api/datetime', body);
    S.dateTimeAt = Date.now();
    renderDateTime();
    toast(t('common.savedOnRondo'));
  } catch (e) {
    if (e.status !== 401) toast(t('common.notSaved', { reason: reason(e) }), 'err');
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
const BOOSTS = [[0, t('common.off')], [3, '+3 dB'], [6, '+6 dB'], [9, '+9 dB'], [12, '+12 dB']];
const AMPS = [[0, t('sound.standard')], [1, '+2 dB'], [2, '+4 dB'], [3, '+6 dB']];
const freqName = (f) => (f >= 1000 ? `${f / 1000}k` : String(f));
const gainName = (g) => (g > 0 ? `+${g.toFixed(1)}` : g.toFixed(1)).replace('.0', '');
async function loadSound(quiet = false) {
  if (!S.sound && !quiet) $('#soundBody').innerHTML = '<div class="group"><div class="lib-skel"></div><div class="lib-skel"></div><div class="lib-skel"></div></div>';
  try {
    S.sound = await api('GET', '/api/sound');
    renderSound();
  } catch (e) {
    if (e.status !== 401 && !S.sound) $('#soundBody').innerHTML = `<div class="group"><div class="row-empty">${te('sound.couldntLoad', { reason: reason(e) })} <button class="link" type="button" data-act="reload">${te('common.tryAgain')}</button></div></div>`;
  }
}
function soundOutput() {
  const s = S.sound;
  if (!soundOut || !s.outputs.some((o) => o.key === soundOut)) soundOut = s.current;
  return s.outputs.find((o) => o.key === soundOut) || s.outputs[0];
}
function renderSound() {
  const s = S.sound, o = soundOutput();
  const outs = s.outputs.map((x) => `<button type="button" role="radio" aria-checked="${x.key === o.key}" data-out="${esc(x.key)}">${x.key === s.current ? te('sound.outputPlaying', { name: x.label }) : esc(x.label)}</button>`).join('');
  const presets = [...s.presets.map((p) => [p.id, p.name + (p.note ? ` (${p.note})` : '')]), ...s.userPresets.map((u) => ['user:' + u.name, u.name]),
    ...(o.match ? [['autoeq', t('sound.matchPreset', { name: o.match.name })]] : []), ['custom', t('sound.custom')]];
  const bands = s.freqs.map((fr, i) => `<label class="eq-band"><output>${gainName(o.gains[i])}</output>
      <input type="range" min="-12" max="12" step="0.5" value="${o.gains[i]}" data-band="${i}" aria-label="${freqName(fr)} Hz"><span>${freqName(fr)}</span></label>`).join('');
  const user = o.preset.startsWith('user:') ? o.preset.slice(5) : '';
  const limitName = (v) => (v >= s.maxVolume ? t('sound.noLimit') : t('common.nOfTotal', { n: v, total: s.maxVolume }));
  $('#soundBody').innerHTML = `
    <div class="group"><div class="row row-wide set-row"><span class="tile t-output">${icon('headphones')}</span>
      <div class="row-main"><div class="row-title">${te('sound.output')}</div><div class="row-sub">${te('sound.outputSub')}</div></div>
      <div class="row-control"><div class="seg seg-wide sound-out" role="radiogroup" aria-label="${te('sound.output')}" id="soundOuts">${outs}</div></div></div></div>
    <h2 class="gh">${esc(o.label)}</h2>
    <div class="group">
      <div class="row row-wide set-row"><span class="tile t-eq">${icon('sliders')}</span>
        <div class="row-main"><div class="row-title">${te('sound.equalizer')}</div><div class="row-sub">${te('sound.equalizerSub')}</div></div>
        <div class="row-control"><div class="select"><select id="eqPreset" aria-label="${te('sound.presetLabel')}">${presets.map(([v, l]) => `<option value="${esc(v)}"${v === o.preset ? ' selected' : ''}>${esc(l)}</option>`).join('')}</select>${icon('down')}</div></div></div>
      <div class="eq" id="eqBands">${bands}</div>
      <div class="eq-actions"><button class="btn btn-sm" type="button" id="eqFlat">${te('sound.flat')}</button><button class="btn btn-sm" type="button" id="eqSave">${icon('plus')}<span>${te('sound.saveAs')}</span></button>
        ${user ? `<button class="btn btn-sm btn-red-text" type="button" id="eqDelete" data-name="${esc(user)}">${icon('trash')}<span>${te('sound.deletePreset', { name: user })}</span></button>` : ''}</div>
      ${o.key === 'speaker' ? '' : `<div class="row set-row"><span class="tile t-search">${icon('search')}</span>
        <div class="row-main"><div class="row-title">${te('sound.match')}</div><div class="row-sub">${o.match ? (o.preset === 'autoeq' ? te('sound.matchInUse', { name: o.match.name, source: o.match.source }) : te('sound.matchSub', { name: o.match.name, source: o.match.source })) : te('sound.matchNone')}</div></div>
        <button class="btn btn-sm" type="button" id="matchFind">${o.match ? te('sound.changeModel') : te('sound.findModel')}</button></div>`}
      <div class="row row-wide set-row"><span class="tile t-now">${icon('volume')}</span>
        <div class="row-main"><div class="row-title">${te('sound.boost')}</div><div class="row-sub">${te('sound.boostSub')}</div></div>
        <div class="row-control">${segHtml('boostDb', BOOSTS, o.boostDb, t('sound.boost'))}</div></div>
    </div>
    <h2 class="gh">${te('sound.every')}</h2>
    <div class="group">
      <div class="row set-row"><span class="tile t-level">${icon('gauge')}</span>
        <div class="row-main"><div class="row-title">${te('sound.leveling')}</div><div class="row-sub">${esc(s.levelingNote)}</div></div>
        <button class="switch" type="button" role="switch" data-key="leveling" aria-checked="${s.leveling}" aria-label="${te('sound.leveling')}"></button></div>
      ${s.amplifier >= 0 ? `<div class="row row-wide set-row"><span class="tile t-amp">${icon('bolt')}</span>
        <div class="row-main"><div class="row-title">${te('sound.amplifier')}</div><div class="row-sub">${te('sound.amplifierSub')}</div></div>
        <div class="row-control">${segHtml('amplifier', AMPS, s.amplifier, t('sound.amplifier'))}</div></div>` : ''}
      <div class="row row-wide set-row"><span class="tile t-limit">${icon('lock')}</span>
        <div class="row-main"><div class="row-title">${te('sound.limit')}</div><div class="row-sub">${te('sound.limitSub')}</div></div>
        <div class="row-control"><div class="range-row">${icon('volume')}<input type="range" id="volLimit" min="1" max="${s.maxVolume}" step="1" value="${s.volumeLimit}" aria-label="${te('sound.limit')}"><output id="volLimitOut">${esc(limitName(s.volumeLimit))}</output></div></div></div>
    </div>`;
  setFill($('#volLimit'));
}
async function putSound(body, { quiet = false } = {}) {
  try {
    S.sound = await api('PUT', '/api/sound', { output: soundOutput().key, ...body });
    renderSound();
    if (!quiet) toast(t('common.savedOnRondo'));
    return true;
  } catch (e) {
    if (e.status !== 401) toast(t('common.notSaved', { reason: reason(e) }), 'err');
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
    const name = await promptSheet({ title: t('sound.presetName'), text: te('sound.presetNameText'), action: t('common.save'), placeholder: t('common.name') });
    if (name) putSound({ savePreset: name, custom: soundOutput().gains });
    return;
  }
  const del = e.target.closest('#eqDelete');
  if (del) {
    if (await confirmSheet({ title: t('common.deleteName', { name: del.dataset.name }), text: te('sound.deleteText'), action: t('common.delete') })) putSound({ deletePreset: del.dataset.name });
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
    $('#volLimitOut').textContent = +e.target.value >= S.sound.maxVolume ? t('sound.noLimit') : t('common.nOfTotal', { n: +e.target.value, total: S.sound.maxVolume });
  }
});
// Headphone match: a model looked up on the server's AutoEq index, its correction kept with the output
function findMatch() {
  const o = soundOutput();
  openSheet(`<h2>${te('sound.match')}</h2><p>${te('sound.matchFor', { name: o.label })}</p>
    <form class="sheet-form" id="matchForm"><label class="search pick-search">${icon('search')}<input type="search" placeholder="${te('sound.matchPlaceholder')}" value="${esc(o.key.startsWith('bt:') ? o.label : '')}" autocomplete="off" aria-label="${te('sound.model')}"></label></form>
    <div class="match-list" id="matchList"></div><div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('common.close')}</button></div>`);
  const input = $('input', sheet), list = $('#matchList', sheet);
  let asked = 0;
  const search = async () => {
    const q = input.value.trim();
    if (!q) return;
    const mine = ++asked;
    list.innerHTML = `<div class="row-empty">${te('sound.looking')}</div>`;
    try {
      const r = await api('GET', `/api/sound/match?q=${encodeURIComponent(q)}`, undefined, { timeout: 60000 });
      if (mine !== asked) return;
      const items = r.items || [];
      list.innerHTML = items.length ? items.map((it) => `<button type="button" data-id="${esc(it.id)}" data-name="${esc(it.name)}" data-source="${esc(it.source)}"><div class="row-title">${esc(it.name)}</div><div class="row-sub">${te('sound.measuredBy', { source: it.source })}</div></button>`).join('') : `<div class="row-empty">${te('sound.noModel')}</div>`;
    } catch (e) {
      if (mine === asked) list.innerHTML = `<div class="row-empty">${esc(e.status === 404 ? t('sound.serverOld') : reason(e))}</div>`;
    }
  };
  $('#matchForm', sheet).addEventListener('submit', (e) => { e.preventDefault(); search(); });
  $('[data-r="0"]', sheet).addEventListener('click', () => sheet.close());
  list.addEventListener('click', async (e) => {
    const b = e.target.closest('[data-id]');
    if (!b) return;
    list.innerHTML = `<div class="row-empty">${te('sound.getting', { name: b.dataset.name })}</div>`;
    try {
      S.sound = await api('POST', '/api/sound/match', { output: o.key, id: b.dataset.id, name: b.dataset.name, source: b.dataset.source }, { timeout: 60000 });
      sheet.close();
      renderSound();
      toast(t('sound.inUse', { name: b.dataset.name, output: o.label }));
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
  ['title', t('library.sort.title'), 'text'], ['artist', t('library.sort.artist'), 'text'], ['album', t('library.sort.album'), 'text'], ['albumArtist', t('library.sort.albumArtist'), 'text'],
  ['genre', t('library.sort.genre'), 'text'], ['folder', t('smart.field.folder'), 'text'], ['year', t('library.sort.year'), 'number'], ['rating', t('smart.field.rating'), 'number'],
  ['plays', t('smart.field.plays'), 'number'], ['length', t('smart.field.length'), 'number'], ['added', t('smart.field.added'), 'date'], ['lastPlayed', t('smart.field.lastPlayed'), 'date'],
];
const SM_OPS = {
  text: [['contains', t('smart.op.contains')], ['is', t('smart.op.is')], ['isNot', t('smart.op.isNot')], ['notContains', t('smart.op.notContains')], ['startsWith', t('smart.op.startsWith')]],
  number: [['atLeast', t('smart.op.atLeast')], ['atMost', t('smart.op.atMost')], ['is', t('smart.op.is')], ['isNot', t('smart.op.isNot')]],
  date: [['inLast', t('smart.op.inLast')], ['notInLast', t('smart.op.notInLast')]],
};
const SM_SORTS = [['artist', t('library.sort.artist')], ['album', t('library.sort.album')], ['title', t('library.sort.title')], ['random', t('smart.sort.random')], ['added', t('library.sort.added')],
  ['lastPlayed', t('library.sort.played')], ['plays', t('library.sort.plays')], ['rating', t('smart.sort.rating')], ['year', t('smart.sort.year')]];
const smKind = (field) => (SM_FIELDS.find((x) => x[0] === field) || SM_FIELDS[0])[2];
// "genre contains “Pop”": the field's name without its unit, in lower case
function smRuleText(r) {
  const field = (SM_FIELDS.find((x) => x[0] === r.field) || [r.field, r.field])[1].replace(/ \(.*\)/, '').toLocaleLowerCase(L.code);
  const op = ((SM_OPS[smKind(r.field)] || []).find((x) => x[0] === r.op) || [r.op, r.op])[1];
  const value = smKind(r.field) === 'date' ? t('smart.days', { count: r.value }) : smKind(r.field) === 'text' ? `“${r.value}”` : String(r.value);
  return t('smart.rule', { field, op, value });
}
const smSub = (p) => `${t('common.songs', { count: p.count })}${p.durationSec ? ` · ${fmtDur(p.durationSec)}` : ''} · ${p.rules.map(smRuleText).join(p.match === 'any' ? t('smart.or') : ', ')}`;
function smartListHtml(q) {
  const all = S.smart || [];
  const shown = q ? all.filter((p) => q.split(/\s+/).every((w) => fold(p.title || p.name).includes(w))) : all;
  return `<div class="lhdr" role="heading" aria-level="3">${te('smart.title')}</div>
    <div class="song pl-row pl-new" role="listitem" data-act="sm-new"><span class="art art-add">${icon('plus')}</span><span class="s-main"><span class="s-title">${te('smart.new')}</span><span class="s-sub">${te('smart.newSub')}</span></span></div>` +
    shown.map((p) => `<div class="song pl-row" role="listitem" data-sm="${esc(p.name)}"><span class="art">${icon('bolt')}</span>
      <span class="s-main"><span class="s-title">${esc(p.name)}</span><span class="s-sub">${esc(smSub(p))}</span></span>
      <span class="s-acts">${p.count ? `<button class="ib ib-sm" type="button" data-act="sm-play" aria-label="${te('common.playNameOnRondo', { name: p.name })}">${icon('play')}</button>` : ''}<button class="ib ib-sm" type="button" data-act="sm-edit" aria-label="${te('smart.editName', { name: p.name })}">${icon('gear')}</button></span></div>`).join('');
}
async function renderSmart() {
  const name = S.openSmart;
  const p = (S.smart || []).find((x) => x.name === name);
  if (!p) { S.openSmart = null; return renderPlaylists(); }
  $('#libSummary').textContent = t('smart.summary');
  libList.innerHTML = '<div class="lib-skel"></div>'.repeat(4);
  let songs;
  try { songs = await api('GET', `/api/smart/songs?name=${encodeURIComponent(name)}`); } catch (e) {
    if (e.status !== 401) libList.innerHTML = `<div class="lib-empty"><h3>${te('smart.couldntRead')}</h3><p>${esc(reason(e))}</p></div>`;
    return;
  }
  if (S.openSmart !== name) return;
  S.smartPaths = songs.paths;
  const head = `<div class="pl-head"><button class="btn btn-sm" type="button" data-act="sm-back">${icon('chevron', 'i flip')}<span>${te('common.playlists')}</span></button>
      <h2>${esc(p.name)}</h2><p>${esc(smSub(songs))}</p>
      <div class="pl-acts">${songs.count ? `<button class="btn btn-primary btn-sm" type="button" data-act="sm-play">${icon('play')}<span>${te('common.playOnRondo')}</span></button>` : ''}
        <button class="btn btn-sm" type="button" data-act="sm-edit">${icon('gear')}<span>${te('smart.editRules')}</span></button>
        <button class="btn btn-sm btn-red-text" type="button" data-act="sm-delete">${icon('trash')}<span>${te('common.delete')}</span></button></div></div>`;
  const rows = songs.paths.map((path) => {
    const s = S.libByPath?.get(path);
    if (!s) return '';
    return `<div class="song${isPlaying(s) ? ' is-playing' : ''}" role="listitem" data-smpath="${esc(path)}">
      <span class="art"${s.art ? ` data-art="${esc(path)}"` : ''}>${icon('note')}</span>
      <span class="s-main"><span class="s-title">${esc(s.title || fileName(path))}</span><span class="s-sub">${esc([s.artist, s.album].filter(Boolean).join(' · '))}</span></span>
      <span class="s-time">${fmtDur(s.durationSec)}</span></div>`;
  }).join('');
  libList.innerHTML = head + (rows || `<div class="lib-empty"><h3>${te('smart.noMatch')}</h3><p>${te('smart.noMatchText')}</p></div>`);
  $$('.art[data-art]', libList).forEach((el) => artIO.observe(el));
}
async function playSmart(p, from) {
  try {
    const songs = from && S.smartPaths ? { paths: S.smartPaths } : await api('GET', `/api/smart/songs?name=${encodeURIComponent(p.name)}`);
    if (!songs.paths.length) return;
    const pl = await api('POST', '/api/player/play', { path: from || songs.paths[0], queue: songs.paths });
    if (S.status) { S.status.player = pl; S.statusAt = performance.now(); }
    renderMini(); markPlayingRows();
    toast(t('common.playingOnRondo', { name: p.title || p.name }));
  } catch (e) { if (e.status !== 401) toast(t('common.couldntPlayIt', { reason: reason(e) }), 'err'); }
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
    if (!(await confirmSheet({ title: t('common.deleteName', { name: p.name }), text: te('smart.deleteText'), action: t('common.delete') }))) return true;
    try {
      S.smart = (await api('DELETE', `/api/smart?name=${encodeURIComponent(p.name)}`)).playlists;
      S.openSmart = null;
      renderPlaylists();
      toast(t('common.deletedName', { name: p.name }));
    } catch (e2) { if (e2.status !== 401) toast(t('common.couldntDeleteIt', { reason: reason(e2) }), 'err'); }
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
    const value = kind === 'text' ? `<input type="text" data-k="value" value="${esc(r.value ?? '')}" placeholder="${r.field === 'genre' ? 'Pop' : r.field === 'folder' ? 'Music/Jazz' : ''}" aria-label="${te('smart.value')}">`
      : `<input type="number" data-k="value" value="${esc(r.value ?? '')}" min="${kind === 'date' ? 1 : 0}" step="1" aria-label="${kind === 'date' ? te('smart.daysLabel') : te('smart.number')}">${kind === 'date' ? `<span class="sm-unit">${te('smart.daysUnit')}</span>` : ''}`;
    return `<div class="sm-rule" data-i="${i}"><div class="select"><select data-k="field" aria-label="${te('smart.field')}">${opts(SM_FIELDS.map((x) => [x[0], x[1]]), r.field)}</select>${icon('down')}</div>
      <div class="select"><select data-k="op" aria-label="${te('smart.condition')}">${opts(SM_OPS[kind], r.op)}</select>${icon('down')}</div>${value}
      <button class="ib ib-sm" type="button" data-rm="${i}" aria-label="${te('smart.removeRule')}"${draft.rules.length > 1 ? '' : ' disabled'}>${icon('close')}</button></div>`;
  };
  const matchSel = () => `<div class="select"><select id="smMatch" aria-label="${te('smart.matchLabel')}">${opts([['all', t('smart.all')], ['any', t('smart.any')]], draft.match)}</select>${icon('down')}</div>`;
  const sortSel = () => `<div class="select"><select id="smSort" aria-label="${te('smart.order')}">${opts(SM_SORTS, draft.sort)}</select>${icon('down')}</div>`;
  const limitIn = () => `<input type="number" id="smLimit" min="0" max="5000" step="1" value="${draft.limit || ''}" placeholder="${te('smart.anyNumber')}" aria-label="${te('smart.limitLabel')}">`;
  const sheetHtml = () => `<h2>${p ? te('smart.edit') : te('smart.new')}</h2>
    <form class="sheet-form sm-form" novalidate>
      <label class="field-label" for="smName">${te('common.name')}</label>
      <div class="field-row"><input type="text" id="smName" maxlength="60" value="${esc(draft.name)}" placeholder="${te('smart.namePlaceholder')}" autocomplete="off"></div>
      <div class="sm-match">${th('smart.matchLine', { select: html(matchSel()) })}</div>
      <div id="smRules">${draft.rules.map(ruleHtml).join('')}</div>
      <button class="btn btn-sm" type="button" id="smAdd"${draft.rules.length >= 12 ? ' disabled' : ''}>${icon('plus')}<span>${te('smart.addRule')}</span></button>
      <div class="sm-match">${th('smart.orderLine', { sort: html(sortSel()), limit: html(limitIn()) })}</div>
      <p class="sm-preview" id="smPreview" aria-live="polite"></p>
      <p class="field-err" id="smErr" hidden></p>
      <div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('common.cancel')}</button><button class="btn btn-primary" type="submit">${te('common.save')}</button></div>
    </form>`;
  openSheet(sheetHtml());
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
        if (mine === asked) out.textContent = r.count ? (r.durationSec ? t('smart.previewLength', { count: r.count, length: fmtDur(r.durationSec) }) : t('smart.preview', { count: r.count })) : t('smart.previewNone');
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
    if (!draft.name) { err.textContent = t('smart.typeName'); err.hidden = false; return; }
    try {
      S.smart = (await api('PUT', '/api/smart', { playlist: draft, oldName: p ? p.name : undefined })).playlists;
      sheet.close();
      // the name as the card keeps it (FAT: no : / \ * ? " < > |)
      if (S.openSmart) S.openSmart = draft.name.replace(/[\\/:*?"<>|]/g, ' ').replace(/\s+/g, ' ').trim();
      renderPlaylists();
      toast(t('smart.saved', { name: draft.name }));
    } catch (e2) { if (e2.status !== 401) { err.textContent = e2.code && e2.code !== 'pair' ? e2.code : reason(e2); err.hidden = false; } }
  });
  $('#smName', sheet).focus();
  read();
  preview();
}

// ---------- Organize the card (Library › Organize the card on the Rondo) ----------
// Every song into Music/Artist/Album (Year)/, every video into Videos/Shows/ or Videos/Movies/, as
// Jellyfin expects: first what would move, then the moves only when asked. Playlists, plays and ratings
// go along. A series' years and a movie's year, when the names don't say them, are looked up online
// (TVmaze, Wikidata) and picked here; the folders keep them from then on.
$('#organizeBtn').addEventListener('click', openOrganize);
const ORG = { plan: null, lookup: null, pick: {} };  // pick: a title's key -> the match taken (-1: as it is)
async function openOrganize() {
  openSheet(`<h2>${te('organize.title')}</h2><p>${te('organize.working')}</p>`);
  let plan;
  try { plan = await api('GET', '/api/organize', undefined, { timeout: 60000 }); } catch (e) {
    if (e.status !== 401) openSheet(`<h2>${te('organize.title')}</h2><p>${esc(reason(e))}</p><div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('common.close')}</button></div>`);
    $('[data-r="0"]', sheet)?.addEventListener('click', () => sheet.close());
    return;
  }
  if (plan.status.running) return organizeProgress();
  ORG.plan = plan, ORG.lookup = plan.lookup, ORG.pick = {};
  for (const m of plan.lookup?.matches || []) ORG.pick[m.title.key] = m.pick;
  renderOrganize();
  if (plan.lookup?.running) followLookup();
  else if (Object.keys(orgNames()).length) replan();  // a lookup made earlier: its picks in the plan
}
// what's been picked, as POST /api/organize takes it
function orgNames() {
  const names = {};
  for (const m of ORG.lookup?.matches || []) {
    const f = m.found[ORG.pick[m.title.key] ?? -1];
    if (f) names[m.title.key] = { name: f.name, years: f.years };
  }
  return names;
}
const titleWithYears = (name, years) => (years ? `${name} (${years})` : name);
function orgTitlesHtml() {
  const titles = ORG.plan.titles || [], lk = ORG.lookup;
  if (!titles.length) return '';
  const keys = new Set(titles.map((x) => x.key));
  const matches = (lk?.matches || []).filter((m) => keys.has(m.title.key));
  const shows = titles.filter((x) => x.series).length, movies = titles.length - shows;
  const parts = [shows ? t('organize.shows', { count: shows }) : '', movies ? t('organize.movies', { count: movies }) : ''].filter(Boolean);
  const what = parts.length > 1 ? t('organize.and', { a: parts[0], b: parts[1] }) : parts[0] || '';
  let body;
  if (lk?.running) {
    body = `<div class="meter"><span style="--p:${lk.total ? (lk.done / lk.total) * 100 : 0}%"></span></div><p class="row-sub">${te('organize.lookingUp', { n: String(lk.done + 1 > lk.total ? lk.total : lk.done + 1), total: String(lk.total) })}</p>`;
  } else if (!matches.length) {
    body = `<p class="row-sub">${te('organize.yearsFor', { what })}</p>
      <button class="btn" type="button" data-r="lookup">${icon('globe')}<span>${titles.length === 1 ? te('organize.lookUpIts') : te('organize.lookUpTheir')}</span></button>
      <p class="row-sub org-src">${te('organize.source')}</p>`;
  } else {
    body = `<div class="org-list org-titles">${matches.map((m) => {
      const ti = m.title, pick = ORG.pick[ti.key] ?? -1;
      const opts = m.found.map((f, i) => `<option value="${i}"${i === pick ? ' selected' : ''}>${esc(titleWithYears(f.name, f.years))}${f.detail ? ` · ${esc(f.detail)}` : ''}</option>`).join('');
      const none = m.failed ? te('organize.couldntLookUp') : m.found.length ? te('organize.leave') : te('organize.notFound');
      return `<div class="org-move"><span class="tag">${ti.series ? te('organize.show') : te('organize.movie')}</span><div class="org-pick">
        <div class="org-to">${esc(titleWithYears(ti.name, ti.years))}${ti.series ? ` <span class="org-sub">${te('organize.episodes', { count: ti.files })}</span>` : ''}</div>
        <label class="select"><span class="sr-only">${te('organize.nameIt')}</span><select data-key="${esc(ti.key)}">${opts}<option value="-1"${pick < 0 ? ' selected' : ''}>${none}</option></select>${icon('down')}</label></div></div>`;
    }).join('')}</div>`;
  }
  return `<h3 class="org-h">${te('organize.showsMovies')}</h3>${body}`;
}
function orgMovesHtml() {
  const plan = ORG.plan, n = plan.moves.length;
  const kindName = { song: t('organize.kind.song'), video: t('organize.kind.video'), lyrics: t('organize.kind.lyrics'), subtitles: t('organize.kind.subtitles'), cover: t('organize.kind.cover') };
  const shown = plan.moves.slice(0, 400);
  // the folders as Organize writes them on the card: their names stay as they are
  const facts = [[t('library.songs'), plan.songs, 'Music/Artist/Album (Year)/01 - Title'], [t('library.videos'), plan.videos, t('organize.videoFolders', { shows: 'Videos/Shows/Series (Years)/Season 01', movies: 'Videos/Movies/Title (Year)' })],
    [t('organize.sidecars'), plan.sidecars, t('organize.sidecarsWhere')], [t('organize.covers'), plan.covers, t('organize.coversWhere')], [t('organize.already'), plan.alreadyThere, '']]
    .filter((x) => x[1]);
  return `<dl class="facts">${facts.map(([k, v, sub]) => `<dt>${esc(k)}</dt><dd>${v}${sub ? ` <span class="org-sub">${esc(sub)}</span>` : ''}</dd>`).join('')}</dl>
    ${n ? `<div class="org-list" role="list" aria-label="${te('organize.moves')}">${shown.map((m) => `<div class="org-move" role="listitem"><span class="tag">${esc(kindName[m.kind] || m.kind)}</span>
      <div><div class="org-to">${esc(m.to)}</div><div class="org-from">${te('organize.from', { path: m.from })}</div></div></div>`).join('')}
      ${n > shown.length ? `<div class="row-empty">${te('organize.andMore', { count: n - shown.length })}</div>` : ''}</div>` : ''}`;
}
function renderOrganize() {
  const n = ORG.plan.moves.length;
  openSheet(`<h2>${te('organize.title')}</h2>
    <p>${n ? te('organize.intro') : te('organize.inPlace')}</p>
    <div id="orgTitles">${orgTitlesHtml()}</div>
    <div id="orgMoves">${orgMovesHtml()}</div>
    <div class="sheet-actions"><button class="btn" type="button" data-r="0">${n ? te('common.cancel') : te('common.close')}</button><button class="btn btn-primary" type="button" data-r="go"${n ? '' : ' hidden'}>${te('organize.go', { count: n })}</button></div>`);
  $('[data-r="0"]', sheet).addEventListener('click', () => sheet.close());
  wireOrgTitles();
  $('[data-r="go"]', sheet).addEventListener('click', async (e) => {
    const b = e.target.closest('button');
    b.disabled = true;
    try { await api('POST', '/api/organize', { names: orgNames() }); organizeProgress(); } catch (err) {
      if (err.status !== 401) toast(t('common.didntStart', { reason: err.code && err.code !== 'pair' ? err.code : reason(err) }), 'err');
      b.disabled = false;
    }
  });
}
function wireOrgTitles() {
  $('[data-r="lookup"]', sheet)?.addEventListener('click', async (e) => {
    e.target.closest('button').disabled = true;
    try { ORG.lookup = await api('POST', '/api/organize/lookup'); } catch (err) {
      if (err.status !== 401) toast(t('organize.didntLookUp', { reason: err.code && err.code !== 'pair' ? err.code : reason(err) }), 'err');
      e.target.closest('button').disabled = false;
      return;
    }
    refreshOrgTitles();
    followLookup();
  });
  for (const sel of sheet.querySelectorAll('#orgTitles select')) sel.addEventListener('change', () => { ORG.pick[sel.dataset.key] = Number(sel.value); replan(); });
}
function refreshOrgTitles() {
  const box = document.getElementById('orgTitles');
  if (!box) return false;
  box.innerHTML = orgTitlesHtml();
  wireOrgTitles();
  return true;
}
async function followLookup() {
  for (;;) {
    await new Promise((r) => setTimeout(r, 700));
    if (!document.getElementById('orgTitles')) return;  // the sheet was closed
    try { ORG.lookup = await api('GET', '/api/organize/lookup'); } catch { continue; }
    if (!ORG.lookup.running) {
      ORG.pick = {};
      for (const m of ORG.lookup.matches) ORG.pick[m.title.key] = m.pick;
      refreshOrgTitles();
      return replan();
    }
    refreshOrgTitles();
  }
}
// the plan again with the names picked (what moves where changes with them)
let replanning = 0;
async function replan() {
  const mine = ++replanning;
  try {
    const plan = await api('POST', '/api/organize/plan', { names: orgNames() }, { timeout: 60000 });
    if (mine !== replanning || !document.getElementById('orgMoves')) return;
    ORG.plan.moves = plan.moves, ORG.plan.songs = plan.songs, ORG.plan.videos = plan.videos;
    ORG.plan.sidecars = plan.sidecars, ORG.plan.covers = plan.covers, ORG.plan.alreadyThere = plan.alreadyThere;
    document.getElementById('orgMoves').innerHTML = orgMovesHtml();
    const go = $('[data-r="go"]', sheet), n = plan.moves.length;
    go.hidden = !n;
    go.textContent = t('organize.go', { count: n });
    $('[data-r="0"]', sheet).textContent = n ? t('common.cancel') : t('common.close');
  } catch (err) {
    if (err.status !== 401) toast(t('organize.couldntReplan', { reason: reason(err) }), 'err');
  }
}
async function organizeProgress() {
  openSheet(`<h2>${te('organize.organizing')}</h2><div class="meter"><span id="orgBar" style="--p:0%"></span></div><p id="orgNote">${te('common.starting')}</p>`);
  for (;;) {
    let st;
    try { st = await api('GET', '/api/organize/status'); } catch { await new Promise((r) => setTimeout(r, 1500)); continue; }
    const bar = document.getElementById('orgBar'), note = document.getElementById('orgNote');
    if (!bar) return;  // the sheet was closed
    bar.style.setProperty('--p', `${st.total ? (st.done / st.total) * 100 : 0}%`);
    note.textContent = st.running ? t('organize.moving', { n: String(st.done), total: String(st.total) }) : '';
    if (!st.running) {
      const result = st.errors.length ? te('organize.resultFailed', { moved: t('organize.filesMoved', { count: st.moved }), failed: t('organize.filesFailed', { count: st.errors.length }) }) : te('organize.result', { count: st.moved });
      openSheet(`<h2>${te('organize.done')}</h2><p>${result}</p>
        ${st.errors.length ? `<div class="org-list">${st.errors.slice(0, 100).map((x) => `<div class="org-move"><div class="org-from">${esc(x)}</div></div>`).join('')}</div>` : ''}
        <div class="sheet-actions"><button class="btn btn-primary" type="button" data-r="0">${te('common.done')}</button></div>`);
      $('[data-r="0"]', sheet).addEventListener('click', () => sheet.close());
      setTimeout(() => loadLibrary(true), 1500);  // the new paths
      return;
    }
    await new Promise((r) => setTimeout(r, 700));
  }
}

// ---------- Fill in: what the songs' tags leave out (the dashboard only) ----------
// A song's missing genre, year, album and track number, looked up album by album (the iTunes Store,
// then MusicBrainz), shown for review and then saved into the files; lyrics from LRCLIB for every song
// without any, saved into the file. It all runs on the Rondo: this page can close and come back.
$('#fillBtn').addEventListener('click', openFillIn);
const FILL = { skip: new Set(), proposals: [] };
const fillClose = () => { $('[data-r="0"]', sheet)?.addEventListener('click', () => sheet.close()); };
// the iTunes Stores of this browser's languages' regions ("en-GB" -> "gb"), the songs' own and the US one besides
const storeCountries = () => [...new Set((navigator.languages || [navigator.language]).map((l) => (l.split('-')[1] || '').toLowerCase()).filter((c) => /^[a-z]{2}$/.test(c)))];
async function openFillIn() {
  openSheet(`<h2>${te('fill.title')}</h2><p>${te('fill.looking')}</p>`);
  let r;
  try { r = await api('GET', '/api/fill', undefined, { timeout: 30000 }); } catch (e) {
    if (e.status !== 401) openSheet(`<h2>${te('fill.title')}</h2><p>${esc(reason(e))}</p><div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('common.close')}</button></div>`);
    return fillClose();
  }
  const st = r.status;
  if (st.running) return fillProgress();
  const waiting = st.kind === 'details' && st.finished && st.proposed;  // found, not saved yet
  const m = r.summary, lacking = [[t('library.noGenre'), m.noGenre], [t('library.noYear'), m.noYear], [t('fill.noAlbum'), m.noAlbum], [t('fill.noTrack'), m.noTrack]].filter((x) => x[1]);
  const minutes = Math.max(1, Math.round((m.albums * 3) / 60));  // about 3 s an album on the Y1 (the services allow a request a second)
  openSheet(`<h2>${te('fill.title')}</h2>
    <p>${te('fill.intro')}</p>
    <section class="fill-sec">
      <h3>${icon('tag')}<span>${te('fill.details')}</span></h3>
      ${lacking.length ? `<dl class="facts">${lacking.map(([k, v]) => `<dt>${esc(k)}</dt><dd>${te('common.songs', { count: v })}</dd>`).join('')}</dl>
      ${waiting ? `<p class="row-sub">${te('fill.foundNotSaved', { count: st.proposed })}</p>
      <div class="fill-acts"><button class="btn btn-primary" type="button" data-r="review">${icon('check')}<span>${te('fill.review')}</span></button>
        <button class="btn" type="button" data-r="details">${icon('search')}<span>${te('fill.lookUpAgain')}</span></button></div>`
      : `<p class="row-sub">${te('fill.asked', { albums: t('common.albums', { count: m.albums }), minutes: t('common.minutes', { count: minutes }) })}</p>
      <button class="btn btn-primary" type="button" data-r="details">${icon('search')}<span>${te('fill.lookUpDetails')}</span></button>`}`
      : `<p class="row-sub">${te('fill.complete')}</p>`}
    </section>
    <section class="fill-sec">
      <h3>${icon('lyrics')}<span>${te('fill.lyrics')}</span></h3>
      <p class="row-sub">${te('fill.lyricsText')}</p>
      <button class="btn" type="button" data-r="lyrics"${m.songs ? '' : ' disabled'}>${icon('search')}<span>${te('fill.lookUpLyrics')}</span></button>
    </section>
    ${m.unsupported ? `<div class="note info">${icon('info')}<span>${te('fill.unsupported', { count: m.unsupported })}</span></div>` : ''}
    <div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('common.close')}</button></div>`);
  fillClose();
  const start = async (btn, path, body) => {
    btn.disabled = true;
    try { await api('POST', path, body); fillProgress(); } catch (err) {
      if (err.status !== 401) toast(t('common.didntStart', { reason: err.code && err.code !== 'pair' ? err.code : reason(err) }), 'err');
      btn.disabled = false;
    }
  };
  $('[data-r="review"]', sheet)?.addEventListener('click', fillReview);
  $('[data-r="details"]', sheet)?.addEventListener('click', (e) => start(e.target.closest('button'), '/api/fill/details', { countries: storeCountries() }));
  $('[data-r="lyrics"]', sheet)?.addEventListener('click', (e) => start(e.target.closest('button'), '/api/fill/lyrics'));
}
async function fillProgress() {
  const titles = { details: t('fill.lookingDetails'), lyrics: t('fill.lookingLyrics'), save: t('fill.saving') };
  openSheet(`<h2 id="fillH">${te('fill.working')}</h2><div class="meter"><span id="fillBar" style="--p:0%"></span></div><p id="fillNote">${te('common.starting')}</p>
    <p class="row-sub">${te('fill.runs')}</p>
    <div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('common.hide')}</button><button class="btn" type="button" data-r="stop">${te('common.stop')}</button></div>`);
  fillClose();
  $('[data-r="stop"]', sheet).addEventListener('click', async (e) => {
    e.target.closest('button').disabled = true;
    try { await api('POST', '/api/fill/cancel'); } catch { /* the next status says */ }
  });
  for (;;) {
    let st;
    try { st = await api('GET', '/api/fill/status'); } catch { await new Promise((r) => setTimeout(r, 1500)); continue; }
    const bar = document.getElementById('fillBar'), note = document.getElementById('fillNote');
    if (!bar) return;  // the sheet was closed: the job goes on
    document.getElementById('fillH').textContent = titles[st.kind] || t('fill.working');
    bar.style.setProperty('--p', `${st.total ? (st.done / st.total) * 100 : 0}%`);
    const at = String(Math.min(st.done + 1, st.total)), total = String(st.total);
    note.textContent = st.kind === 'details' ? t('fill.progressDetails', { at, total, albums: t('common.albums', { count: st.proposed }) })
      : st.kind === 'lyrics' ? (st.synced ? t('fill.progressLyricsTimed', { at, total, songs: t('common.songs', { count: st.saved }), synced: String(st.synced) }) : t('fill.progressLyrics', { at, total, songs: t('common.songs', { count: st.saved }) }))
      : t('fill.progressSave', { at, total });
    if (!st.running) return st.kind === 'details' && st.proposed ? fillReview() : fillDone(st);
    await new Promise((r) => setTimeout(r, 1000));
  }
}
async function fillReview() {
  let st;
  try { st = await api('GET', '/api/fill/status?proposals=1', undefined, { timeout: 30000 }); } catch (e) {
    if (e.status !== 401) toast(reason(e), 'err');
    return;
  }
  FILL.proposals = st.proposals || [];
  FILL.skip = new Set();
  const songs = () => FILL.proposals.filter((p) => !FILL.skip.has(p.id)).reduce((n, p) => n + p.songs, 0);
  const shown = FILL.proposals.slice(0, 600);
  const left = [st.notFound ? t('fill.albumsNotFound', { count: st.notFound }) : '', st.failed ? t('fill.albumsFailed', { count: st.failed }) : '',
    st.canceled ? t('fill.lookupStopped') : ''].filter(Boolean);
  const same = (a, b) => a.toLowerCase().replace(/ - (single|ep)$/i, '') === b.toLowerCase();
  openSheet(`<h2>${te('fill.foundFor', { count: FILL.proposals.length })}</h2>
    <p>${left.length ? te('fill.reviewTextLeft', { left: left.join(', ') }) : te('fill.reviewText')}</p>
    <div class="org-list fill-list" role="list">${shown.map((p) => {
      const what = [p.genre ? th('fill.genre', { genre: p.genre }) : '', p.year ? th('fill.year', { year: String(p.year) }) : '', p.newAlbum ? th('fill.album', { album: p.newAlbum }) : '',
        p.tracks ? te('fill.trackNumbers', { count: p.tracks }) : ''].filter(Boolean).join(' · ');
      const from = !same(p.foundAlbum, p.album || p.foundAlbum) || !p.foundArtist.toLowerCase().includes(p.artist.toLowerCase().split(/[,&]/)[0].trim())
        ? ` <span class="org-sub">${te('fill.foundAs', { album: p.foundAlbum, artist: p.foundArtist })}</span>` : '';
      return `<label class="org-move fill-row" role="listitem"><input type="checkbox" data-id="${p.id}" checked>
        <div><div class="org-to">${esc(p.artist)} — ${esc(p.album || p.foundAlbum)}</div><div class="org-from">${what}</div>
        <div class="org-from">${te('common.songs', { count: p.songs })} · ${esc(p.source)}${from}</div></div></label>`;
    }).join('')}${FILL.proposals.length > shown.length ? `<div class="row-empty">${te('fill.andMoreSaved', { count: FILL.proposals.length - shown.length })}</div>` : ''}</div>
    <div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('fill.notNow')}</button><button class="btn btn-primary" type="button" data-r="save">${te('fill.saveInto', { count: songs() })}</button></div>`);
  fillClose();
  const save = $('[data-r="save"]', sheet);
  for (const box of sheet.querySelectorAll('.fill-row input')) box.addEventListener('change', () => {
    const id = Number(box.dataset.id);
    if (box.checked) FILL.skip.delete(id); else FILL.skip.add(id);
    const n = songs();
    save.textContent = t('fill.saveInto', { count: n });
    save.disabled = !n;
  });
  save.addEventListener('click', async () => {
    save.disabled = true;
    try { await api('POST', '/api/fill/save', { skip: [...FILL.skip] }); fillProgress(); } catch (err) {
      if (err.status !== 401) toast(t('fill.didntSave', { reason: err.code && err.code !== 'pair' ? err.code : reason(err) }), 'err');
      save.disabled = false;
    }
  });
}
function fillDone(st) {
  let title, text;
  if (st.kind === 'lyrics') {
    title = st.saved ? t('fill.lyricsFor', { count: st.saved }) : t('fill.noLyrics');
    text = [st.synced ? t('fill.timed', { count: String(st.synced) }) : '', st.beside ? t('fill.beside', { count: String(st.beside) }) : '', st.had ? t('fill.had', { count: st.had }) : '',
      st.notFound ? t('fill.lrclibMissing', { count: st.notFound }) : '', st.instrumental ? t('fill.instrumental', { count: st.instrumental }) : '',
      st.failed ? t('fill.songsFailed', { count: st.failed }) : ''].filter(Boolean).join('; ');
  } else if (st.kind === 'save') {
    title = t('fill.savedInto', { count: st.saved });
    text = st.errors.length ? t('fill.saveErrors', { count: st.errors.length }) : t('fill.newDetails');
  } else {
    title = t('fill.nothingFound');
    text = [st.notFound ? t('fill.albumsNotFound', { count: st.notFound }) : '', st.failed ? t('fill.albumsFailed', { count: st.failed }) : ''].filter(Boolean).join('; ');
  }
  if (st.canceled) text = t('fill.stopped', { text });
  openSheet(`<h2>${esc(title)}</h2>${st.stopped ? `<div class="note err">${icon('alert')}<span>${esc(st.stopped)}.</span></div>` : ''}<p>${esc(text)}${text && !/[.:]$/.test(text) ? '.' : ''}</p>
    ${st.errors.length ? `<div class="org-list">${st.errors.slice(0, 100).map((x) => `<div class="org-move"><div class="org-from">${esc(x)}</div></div>`).join('')}</div>` : ''}
    <div class="sheet-actions"><button class="btn btn-primary" type="button" data-r="0">${te('common.done')}</button></div>`);
  fillClose();
  if (st.saved) setTimeout(() => loadLibrary(true), 800);  // the new genres and years
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
    if (e.status !== 401) libList.innerHTML = `<div class="lib-empty"><h3>${te('files.couldntOpen')}</h3><p>${esc(reason(e))}</p></div>`;
    return;
  }
  renderLibrary();
}
const relName = (p) => { for (const r of S.files?.roots || [{ path: '/storage/sdcard0', name: t('files.card') }, { path: '/storage/sdcard1', name: t('files.internal') }]) if (p === r.path) return r.name; else if (p.startsWith(r.path + '/')) return `${r.name}/${p.slice(r.path.length + 1)}`; return p; };
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
  crumbs += `<button class="link" type="button" data-dir="${esc(at)}">${esc(root ? root.name : t('files.card'))}</button>`;
  let acc = at;
  for (const part of F.path.slice(at.length).split('/').filter(Boolean)) { acc += '/' + part; crumbs += ` › <button class="link" type="button" data-dir="${esc(acc)}">${esc(part)}</button>`; }
  const one = sel.length === 1 ? F.entries.find((e) => e.name === sel[0]) : null;
  $('#libSummary').textContent = t('files.summary', { count: F.entries.length, folder: relName(F.path) });
  libList.innerHTML = `<div class="files-head">
      <div class="crumbs">${crumbs}${F.roots.length > 1 ? `<span class="crumb-roots">${F.roots.filter((r) => r.path !== at).map((r) => `<button class="btn btn-sm btn-quiet" type="button" data-dir="${esc(r.path)}">${esc(r.name)}</button>`).join('')}</span>` : ''}</div>
      <div class="files-acts">${sel.length ? `<span class="sel-count">${te('files.selected', { count: String(sel.length) })}</span>
          ${one ? `<button class="btn btn-sm" type="button" data-fa="rename">${te('common.rename')}</button>` : ''}
          ${one && one.kind !== 'folder' ? `<button class="btn btn-sm" type="button" data-fa="download">${te('common.download')}</button>` : ''}
          <button class="btn btn-sm" type="button" data-fa="move">${te('files.move')}</button>
          <button class="btn btn-sm btn-red-text" type="button" data-fa="delete">${icon('trash')}<span>${te('common.delete')}</span></button>
          <button class="btn btn-sm btn-quiet" type="button" data-fa="none">${te('files.clear')}</button>`
        : `<button class="btn btn-sm" type="button" data-fa="folder">${icon('plus')}<span>${te('files.newFolder')}</span></button>
          <button class="btn btn-sm" type="button" data-fa="upload">${icon('upload')}<span>${te('files.uploadHere')}</span></button>`}</div></div>
    ${F.parent ? `<div class="song file-row" role="listitem" data-dir="${esc(F.parent)}"><span class="fsel"></span><span class="art">${icon('up')}</span><span class="s-main"><span class="s-title">${te('files.up')}</span></span></div>` : ''}
    ${list.length ? list.map((e) => `<div class="song file-row${S.fileSel.has(e.name) ? ' is-sel' : ''}" role="listitem" data-name="${esc(e.name)}" data-kind="${e.kind}">
        <label class="fsel"><input type="checkbox" ${S.fileSel.has(e.name) ? 'checked' : ''} aria-label="${te('files.selectName', { name: e.name })}"></label>
        <span class="art">${icon(FILE_ICON[e.kind] || 'info')}</span>
        <span class="s-main"><span class="s-title">${esc(e.name)}</span><span class="s-sub">${esc(e.kind === 'folder' ? t('files.folder') : [e.title && e.title !== e.name ? e.title : '', e.artist, fmtBytes(e.size)].filter(Boolean).join(' · '))}</span></span>
        <span class="s-time">${new Date(e.modified).toLocaleDateString(dateLocale(), { dateStyle: 'medium' })}</span></div>`).join('')
      : `<div class="lib-empty"><h3>${q ? te('common.noMatches') : te('files.empty')}</h3><p>${q ? te('files.noMatchText') : te('files.emptyText')}</p></div>`}`;
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
    const name = await promptSheet({ title: t('files.newFolder'), text: te('files.newFolderIn', { folder: relName(F.path) }), action: t('files.make'), placeholder: t('common.name') });
    if (!name) return;
    try { S.files = await api('POST', '/api/files/folder', { path: F.path, name }); renderFiles(); toast(t('common.made', { name })); }
    catch (e) { if (e.status !== 401) toast(t('files.notMade', { reason: e.code && e.code !== 'pair' ? e.code : reason(e) }), 'err'); }
    return;
  }
  if (act === 'rename') {
    const name = await promptSheet({ title: t('common.rename'), value: sel[0], action: t('common.rename') });
    if (!name || name === sel[0]) return;
    try { S.files = await api('POST', '/api/files/rename', { path: paths[0], name }); S.fileSel = new Set(); renderFiles(); toast(t('files.renamed')); }
    catch (e) { if (e.status !== 401) toast(t('files.notRenamed', { reason: e.code && e.code !== 'pair' ? e.code : reason(e) }), 'err'); }
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
    const one = sel.length === 1;
    const title = one ? t('common.deleteName', { name: sel[0] }) : t('files.deleteMany', { count: sel.length });
    if (!(await confirmSheet({ title, text: folders ? te('files.deleteTextFolders') : te('files.deleteText'), action: t('common.delete') }))) return;
    try {
      const r = await api('POST', '/api/files/delete', { paths }, { timeout: 120000 });
      if (r.errors.length) toast(t('files.couldntDelete', { count: r.errors.length }), 'err');
      else toast(one ? t('common.deletedName', { name: sel[0] }) : t('files.deletedMany', { count: sel.length }));
      loadFiles(F.path, true); loadLibrary(true);
    } catch (e) { if (e.status !== 401) toast(t('files.notDeleted', { reason: reason(e) }), 'err'); }
    return;
  }
  if (act === 'move') return moveSheet(paths);
}
// a folder to move into: browse the folders, then Move here
async function moveSheet(paths) {
  let dir = S.files.path;
  const draw = async () => {
    let to;
    try { to = await api('GET', `/api/files?path=${encodeURIComponent(dir)}`); } catch (e) { return; }
    const folders = to.entries.filter((e) => e.kind === 'folder').sort((a, b) => collator.compare(a.name, b.name));
    openSheet(`<h2>${te('files.moveTitle', { count: paths.length })}</h2><p>${th('files.into', { folder: relName(to.path) })}</p>
      <div class="org-list mv-list">${to.parent ? `<button type="button" class="mv-dir" data-to="${esc(to.parent)}">${icon('up')}<span>${te('files.up')}</span></button>` : ''}
        ${folders.map((e) => `<button type="button" class="mv-dir" data-to="${esc(to.path + '/' + e.name)}"${paths.includes(to.path + '/' + e.name) ? ' disabled' : ''}>${icon('folder')}<span>${esc(e.name)}</span></button>`).join('') || `<div class="row-empty">${te('files.noFolders')}</div>`}</div>
      <div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('common.cancel')}</button><button class="btn btn-primary" type="button" data-r="here"${to.path === S.files.path ? ' disabled' : ''}>${te('files.moveHere')}</button></div>`);
    $('[data-r="0"]', sheet).addEventListener('click', () => sheet.close());
    $$('.mv-dir', sheet).forEach((b) => b.addEventListener('click', () => { dir = b.dataset.to; draw(); }));
    $('[data-r="here"]', sheet).addEventListener('click', async () => {
      try {
        const r = await api('POST', '/api/files/move', { paths, to: to.path }, { timeout: 120000 });
        sheet.close();
        if (r.errors.length) toast(r.errors[0], 'err');
        else if (r.background) toast(t('files.movingBackground', { folder: relName(to.path) }));
        else toast(t('files.moved', { count: r.moved, folder: relName(to.path) }));
        if (r.background) setTimeout(() => { loadFiles(S.files.path, true); loadLibrary(true); }, 8000);
        S.fileSel = new Set();
        loadFiles(S.files.path, true); loadLibrary(true);
      } catch (e) { if (e.status !== 401) toast(t('files.notMoved', { reason: reason(e) }), 'err'); }
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
  const lib = S.library;
  if (!lib) return [];
  if (S.albumsFor === lib) return S.albums;
  const by = new Map();
  for (const s of lib.songs) {
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
  S.albumsFor = lib;
  return S.albums;
}
function renderAlbums() {
  if (!S.library) { libList.innerHTML = '<div class="lib-skel"></div>'.repeat(4); return; }
  const q = fold(S.libQuery.trim());
  const all = albumsOf().filter((a) => !q || fold(a.name + ' ' + a.artist).includes(q));
  S.cfList = all;
  S.cfAt = Math.max(0, Math.min(all.length - 1, S.cfAt || 0));
  $('#libSummary').textContent = all.length ? t('albums.summary', { count: all.length }) : t('albums.none');
  if (!all.length) { libList.innerHTML = `<div class="lib-empty"><h3>${te('albums.none')}</h3><p>${te('albums.noneText')}</p></div>`; return; }
  libList.innerHTML = `<div class="cf" id="cf" tabindex="0" aria-label="${te('albums.coverFlow')}">
      <button class="cf-nav prev" type="button" data-cf="-1" aria-label="${te('albums.prev')}">${icon('chevron', 'i flip')}</button>
      <button class="cf-nav next" type="button" data-cf="1" aria-label="${te('albums.next')}">${icon('chevron')}</button>
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
  $('#cfSub').textContent = [a.artist, a.year || '', t('common.nOfTotal', { n: String(at + 1), total: String(all.length) })].filter(Boolean).join(' · ');
  clearTimeout(S.cfSongsTimer);
  S.cfSongsTimer = setTimeout(() => {  // the songs once the glide stops
    const songs = $('#cfSongs');
    if (!songs || S.cfList[S.cfAt] !== a) return;
    songs.innerHTML = `<div class="lhdr" role="heading" aria-level="3">${esc(a.name)}<small>${te('common.songs', { count: a.songs.length })}</small></div>` +
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
    if (e.status !== 401 && !S.photos) libList.innerHTML = `<div class="lib-empty"><h3>${te('photos.couldntLook')}</h3><p>${esc(reason(e))}</p></div>`;
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
  $('#libSummary').textContent = all.length ? t('photos.summary', { count: all.length }) : t('photos.none');
  if (!shown.length) { libList.innerHTML = `<div class="lib-empty"><h3>${all.length ? te('common.noMatches') : te('photos.none')}</h3><p>${all.length ? te('photos.noMatchText') : te('photos.noneText')}</p></div>`; return; }
  const groups = [];
  for (let i = 0; i < shown.length; i++) {
    const dir = shown[i].path.slice(0, shown[i].path.lastIndexOf('/'));
    let g = groups.find((x) => x.dir === dir);
    if (!g) groups.push((g = { dir, items: [] }));
    g.items.push(i);
  }
  libList.innerHTML = groups.map((g) => `<div class="lhdr" role="heading" aria-level="3">${esc(relName(g.dir))}<small>${te('common.photos', { count: g.items.length })}</small></div>
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
    lb.setAttribute('aria-label', t('photos.photo'));
    lb.innerHTML = `<div class="lb-stage"><button class="lb-nav prev" type="button" data-lb="-1" aria-label="${te('photos.prev')}">${icon('chevron', 'i flip')}</button>
      <button class="lb-nav next" type="button" data-lb="1" aria-label="${te('photos.next')}">${icon('chevron')}</button></div>
      <div class="lb-bar"><span class="lb-name"></span><span class="lb-meta"></span>
        <button class="btn btn-sm" type="button" data-lb="show">${icon('play')}<span>${te('photos.slideshow')}</span></button>
        <a class="btn btn-sm" data-lb="save" download>${te('common.download')}</a>
        <button class="btn btn-sm" type="button" data-lb="close">${icon('close')}<span>${te('common.close')}</span></button></div>`;
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
  $('.lb-meta', lb).textContent = `${new Date(p.modified).toLocaleDateString(dateLocale(), { dateStyle: 'medium' })} · ${t('common.nOfTotal', { n: String(i + 1), total: String(all.length) })}`;
  const save = $('[data-lb="save"]', lb);
  save.href = `/api/files/download?path=${encodeURIComponent(p.path)}`;
  save.setAttribute('download', fileName(p.path));
}
let showTimer = 0;
function toggleShow() {
  const b = $('[data-lb="show"] span', lb);
  if (showTimer) { clearInterval(showTimer); showTimer = 0; b.textContent = t('photos.slideshow'); return; }
  showTimer = setInterval(() => lbGo(lb._i + 1, 1, true), 4000);
  b.textContent = t('common.stop');
}
function closeLightbox() {
  if (showTimer) toggleShow();
  lb.hidden = true;
}
