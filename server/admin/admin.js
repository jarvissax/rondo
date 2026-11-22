// Rondo server admin page (docs/dev/server-api.md). Plain ES module, no build step, no dependencies.
// Talks to /admin/api/* with the rondo_admin session cookie; any 401 shows the sign-in screen.

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
const icon = (name, cls = 'i') => `<svg class="${cls}" aria-hidden="true"><use href="#i-${name}"/></svg>`;
const MARK_INNER = '<g class="mark-ring" fill="none" stroke-width="13" stroke-linecap="round"><path d="M57.965 2.024A58 58 0 1 1 12.059-56.733" stroke="#f5f5f7"/><path d="M29-50.229A58 58 0 0 1 55.753-15.987" stroke="#ff3b4f"/></g><path d="M-9-15v30L16 0z" fill="#ff3b4f" stroke="#ff3b4f" stroke-width="3" stroke-linejoin="round"/>';
const spinner = (cls = 'spin') => `<svg class="${cls}" viewBox="-66 -66 132 132" aria-hidden="true">${MARK_INNER}</svg>`;
const markTile = '<svg class="i" aria-hidden="true"><use href="#mark"/></svg>';
const MIN_PASSWORD = 10;
const CODE_MS = 10 * 60 * 1000; // connect codes last 10 minutes

// ---------- language (lang/*.json, one file per language: docs/translating.md) ----------
// t(key, {name: …, count: n}) is a text in the page's language with its blanks filled ("count"
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

// the page's language: the one picked here before (this browser), else the browser's own when Rondo
// has it, else English; the server answers with the nearest it has
const LANG_KEY = 'rondo_admin_lang';
function wantedLang() {
  try { const v = localStorage.getItem(LANG_KEY); if (v) return v; } catch { /* private mode */ }
  return (navigator.languages || [navigator.language || 'en']).join(',');
}
function setLang(code) {
  try { localStorage.setItem(LANG_KEY, code); } catch { /* private mode */ }
  location.reload();
}
async function loadLang() {
  for (;;) {
    try {
      const res = await fetch(`/admin/lang/${encodeURIComponent(wantedLang())}.json`, { cache: 'no-cache' });
      if (res.ok) {
        Object.assign(L, await res.json());
        document.documentElement.lang = L.code;
        applyLang();
        return;
      }
    } catch { /* not reachable now */ }
    await new Promise((r) => setTimeout(r, 4000));
  }
}
await loadLang();

const S = {
  state: null,       // GET /state
  status: null,      // GET /status
  statusAt: 0,
  skew: 0,           // server clock minus ours, from the Date header
  booted: false,     // the app shell has been shown once
  online: true,
  route: 'overview',
  logs: null,
  pair: null,        // { code, expiresAt, base: device.pairedAt when the code appeared, expired }
  celebrate: false,
  codeBusy: false,
  checking: false,
  cookieBusy: false,
  rz: { updating: false, updateStart: 0, update: null, testing: false, test: null },
};
const now = () => Date.now() + S.skew;

// ---------- formatting ----------
const dfDay = new Intl.DateTimeFormat(L.code, { month: 'short', day: 'numeric' });
const dfDayYear = new Intl.DateTimeFormat(L.code, { month: 'short', day: 'numeric', year: 'numeric' });
const dfTime = new Intl.DateTimeFormat(L.code, { hour: 'numeric', minute: '2-digit' });
const dfWeekday = new Intl.DateTimeFormat(L.code, { weekday: 'long', month: 'short', day: 'numeric' });
const dfFull = new Intl.DateTimeFormat(L.code, { dateStyle: 'medium', timeStyle: 'medium' });
const day = (ms) => (new Date(ms).getFullYear() === new Date().getFullYear() ? dfDay : dfDayYear).format(ms);
const when = (ms) => t('time.dateAt', { date: day(ms), time: dfTime.format(ms) });
function ago(ms) {
  const s = Math.round((now() - ms) / 1000);
  if (s < 45) return t('time.justNow');
  const m = Math.round(s / 60);
  if (m < 60) return t('time.minutesAgo', { count: m });
  const h = Math.round(m / 60);
  if (h < 24) return t('time.hoursAgo', { count: h });
  const d = Math.round(h / 24);
  if (d === 1) return t('time.yesterday');
  if (d < 7) return t('time.daysAgo', { count: d });
  return day(ms);
}
function uptime(sec) {
  const m = Math.floor(sec / 60), h = Math.floor(m / 60), d = Math.floor(h / 24);
  if (d) return h % 24 ? t('time.upDaysHours', { count: d, hours: h % 24 }) : t('time.upDays', { count: d });
  if (h) return m % 60 ? t('time.upHoursMinutes', { count: h, minutes: m % 60 }) : t('time.upHours', { count: h });
  if (m) return t('time.upMinutes', { count: m });
  return t('time.upLessThanMinute');
}
const clock = (sec) => `${Math.floor(sec / 60)}:${String(sec % 60).padStart(2, '0')}`;
const timeAttr = (ms) => `datetime="${new Date(ms).toISOString()}" title="${esc(dfFull.format(ms))}"`;
const signedInAs = (a) => (a?.name ? (a.handle ? t('account.signedInAsHandle', { name: a.name, handle: a.handle }) : t('account.signedInAs', { name: a.name })) : t('shared.signedIn'));
const ORIGIN = location.origin;
const LOCAL = ['localhost', '127.0.0.1', '[::1]', '::1'].includes(location.hostname);
// the server guide's update steps (a server updates with git pull and docker compose, not from here)
const UPDATE_HELP = 'https://github.com/jarvissax/rondo/blob/main/docs/server.md#keeping-it-running';

// ---------- API ----------
class ApiError extends Error {
  constructor(status, code, data) { super(code || `HTTP ${status}`); this.status = status; this.code = code; this.data = data; }
}
async function api(method, path, body, { timeout = 20000, text = false, auth = true } = {}) {
  const headers = {};
  let payload;
  if (body !== undefined) {
    headers['Content-Type'] = text ? 'text/plain; charset=utf-8' : 'application/json';
    payload = text ? body : JSON.stringify(body);
  }
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), timeout);
  let res;
  try {
    res = await fetch(`/admin/api${path}`, { method, headers, body: payload, signal: ctl.signal, credentials: 'same-origin', cache: 'no-store' });
  } catch (e) {
    clearTimeout(timer);
    const timedOut = e?.name === 'AbortError';
    if (!timedOut) connectionLost();
    throw new ApiError(0, timedOut ? 'timeout' : 'network');
  }
  clearTimeout(timer);
  connectionBack();
  const date = Date.parse(res.headers.get('date') || '');
  if (date) { const skew = date - Date.now(); S.skew = Math.abs(skew) > 5000 ? skew : 0; }
  let data = null;
  try { data = await res.json(); } catch { /* empty body */ }
  if (res.status === 401 && auth) { needSignIn(); throw new ApiError(401, 'signin', data); }
  if (!res.ok) throw new ApiError(res.status, data?.error, data);
  return data;
}
// the server answers with codes ("not_found", "google"…), the same for every browser: shown as they are
const reason = (e) => (e?.code === 'timeout' ? t('error.timeout')
  : e?.status === 0 ? t('error.unreachable')
  : e?.code ? t('error.serverSaid', { code: String(e.code) })
  : e?.status != null ? t('error.somethingWrong', { status: String(e.status) }) : t('error.somethingWrongPlain'));

// ---------- screens ----------
const screens = { splash: $('#splash'), offline: $('#offline'), setup: $('#setup'), signin: $('#signin'), app: $('#app') };
function showScreen(name) {
  for (const [k, el] of Object.entries(screens)) el.hidden = k !== name;
  if (name !== 'app') { stopPolling(); sheet.open && sheet.close(); }
}
function markInsecure(https) { $$('[data-insecure]').forEach((el) => { el.hidden = !!https; }); }

// the page's language: under the sign-in, setup and offline screens, and in Admin account › This
// browser; each language in its own name, this browser keeps the choice (setLang reloads the page)
function renderLangPickers() {
  const many = L.languages.length > 1;
  for (const box of $$('[data-lang-pick]')) {
    const gate = !!box.closest('.gate');
    box.innerHTML = many ? `<div class="select">${gate ? icon('globe', 'i lang-globe') : ''}<select aria-label="${te('language.label')}">${L.languages.map((l) => `<option value="${esc(l.code)}" lang="${esc(l.code)}"${l.code === L.code ? ' selected' : ''}>${esc(l.name)}</option>`).join('')}</select>${icon('down')}</div>` : '';
    if (gate) box.hidden = !many;
  }
  $('#langRow').hidden = !many;
}
document.addEventListener('change', (e) => {
  if (e.target.matches('[data-lang-pick] select') && e.target.value !== L.code) setLang(e.target.value);
});
renderLangPickers();

let offlineTimer = 0;
function showOffline(e) {
  showScreen('offline');
  $('#offlineText').textContent = e?.status
    ? t('offline.serverError', { code: String(e.code || e.status) })
    : t('offline.checkAddress');
  $('#offlineNote').textContent = t('offline.retrying');
  clearTimeout(offlineTimer);
  offlineTimer = setTimeout(() => { if (!document.hidden) boot(); }, 5000);
}
$('#offlineRetry').addEventListener('click', () => { $('#offlineNote').textContent = t('offline.trying'); boot(); });

function connectionLost() {
  if (!S.booted || screens.app.hidden) return;
  if (S.online) {
    S.online = false;
    $('#banner').hidden = false;
    schedulePoll();
  }
}
function connectionBack() {
  if (!S.online) { S.online = true; $('#banner').hidden = true; }
}

// ---------- boot ----------
async function boot() {
  clearTimeout(offlineTimer);
  let st;
  try { st = await api('GET', '/state', undefined, { auth: false }); } catch (e) { return showOffline(e); }
  S.state = st;
  markInsecure(st.https);
  if (st.setup) return showSetup();
  if (!st.signedIn) return showSignIn();
  try { applyStatus(await api('GET', '/status'), false); } catch (e) { if (e.status !== 401) showOffline(e); return; }
  enterApp();
}

function enterApp() {
  if (!S.booted) buildNav();
  S.booted = true;
  showScreen('app');
  route(true);
  startPolling();
}

// ---------- passwords ----------
function strength(pw) {
  const n = [...pw].length;
  if (!n) return { level: 0, text: t('password.atLeast', { count: MIN_PASSWORD }) };
  if (n < MIN_PASSWORD) return { level: 1, text: t('password.tooShort', { count: MIN_PASSWORD - n }) };
  const classes = [/[a-z]/, /[A-Z]/, /\d/, /[^A-Za-z0-9]/].filter((r) => r.test(pw)).length;
  const unique = new Set(pw.toLowerCase()).size;
  const guessable = unique <= 4 || /^\d+$/.test(pw) || /^(?:password|qwerty|letmein|admin|welcome|rondo|123456|abcdef)/i.test(pw)
    || /(?:0123|1234|2345|3456|4567|5678|6789|abcd|bcde|cdef)/i.test(pw) && unique < 8;
  if (guessable) return { level: 2, text: t('password.guessable') };
  let score = Math.max(0, classes - 1);
  if (n >= 14) score++;
  if (n >= 18) score++;
  if (n >= 24) score++;
  if (unique < n / 3) score = Math.min(score, 1);
  if (score <= 1) return { level: 2, text: t('password.okay') };
  if (score <= 3) return { level: 3, text: t('password.good') };
  return { level: 4, text: t('password.strong') };
}
function bindStrength(input, out) {
  const update = () => {
    const s = strength(input.value);
    out.dataset.level = s.level;
    $('span', out).textContent = s.text;
  };
  input.addEventListener('input', update);
  update();
}
document.addEventListener('click', (e) => {
  const eye = e.target.closest('.pw-eye');
  if (!eye) return;
  const input = $('input', eye.parentElement);
  const show = input.type === 'password';
  input.type = show ? 'text' : 'password';
  eye.setAttribute('aria-pressed', String(show));
  eye.setAttribute('aria-label', show ? t('password.hide') : t('password.show'));
  $('use', eye).setAttribute('href', show ? '#i-eye-off' : '#i-eye');
  input.focus();
});
const clearInvalid = (e) => { if (e.target.matches('input')) e.target.classList.remove('is-invalid'); };

// ---------- setup ----------
const setupForm = $('#setupForm'), setupCode = $('#setupCode'), setupBox = $('#setupCodeBox');
const setupPw = $('#setupPw'), setupPw2 = $('#setupPw2'), setupErr = $('#setupErr'), setupBtn = $('#setupBtn');
// its example code keeps its tabular digits (a blank: texts carry only plain <b>, <code>…)
$('#setupCodeHelp').innerHTML = th('setup.codeHelp', { example: html('<b class="num">482-913</b>') });
bindStrength(setupPw, $('#setupStrength'));
setupForm.addEventListener('input', clearInvalid);
function showSetup() {
  showScreen('setup');
  renderSetupCode();
  setTimeout(() => setupCode.focus(), 60);
}
function renderSetupCode() {
  const v = setupCode.value;
  $$('.code-boxes span', setupBox).forEach((b, i) => {
    b.textContent = v[i] || '';
    b.classList.toggle('is-filled', !!v[i]);
    b.classList.toggle('is-active', i === Math.min(v.length, 5));
  });
}
setupCode.addEventListener('input', () => {
  const before = setupCode.dataset.len | 0;
  setupCode.value = setupCode.value.replace(/\D/g, '').slice(0, 6);
  setupCode.dataset.len = setupCode.value.length;
  setupBox.classList.remove('is-error');
  renderSetupCode();
  if (setupCode.value.length === 6 && before < 6 && !setupPw.value) setupPw.focus();
});
setupCode.addEventListener('focus', renderSetupCode);
setupCode.addEventListener('blur', () => $$('.code-boxes span', setupBox).forEach((b) => b.classList.remove('is-active')));
function setupFail(msg, field) {
  setupErr.className = 'form-err';
  setupErr.textContent = msg;
  if (field === 'code') { setupBox.classList.remove('is-error'); void setupBox.offsetWidth; setupBox.classList.add('is-error'); setupCode.focus(); }
  else if (field) { field.classList.add('is-invalid'); field.focus(); }
}
setupForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  if (setupBtn.disabled) return;
  const code = setupCode.value, pw = setupPw.value;
  if (code.length !== 6) return setupFail(t('setup.allDigits'), 'code');
  if ([...pw].length < MIN_PASSWORD) return setupFail(t('password.needsAtLeast', { count: MIN_PASSWORD }), setupPw);
  if (pw !== setupPw2.value) return setupFail(t('setup.noMatch'), setupPw2);
  setupErr.textContent = '';
  setupBtn.disabled = true;
  setupBtn.textContent = t('setup.settingUp');
  $('#setupMark').classList.add('is-turning');
  try {
    await api('POST', '/setup', { code, password: pw }, { auth: false });
    setupForm.reset();
    setupCode.dataset.len = 0;
    await boot();
    toast(t('setup.done'));
  } catch (err) {
    if (err.status === 403) { setupCode.value = ''; setupCode.dataset.len = 0; renderSetupCode(); setupFail(t('setup.wrongCode'), 'code'); }
    else if (err.status === 400) setupFail(t('password.needsAtLeast', { count: MIN_PASSWORD }), setupPw);
    else if (err.status === 409) { setupFail(t('setup.already')); setTimeout(boot, 1600); }
    else if (err.status === 429) setupFail(t('setup.tooManyTries', { count: Number(err.data?.retryAfterSec ?? 60) }));
    else setupFail(err.status === 0 ? t('error.unreachableRetry') : t('setup.failed', { reason: reason(err) }));
  } finally {
    setupBtn.disabled = false;
    setupBtn.textContent = t('setup.button');
    $('#setupMark').classList.remove('is-turning');
  }
});

// ---------- sign-in ----------
const signinForm = $('#signinForm'), signinPw = $('#signinPw'), signinErr = $('#signinErr'), signinBtn = $('#signinBtn');
signinForm.addEventListener('input', (e) => { clearInvalid(e); if (!waitUntil) signinErr.textContent = ''; });
let waitTimer = 0, waitUntil = 0;
function showSignIn(note) {
  const wasShown = !screens.signin.hidden;
  showScreen('signin');
  if (!wasShown) { signinPw.value = ''; signinPw.type = 'password'; }
  if (note) { signinErr.className = 'form-err is-quiet'; signinErr.textContent = note; }
  setTimeout(() => signinPw.focus(), 60);
}
function needSignIn() {
  if (!screens.signin.hidden || !screens.setup.hidden) return;
  showSignIn(S.booted ? t('signin.sessionEnded') : '');
  // a server that lost its config wants setup again
  api('GET', '/state', undefined, { auth: false }).then((st) => { S.state = st; markInsecure(st.https); if (st.setup) showSetup(); }).catch(() => {});
}
function startWait(sec) {
  waitUntil = Date.now() + sec * 1000;
  signinBtn.disabled = true;
  signinErr.className = 'form-err';
  signinErr.textContent = t('signin.tooMany');
  const tickWait = () => {
    const left = Math.ceil((waitUntil - Date.now()) / 1000);
    if (left <= 0) {
      clearInterval(waitTimer);
      waitUntil = 0;
      signinBtn.disabled = false;
      signinBtn.textContent = t('signin.signIn');
      signinErr.className = 'form-err is-quiet';
      signinErr.textContent = t('signin.canRetry');
      return;
    }
    signinBtn.textContent = t('signin.retryIn', { time: clock(left) });
  };
  clearInterval(waitTimer);
  tickWait();
  waitTimer = setInterval(tickWait, 1000);
}
signinForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  if (signinBtn.disabled) return;
  const pw = signinPw.value;
  if (!pw) { signinErr.className = 'form-err'; signinErr.textContent = t('signin.enterPassword'); return signinPw.focus(); }
  signinBtn.disabled = true;
  signinBtn.textContent = t('signin.signingIn');
  $('#signinMark').classList.add('is-turning');
  let wait = 0;
  try {
    await api('POST', '/login', { password: pw }, { auth: false });
    signinErr.textContent = '';
    signinPw.value = '';
    await boot();
  } catch (err) {
    signinErr.className = 'form-err';
    if (err.status === 403) { signinErr.textContent = t('signin.wrongPassword'); signinPw.classList.add('is-invalid'); signinPw.select(); }
    else if (err.status === 429) wait = Math.max(1, Number(err.data?.retryAfterSec) || 60);
    else signinErr.textContent = err.status === 0 ? t('error.unreachableRetry') : t('signin.failed', { reason: reason(err) });
  } finally {
    $('#signinMark').classList.remove('is-turning');
    signinBtn.disabled = false;
    signinBtn.textContent = t('signin.signIn');
    if (wait) startWait(wait);
  }
});

// ---------- navigation ----------
// (the page's language is loaded before this runs, and changing it reloads the page)
const NAV = [
  { id: 'overview', label: t('nav.overview'), short: t('nav.overview'), tile: 'rondo' },
  { id: 'youtube', label: 'YouTube', short: 'YouTube', tile: 'youtube' },
  { id: 'whatsapp', label: 'WhatsApp', short: 'WhatsApp', tile: 'whatsapp' },
  { id: 'device', label: t('nav.device'), short: t('nav.device'), tile: 'device', glyph: 'player' },
  { id: 'resolver', label: t('nav.resolver'), short: t('nav.resolver'), tile: 'resolver', glyph: 'bolt' },
  { id: 'activity', label: t('nav.activity'), short: t('nav.activity'), tile: 'activity', glyph: 'pulse' },
  { id: 'admin', label: t('nav.admin'), short: t('nav.adminShort'), tile: 'settings', glyph: 'lock' },
];
// YouTube and WhatsApp show their own logos on white tiles, as on the Rondo
const tileHtml = (tile, glyph, extra = '') => tile === 'youtube' ? ytTile(extra) : tile === 'whatsapp' ? waTile(extra)
  : `<span class="tile t-${tile}${extra}">${glyph ? icon(glyph) : markTile}</span>`;
const waTile = (extra = '') => `<span class="tile t-whatsapp${extra}"><svg class="brand-glyph" viewBox="0 0 24 24" aria-hidden="true"><path fill="currentColor" d="M17.472 14.382c-.297-.149-1.758-.867-2.03-.967-.273-.099-.471-.148-.67.15-.197.297-.767.966-.94 1.164-.173.199-.347.223-.644.075-.297-.15-1.255-.463-2.39-1.475-.883-.788-1.48-1.761-1.653-2.059-.173-.297-.018-.458.13-.606.134-.133.298-.347.446-.52.149-.174.198-.298.298-.497.099-.198.05-.371-.025-.52-.075-.149-.669-1.612-.916-2.207-.242-.579-.487-.5-.669-.51-.173-.008-.371-.01-.57-.01-.198 0-.52.074-.792.372-.272.297-1.04 1.016-1.04 2.479 0 1.462 1.065 2.875 1.213 3.074.149.198 2.096 3.2 5.077 4.487.709.306 1.262.489 1.694.625.712.227 1.36.195 1.871.118.571-.085 1.758-.719 2.006-1.413.248-.694.248-1.289.173-1.413-.074-.124-.272-.198-.57-.347z m-5.421 7.403h-.004a9.87 9.87 0 01-5.031-1.378l-.361-.214-3.741.982.998-3.648-.235-.374a9.86 9.86 0 01-1.51-5.26c.001-5.45 4.436-9.884 9.888-9.884 2.64 0 5.122 1.03 6.988 2.898a9.825 9.825 0 012.893 6.994c-.003 5.45-4.437 9.884-9.885 9.884m8.413-18.297A11.815 11.815 0 0012.05 0C5.495 0 .16 5.335.157 11.892c0 2.096.547 4.142 1.588 5.945L.057 24l6.305-1.654a11.882 11.882 0 005.683 1.448h.005c6.554 0 11.89-5.335 11.893-11.893a11.821 11.821 0 00-3.48-8.413z"/></svg></span>`;
// YouTube's and WhatsApp's logos in white on their own colors, like the page's other tiles (YouTube's
// play mark cut out in its tile's red)
const ytTile = (extra = '') => `<span class="tile t-youtube${extra}"><svg class="brand-glyph yt" viewBox="0 0 28.57 20" aria-hidden="true"><path fill="currentColor" d="M27.9727 3.12324C27.6435 1.89323 26.6768 .926623 25.4468 .597366C23.2197 0 14.285 0 14.285 0C14.285 0 5.35042 0 3.12323 .597366C1.89323 .926623 .926623 1.89323 .597366 3.12324C0 5.35042 0 10 0 10C0 10 0 14.6496 .597366 16.8768C.926623 18.1068 1.89323 19.0734 3.12323 19.4026C5.35042 20 14.285 20 14.285 20C14.285 20 23.2197 20 25.4468 19.4026C26.6768 19.0734 27.6435 18.1068 27.9727 16.8768C28.5701 14.6496 28.5701 10 28.5701 10C28.5701 10 28.5677 5.35042 27.9727 3.12324Z"/><path class="cut" d="M11.4253 14.2854L18.8477 10.0004L11.4253 5.71533Z"/></svg></span>`;
function buildNav() {
  $('#nav').innerHTML = NAV.map((n) => `<a href="#${n.id}" data-id="${n.id}">${tileHtml(n.tile, n.glyph)}<span>${esc(n.label)}</span><span class="nav-badge"></span>${icon('chevron', 'chev')}</a>`).join('');
  $('#tabs').innerHTML = NAV.map((n) => `<a class="tab" href="#${n.id}" data-id="${n.id}"><span class="tab-wrap">${tileHtml(n.tile, n.glyph)}<span class="nav-badge"></span></span><span>${esc(n.short)}</span></a>`).join('');
}
function route(force = false) {
  const id = location.hash.slice(1);
  const next = NAV.some((n) => n.id === id) ? id : 'overview';
  const changed = next !== S.route || $(`#view-${next}`).hidden;
  S.route = next;
  for (const n of NAV) $(`#view-${n.id}`).hidden = n.id !== next;
  $$('#nav a, #tabs a').forEach((a) => (a.dataset.id === next ? a.setAttribute('aria-current', 'page') : a.removeAttribute('aria-current')));
  document.title = next === 'overview' ? t('shared.rondoServer') : `${NAV.find((n) => n.id === next).label} · ${t('shared.rondoServer')}`;
  if (changed) window.scrollTo(0, 0);
  if (changed || force) enterView(next);
}
window.addEventListener('hashchange', () => { if (S.booted && !screens.app.hidden) route(); });
function enterView(id) {
  renderChrome();
  if (id === 'activity') loadLogs();
  else renderView(id);
}
function renderView(id = S.route) {
  if (!S.status) return;
  if (id === 'overview') renderOverview();
  if (id === 'youtube') renderYouTube();
  if (id === 'whatsapp') renderWhatsApp();
  if (id === 'device') renderDevice();
  if (id === 'resolver') renderResolver();
}
// only touch the DOM when the markup changed, so polling doesn't steal focus or restart animations
function setHtml(el, html) {
  if (el._html === html) return;
  el._html = html;
  el.innerHTML = html;
}

// ---------- polling ----------
let pollTimer = 0, tickTimer = 0, polling = false;
const waBusy = () => { const w = S.status?.whatsapp; return !!w && (w.state === 'linking' || w.state === 'connecting' || w.syncing); };
const pollEvery = () => (!S.online ? 5000 : (S.pair && !S.pair.expired) || S.status?.tv?.pending || waBusy() ? 3000 : 10000);
function startPolling() {
  schedulePoll();
  clearInterval(tickTimer);
  tickTimer = setInterval(tick, 1000);
}
function stopPolling() { clearTimeout(pollTimer); clearInterval(tickTimer); tickTimer = 0; }
function schedulePoll(ms = pollEvery()) {
  clearTimeout(pollTimer);
  if (document.hidden || !S.booted || screens.app.hidden) return;
  pollTimer = setTimeout(poll, ms);
}
async function poll() {
  if (polling || S.updatingServer) return;
  polling = true;
  try { applyStatus(await api('GET', '/status')); } catch { /* banner or sign-in handles it */ } finally {
    polling = false;
    schedulePoll();
  }
}
document.addEventListener('visibilitychange', () => {
  if (document.hidden) { clearTimeout(pollTimer); return; }
  if (S.booted && !screens.app.hidden) poll();
});

function applyStatus(st, render = true) {
  if (st.updates?.server && Date.now() - (S.serverUpdateAt || 0) > 60000) loadServerUpdate();
  if (S.status?.tv?.pending && st.tv?.signedIn && !S.status.tv.signedIn) toast(t('tv.signedInToast'));
  if (S.status?.whatsapp && S.status.whatsapp.state !== 'open' && st.whatsapp?.state === 'open') toast(t('whatsapp.linkedToast'));
  S.status = st;
  S.statusAt = Date.now();
  const d = st.device || {};
  const pairedAt = d.source === 'paired' ? d.pairedAt ?? null : null;
  if (S.pair) {
    if (pairedAt && pairedAt !== S.pair.base) {
      S.pair = null;
      S.celebrate = true;
      if (S.route !== 'device') toast(t('pair.connected'));
    } else if (!st.pairing) {
      // gone: expired, or canceled from another tab
      if (now() >= S.pair.expiresAt - 3000) S.pair.expired = true;
      else S.pair = null;
    } else if (st.pairing.code !== S.pair.code) {
      S.pair = { code: st.pairing.code, expiresAt: st.pairing.expiresAt, base: S.pair.base, total: CODE_MS };
    }
  } else if (st.pairing && !S.celebrate) {
    S.pair = { code: st.pairing.code, expiresAt: st.pairing.expiresAt, base: pairedAt, total: CODE_MS };
  }
  if (render && !screens.app.hidden) { renderChrome(); renderView(); }
}

function tick() {
  if (screens.app.hidden) return;
  if (S.pair && !S.pair.expired && now() >= S.pair.expiresAt) { S.pair.expired = true; if (S.route === 'device') renderDevice(); }
  const cd = $('[data-countdown]');
  if (cd && S.pair) {
    const left = Math.max(0, Math.ceil((S.pair.expiresAt - now()) / 1000));
    cd.textContent = S.pair.expired ? t('shared.codeExpired') : t('shared.expiresIn', { time: clock(left) });
    cd.classList.toggle('is-low', !S.pair.expired && left <= 60);
    const drain = $('[data-drain]');
    if (drain) drain.style.setProperty('--p', S.pair.expired ? 0 : Math.min(1, (left * 1000) / Math.max(S.pair.total || CODE_MS, left * 1000)).toFixed(4));
  }
  tickTv();
  tickWa();
  const el = $('[data-elapsed]');
  if (el && S.rz.updating) el.textContent = clock(Math.floor((Date.now() - S.rz.updateStart) / 1000));
}

// ---------- what's ok, what isn't ----------
function areas(st) {
  const c = st.cookies || {}, d = st.device || {}, y = st.ytdlp || {}, s = st.server || {}, tv = st.tv || {};
  const https = s.https ?? S.state?.https ?? false;
  const cookiesOk = c.present && c.signedIn !== false;
  const yt = tv.signedIn
    ? { level: c.present && c.signedIn === false ? 'warn' : 'ok', pill: t('shared.signedIn'), node: t('shared.signedIn'), title: signedInAs(tv.account), sub: c.present && c.signedIn === false ? t('youtube.withCodeCookiesOut') : t('youtube.withCode') }
    : !c.present
    ? { level: 'off', pill: t('shared.signedOut'), node: t('shared.signedOut'), title: t('shared.notSignedIn'), sub: t('youtube.stillWorks') }
    : !c.present
    ? { level: 'warn', pill: t('cookies.none'), node: t('cookies.none'), title: t('cookies.none'), sub: t('cookies.needThem') }
    : c.signedIn === false
      ? { level: 'bad', pill: t('shared.signedOut'), node: t('shared.signedOut'), title: t('cookies.signedOutUpload'), sub: c.checkedAt ? t('cookies.checkedAgo', { ago: ago(c.checkedAt) }) : t('cookies.sessionEnded') }
      : c.signedIn === true
        ? { level: 'ok', pill: t('shared.signedIn'), node: t('shared.signedIn'), title: signedInAs(c.account), sub: c.checkedAt ? t('cookies.checkedAgo', { ago: ago(c.checkedAt) }) : '' }
        : { level: 'off', pill: t('shared.notChecked'), node: t('cookies.checking'), title: t('cookies.uploaded'), sub: t('cookies.notCheckedYet') };
  yt.detail = tv.signedIn ? t('youtube.detailWithCode', { account: signedInAs(tv.account) })
    : !c.present ? t('youtube.detailNotSignedIn')
    : c.signedIn === false ? t('youtube.detailSignedOut')
    : c.signedIn === true ? (c.checkedAt ? t('youtube.detailChecked', { account: signedInAs(c.account), ago: ago(c.checkedAt) }) : signedInAs(c.account))
    : t('youtube.detailNotChecked');
  const seen = d.lastSeenAt ? t('device.lastSeen', { ago: ago(d.lastSeenAt) }) : t('device.notSeen');
  // the same in the middle of a line (an old date is lowercased too, as before: "last seen nov 3")
  const seenLower = d.lastSeenAt ? t('device.lastSeenLower', { ago: ago(d.lastSeenAt).toLowerCase() }) : t('device.notSeenLower');
  const dev = d.source === 'paired'
    ? { level: 'ok', pill: t('device.connected'), node: d.lastSeenAt ? t('device.seen', { ago: ago(d.lastSeenAt) }) : t('device.connected'), title: t('device.rondoConnected'), sub: seen }
    : d.source === 'env'
      ? { level: 'info', pill: 'API_TOKEN', node: d.lastSeenAt ? t('device.seen', { ago: ago(d.lastSeenAt) }) : 'API_TOKEN', title: t('device.usesApiToken'), sub: `${seen} · ${t('device.connectForToken')}` }
      : { level: 'warn', pill: t('device.notConnected'), node: t('device.notConnected'), title: t('device.noRondo'), sub: t('device.connectFromDevicePage') };
  dev.detail = d.source === 'paired' ? `${t('device.rondoConnected')} · ${seenLower}`
    : d.source === 'env' ? `${t('device.usesApiTokenEnv')} · ${seenLower}`
    : t('device.noRondoSentence');
  const rz = y.lastResolveOk === true
    ? { level: 'ok', pill: t('resolver.working'), sub: t('resolver.lastCheckWorked') }
    : y.lastResolveOk === false
      ? { level: 'bad', pill: t('resolver.failing'), sub: t('resolver.lastCheckFailed') }
      : { level: 'off', pill: t('shared.notChecked'), sub: t('resolver.nothingPlayed') };
  const conn = https
    ? { level: 'ok', pill: 'HTTPS', sub: t('overview.encrypted') }
    : { level: 'warn', pill: t('shared.notEncrypted'), sub: t('overview.plainHttp') };
  return { yt, dev, rz, conn, https, c, d, y, s };
}
const pill = (level, text) => `<span class="pill ${level === 'info' ? 'off' : level}">${esc(text)}</span>`;

// ---------- chrome: connection, version, badges ----------
function connHtml(https) {
  return https
    ? `<span class="conn">${icon('lock')}<span>HTTPS</span></span>`
    : `<span class="conn is-plain">${icon('unlock')}<span>${te('shared.notEncrypted')}</span></span>`;
}
function renderChrome() {
  if (!S.status) return;
  const a = areas(S.status);
  setHtml($('#topStatus'), connHtml(a.https));
  const up = (a.s.uptimeSec ?? 0) + Math.floor((Date.now() - S.statusAt) / 1000);
  setHtml($('#sideFoot'), `${connHtml(a.https)}<small>${te('chrome.serverUp', { version: a.s.version || '', uptime: uptime(up) })}</small>`);
  const badges = { youtube: a.yt.level, device: a.dev.level, resolver: a.rz.level };
  $$('.nav-badge').forEach((b) => {
    const level = badges[b.closest('[data-id]').dataset.id];
    const badge = level === 'bad' || level === 'warn'
      ? `<span class="dot-badge ${level}" role="img" aria-label="${level === 'bad' ? te('nav.problem') : te('nav.attention')}"></span>` : '';
    setHtml(b, badge);
  });
}

// ---------- overview ----------
function renderOverview() {
  const a = areas(S.status);
  // the summary is the Rondo and this server: YouTube (directly or through this server, the Rondo's
  // choice) and WhatsApp have their own rows under Status, which say when they need something
  const level = a.dev.level === 'info' ? 'ok' : a.dev.level;
  const todo = [];
  if (a.dev.level === 'warn') todo.push(['warn', S.pair && !S.pair.expired ? t('overview.codeWaiting') : t('overview.connectToStream'), '#device', S.pair && !S.pair.expired ? t('overview.showCode') : t('overview.connect')]);
  if (a.dev.level === 'info') todo.push(['info', t('overview.usesApiToken'), '#device', t('overview.connect')]);
  // newer releases (updates.ts on the server, asked of GitHub every 6 hours)
  const news = st0().updates || {};
  if (news.ytdlp) todo.push(['info', t('overview.ytdlpOut', { version: String(news.ytdlp.version) }), '#resolver', t('overview.update')]);
  if (news.server) {
    const text = t('overview.serverOut', { version: String(news.server.version), current: String(a.s.version) });
    todo.push(S.serverUpdate?.available ? ['info', text, '#server-update', t('overview.update')] : ['info', text, UPDATE_HELP, t('overview.howToUpdate')]);
  }
  const title = level === 'ok' ? t('overview.allWorks') : t('overview.connectRondo');
  const sub = level === 'ok'
    ? t('overview.connectedHere')
    : t('overview.thenItCan');

  const link = (lv) => {
    const dot = lv === 'ok' ? icon('check') : lv === 'bad' || lv === 'warn' ? icon('alert') : '';
    return `<div class="link ${lv}" aria-hidden="true"><span class="link-dot">${dot}</span></div>`;
  };
  // the Rondo and this server: what they serve (YouTube, WhatsApp) has its own row under Status
  const devLevel = a.d.source ? 'ok' : 'off';
  const chainLabel = t('overview.chainLabel', { rondo: a.d.source ? t('overview.chainConnected') : t('overview.chainNotConnected'), server: a.https ? 'HTTPS' : t('overview.chainNotEncrypted') });
  const chain = `
    <div class="chain" role="img" aria-label="${esc(chainLabel)}">
      <div class="node">${tileHtml(a.d.source ? 'device' : 'off', 'player')}<span class="node-name">Rondo</span><span class="node-state ${a.d.source ? '' : 'warn'}">${esc(a.dev.node)}</span></div>
      ${link(devLevel)}
      <div class="node">${tileHtml('rondo')}<span class="node-name">${te('overview.thisServer')}</span><span class="node-state ${a.https ? '' : 'warn'}">${a.https ? 'HTTPS' : te('shared.notEncrypted')}</span></div>
    </div>`;
  const sum = $('#sum');
  sum.className = `sum ${level === 'ok' ? '' : level}`;
  setHtml(sum, `
    <div class="sum-text">
      <h2 class="sum-title">${esc(title)}</h2>
      <p class="sum-sub">${esc(sub)}</p>
    </div>
    ${todo.length ? `<ul class="todo">${todo.map(([lv, text, href, label]) => `<li class="${lv}">${icon(lv === 'info' ? 'info' : 'alert')}<span>${esc(text)}</span><a class="btn btn-sm" href="${href}"${href.startsWith('http') ? ' target="_blank" rel="noopener"' : ''}>${esc(label)}</a></li>`).join('')}</ul>` : ''}
    ${chain}`);

  const up = (a.s.uptimeSec ?? 0) + Math.floor((Date.now() - S.statusAt) / 1000);
  const row = ({ href, tile, glyph, title, sub, level, pillText, extra = '' }) => {
    const tag = href ? 'a' : 'div';
    return `<${tag} class="row"${href ? ` href="${href}"` : ''}>${tileHtml(tile, glyph, extra)}
      <div class="row-main"><div class="row-title">${esc(title)}</div>${sub ? `<div class="row-sub">${esc(sub)}</div>` : ''}</div>
      <div class="row-end">${pill(level, pillText)}${href ? icon('chevron', 'row-chev') : ''}</div></${tag}>`;
  };
  const serverSub = t('chrome.serverUp', { version: a.s.version || '', uptime: uptime(up) })
    + (st0().updates?.server ? ` · ${t('overview.versionOut', { version: String(st0().updates.server.version) })}` : '');
  setHtml($('#statusRows'), [
    row({ tile: 'rondo', title: t('overview.server'), sub: serverSub, level: st0().updates?.server ? 'info' : 'ok', pillText: st0().updates?.server ? t('overview.updatePill') : t('overview.running') }),
    row({ tile: a.https ? 'ok' : 'warn', glyph: a.https ? 'lock' : 'unlock', title: t('overview.connection'), sub: a.conn.sub, level: a.conn.level, pillText: a.conn.pill }),
    row({ href: '#resolver', tile: 'resolver', glyph: 'bolt', title: t('nav.resolver'), sub: t('overview.ytdlpLine', { version: a.y.version || t('overview.versionUnknown'), status: a.rz.sub }), level: a.rz.level, pillText: a.rz.pill }),
    row({ href: '#youtube', tile: 'youtube', title: 'YouTube', sub: a.yt.detail, level: a.yt.level, pillText: a.yt.pill }),
    row({ href: '#device', tile: 'device', glyph: 'player', title: t('nav.device'), sub: a.dev.detail, level: a.dev.level, pillText: a.dev.pill }),
    st0().whatsapp ? row({ href: '#whatsapp', tile: 'whatsapp', title: 'WhatsApp', ...waRow(st0().whatsapp) }) : '',
  ].join(''));
}

// ---------- YouTube account ----------
const st0 = () => S.status || {};

// ---------- sign in with a code (tv.ts on the server) ----------
// the server's codes for why the last sign-in ended
const tvError = (code) => (code === 'denied' ? t('tv.errorDenied')
  : code === 'expired' ? t('tv.errorExpired')
  : code === 'revoked' ? t('tv.errorRevoked')
  : code === 'failed' ? t('tv.errorFailed') : '');
function renderTv() {
  const tv = st0().tv;
  const body = $('#tvBody');
  if (!tv) { setHtml(body, ''); return; }
  if (tv.pending) {
    const groups = String(tv.pending.code).split('-');
    setHtml(body, `<section class="pairing" aria-labelledby="tvLabel">
      <div class="pc-code">
        <p class="pc-label" id="tvLabel">${te('tv.codeLabel')}</p>
        <div class="big-code is-letters" aria-label="${esc(tv.pending.code.split('').join(' '))}">${groups.map((g) => `<span>${esc(g)}</span>`).join('')}</div>
        <p class="expiry" data-tv-countdown role="timer" aria-live="off"></p>
        <div class="pc-actions">
          <button class="btn btn-sm" type="button" data-act="tvCopy">${icon('copy')}<span>${te('shared.copyCode')}</span></button>
          <button class="btn btn-sm btn-quiet" type="button" data-act="tvCancel">${te('shared.cancel')}</button>
        </div>
      </div>
      <div class="pc-steps">
        <ol class="steps">
          <li>${th('tv.step1', { link: html(`<a href="${esc(tv.pending.url)}" target="_blank" rel="noopener noreferrer"><b>${esc(tv.pending.url.replace(/^https?:\/\//, ''))}</b></a>`) })}</li>
          <li>${te('tv.step2')}</li>
          <li>${te('tv.step3')}</li>
        </ol>
        <p class="waiting">${spinner()}<span>${te('tv.waiting')}</span></p>
        <div class="note info">${icon('info')}<span>${te('tv.likeTv')}</span></div>
      </div>
    </section>`);
    tickTv();
    return;
  }
  if (tv.signedIn) {
    const name = tv.account?.name;
    const lead = name ? `<span class="avatar" aria-hidden="true">${esc([...name.trim()][0]?.toUpperCase() || '?')}</span>` : tileHtml('youtube', 'account');
    setHtml(body, `<div class="group"><div class="row acct">${lead}
        <div class="row-main"><div class="row-title">${esc(signedInAs(tv.account))}</div><div class="row-sub">${te('tv.whatYouGet')}</div></div>
        ${pill('ok', t('shared.signedIn'))}
      </div>
      <div class="row-actions"><button class="btn btn-red-text push" type="button" data-act="tvSignOut">${icon('close')}<span>${te('shared.signOut')}</span></button></div></div>`);
    return;
  }
  const why = tv.error ? tvError(tv.error) : '';
  const err = why ? `<div class="note err">${icon('alert')}<span>${esc(why)}</span></div>` : '';
  setHtml(body, `${err}<div class="group"><div class="row row-wrap">${ytTile()}
      <div class="row-main"><div class="row-title">${te('shared.notSignedIn')}</div><div class="row-sub">${te('tv.getCodeText')}</div></div>
      <button class="btn btn-primary${S.tvBusy ? ' is-busy' : ''}" type="button" data-act="tvStart"${S.tvBusy ? ' disabled' : ''}>${S.tvBusy ? spinner() : icon('account')}<span>${te('shared.getCode')}</span></button>
    </div></div>`);
}
function tickTv() {
  const cd = $('[data-tv-countdown]'), p = st0().tv?.pending;
  if (!cd || !p) return;
  const left = Math.max(0, Math.ceil((p.expiresAt - now()) / 1000));
  cd.textContent = left ? t('shared.expiresIn', { time: clock(left) }) : t('shared.codeExpired');
  cd.classList.toggle('is-low', left <= 60);
}
async function tvStart() {
  S.tvBusy = true;
  renderTv();
  try {
    const pending = await api('POST', '/tv', undefined, { timeout: 30000 });
    if (S.status) S.status.tv = { ...(S.status.tv || {}), signedIn: false, pending, error: null };
  } catch (err) {
    if (err.status !== 401) toast(t('tv.noCode', { reason: reason(err) }), 'err');
  } finally {
    S.tvBusy = false;
    renderTv();
    schedulePoll();
  }
}
async function tvCancel() {
  try { await api('DELETE', '/tv/code'); } catch { /* the next status shows it */ }
  if (S.status?.tv) S.status.tv.pending = null;
  renderTv();
}
async function tvSignOut() {
  const ok = await confirmSheet({
    title: t('tv.signOutTitle'),
    text: t('tv.signOutText'),
    action: t('shared.signOut'), danger: true,
  });
  if (!ok) return;
  try {
    await api('DELETE', '/tv');
    toast(t('shared.signedOut'));
  } catch (err) {
    if (err.status !== 401) toast(t('tv.notSignedOut', { reason: reason(err) }), 'err');
  }
  poll();
}

function renderYouTube() {
  renderTv();
  const a = areas(S.status), c = a.c;
  let lead;
  if (c.present && c.signedIn === true && c.account?.name) {
    lead = `<span class="avatar" aria-hidden="true">${esc([...c.account.name.trim()][0]?.toUpperCase() || '?')}</span>`;
  } else {
    const tile = !c.present ? 'off' : c.signedIn === false ? 'bad' : c.signedIn === true ? 'youtube' : 'off';
    lead = tileHtml(tile, c.signedIn === false ? 'alert' : 'account');
  }
  const title = !c.present ? t('cookies.noFile') : c.signedIn === false ? t('cookies.fileSignedOut') : c.signedIn === true ? signedInAs(c.account) : t('cookies.uploaded');
  const sub = !c.present ? t('cookies.genericUntil')
    : c.signedIn === false ? (c.checkedAt ? `${t('cookies.sessionEnded')} ${t('cookies.checkedAgoSentence', { ago: ago(c.checkedAt) })}` : t('cookies.sessionEnded'))
    : c.signedIn === true ? (c.checkedAt ? t('cookies.checkedAgo', { ago: ago(c.checkedAt) }) : '')
    : t('cookies.notCheckedYet');
  // this group is about cookies.txt only (the code sign-in has its own above)
  const [level, pillText] = !c.present ? ['off', t('cookies.none')] : c.signedIn === false ? ['bad', t('shared.signedOut')] : c.signedIn === true ? ['ok', t('shared.signedIn')] : ['off', t('shared.notChecked')];
  const actions = c.present ? `
    <div class="row-actions">
      <button class="btn${S.checking ? ' is-busy' : ''}" type="button" data-act="checkCookies"${S.checking ? ' disabled' : ''}>${icon('refresh')}<span>${S.checking ? te('cookies.checkingBusy') : c.signedIn == null ? te('cookies.checkNow') : te('cookies.checkAgain')}</span></button>
      <button class="btn btn-red-text push" type="button" data-act="removeCookies">${icon('trash')}<span>${te('cookies.remove')}</span></button>
    </div>` : '';
  setHtml($('#ytState'), `
    <div class="row acct">${lead}
      <div class="row-main"><div class="row-title">${esc(title)}</div>${sub ? `<div class="row-sub">${esc(sub)}</div>` : ''}</div>
      ${pill(level, pillText)}
    </div>${actions}`);
  $('#h-upload').textContent = c.present ? t('cookies.replace') : t('cookies.upload');
  renderDropzone();
}
function renderDropzone() {
  const dz = $('#cookieDrop');
  dz.classList.toggle('is-busy', S.cookieBusy);
  $('.dz-ico', dz).innerHTML = S.cookieBusy ? spinner() : icon('file');
  const touch = matchMedia('(hover: none)').matches;
  $('#dzTitle').textContent = S.cookieBusy ? t('cookies.checkingWithYoutube') : touch ? t('cookies.choose') : t('cookies.dropHere');
  $('#dzSub').innerHTML = S.cookieBusy ? te('cookies.takesSeconds')
    : touch ? te('cookies.subTouch') : th('cookies.subMouse', { choose: html(`<u>${te('cookies.chooseFile')}</u>`) });
  $('#cookieFile').disabled = S.cookieBusy;
}
function cookieError(msg, kind = 'err') {
  const box = $('#cookieErr');
  box.hidden = !msg;
  box.className = `note ${kind}`;
  $('use', box).setAttribute('href', kind === 'ok' ? '#i-check' : '#i-alert');
  $('span', box).textContent = msg || '';
}
async function uploadCookies(file) {
  cookieError('');
  if (!file || S.cookieBusy) return;
  if (file.size > 1024 * 1024) return cookieError(t('cookies.tooLarge'));
  if (!file.size) return cookieError(t('cookies.empty'));
  let text;
  try { text = await file.text(); } catch { return cookieError(t('cookies.unreadable')); }
  if (!/youtube\.com/i.test(text)) return cookieError(t('cookies.badFormat'));
  S.cookieBusy = true;
  renderDropzone();
  try {
    const r = await api('POST', '/cookies', text, { text: true, timeout: 60000 });
    text = null;
    S.status.cookies = { ...S.status.cookies, present: true, signedIn: r.signedIn ?? null, account: r.account ?? null, checkedAt: now() };
    if (r.signedIn) toast(r.account?.name ? t('cookies.savedAs', { name: r.account.name }) : t('cookies.saved'));
    else if (r.signedIn === false) cookieError(t('cookies.savedSignedOut'));
  } catch (e) {
    if (e.status === 401) return;
    if (e.code === 'format') cookieError(t('cookies.badFormat'));
    else if (e.status === 413) cookieError(t('cookies.tooLarge'));
    else cookieError(t('cookies.notUploaded', { reason: reason(e) }));
  } finally {
    S.cookieBusy = false;
    $('#cookieFile').value = '';
    renderChrome();
    if (S.route === 'youtube') renderYouTube();
  }
}
$('#cookieFile').addEventListener('change', (e) => uploadCookies(e.target.files[0]));
async function checkCookies() {
  if (S.checking) return;
  S.checking = true;
  renderYouTube();
  try {
    const r = await api('POST', '/cookies/check', undefined, { timeout: 60000 });
    S.status.cookies = { ...S.status.cookies, signedIn: r.signedIn ?? null, account: r.account ?? null, checkedAt: now() };
    if (r.signedIn) toast(r.account?.name ? t('cookies.stillSignedInAs', { name: r.account.name }) : t('cookies.stillSignedIn'));
    else toast(t('cookies.saysSignedOut'), 'err');
  } catch (e) {
    if (e.status !== 401) toast(t('cookies.couldntCheck', { reason: reason(e) }), 'err');
  } finally {
    S.checking = false;
    renderChrome();
    if (S.route === 'youtube') renderYouTube();
  }
}
async function removeCookies() {
  const ok = await confirmSheet({
    title: t('cookies.removeTitle'),
    text: t('cookies.removeText'),
    action: t('cookies.removeAction'),
  });
  if (!ok) return;
  try {
    await api('DELETE', '/cookies');
    S.status.cookies = { present: false, signedIn: null, account: null, checkedAt: null };
    cookieError('');
    toast(t('cookies.removed'));
    renderChrome();
    renderYouTube();
  } catch (e) { if (e.status !== 401) toast(t('cookies.notRemoved', { reason: reason(e) }), 'err'); }
}

// drag and drop: anywhere on the YouTube page
const drop = $('#drop'), dropzone = $('#cookieDrop');
let dragDepth = 0;
const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes('Files');
const canDrop = () => S.booted && !screens.app.hidden && S.route === 'youtube' && !sheet.open && !S.cookieBusy;
window.addEventListener('dragenter', (e) => {
  if (!hasFiles(e)) return;
  e.preventDefault();
  if (!canDrop()) return;
  dragDepth++;
  drop.hidden = false;
});
window.addEventListener('dragover', (e) => {
  if (!hasFiles(e)) return;
  e.preventDefault();
  e.dataTransfer.dropEffect = canDrop() ? 'copy' : 'none';
});
window.addEventListener('dragleave', (e) => {
  if (!hasFiles(e) || !canDrop()) return;
  dragDepth = Math.max(0, dragDepth - 1);
  if (!dragDepth) drop.hidden = true;
});
window.addEventListener('drop', (e) => {
  if (!hasFiles(e)) return;
  e.preventDefault();
  dragDepth = 0;
  drop.hidden = true;
  if (!canDrop()) return;
  const files = [...e.dataTransfer.files];
  if (files.length > 1) return cookieError(t('cookies.dropOne'));
  uploadCookies(files[0]);
});
dropzone.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); $('#cookieFile').click(); } });

// ---------- WhatsApp ----------
// the overview's row: what the link is now
function waRow(w) {
  if (w.state === 'open') {
    const linked = w.me?.name ? t('whatsapp.linkedAs', { name: w.me.name }) : t('whatsapp.linked');
    return { sub: `${linked} · ${w.syncing ? t('whatsapp.syncingLower') : t('whatsapp.chatCount', { count: Number(w.chats) || 0 })}`, level: 'ok', pillText: t('whatsapp.linked') };
  }
  if (w.state === 'linking') return { sub: t('whatsapp.codeWaits'), level: 'warn', pillText: t('whatsapp.linking') };
  if (w.state === 'connecting' || w.state === 'closed') return { sub: t('whatsapp.connectingTo'), level: 'warn', pillText: t('whatsapp.connecting') };
  return { sub: t('whatsapp.notLinked'), level: 'off', pillText: t('whatsapp.off') };
}
// the server's codes for a link that didn't start
const waError = (code) => (code === 'number' ? t('whatsapp.errorNumber')
  : code === 'linked' ? t('whatsapp.errorLinked')
  : code === 'whatsapp' ? t('whatsapp.errorNoCode') : '');
// why the link ended: the server says it in English (whatsapp.ts), the same for every browser
const waStatusError = (msg) => (msg === 'Unlinked from the phone' ? t('whatsapp.unlinkedFromPhone') : msg);
function renderWhatsApp() {
  if (S.waBusy) return;
  const w = st0().whatsapp, body = $('#waBody');
  if (!w) { setHtml(body, `<div class="note">${icon('alert')}<span>${te('whatsapp.none')}</span></div>`); return; }
  const err = w.error && w.state !== 'open' ? `<div class="note err">${icon('alert')}<span>${esc(waStatusError(w.error))}</span></div>` : '';
  if (w.state === 'linking' && w.code) {
    const c = String(w.code.code), groups = c.length === 8 ? [c.slice(0, 4), c.slice(4)] : [c];
    setHtml(body, `<section class="pairing wa-pairing" aria-labelledby="waLabel">
      <div class="pc-code">
        <p class="pc-label" id="waLabel">${te('whatsapp.codeLabel')}</p>
        <div class="big-code is-letters" aria-label="${esc(c.split('').join(' '))}">${groups.map((g) => `<span>${esc(g)}</span>`).join('')}</div>
        <p class="expiry" data-wa-countdown role="timer" aria-live="off"></p>
        <div class="pc-actions">
          <button class="btn btn-sm" type="button" data-act="waCopy">${icon('copy')}<span>${te('shared.copyCode')}</span></button>
          <button class="btn btn-sm btn-quiet" type="button" data-act="waCancel">${te('shared.cancel')}</button>
        </div>
      </div>
      <div class="pc-steps">
        <ol class="steps">
          <li>${th('whatsapp.step1', { path: html(`<b class="nowrap">${te('whatsapp.linkedDevicesPath')}</b>`) })}</li>
          <li>${th('whatsapp.step2')}</li>
          <li>${te('whatsapp.step3')}</li>
        </ol>
        <p class="waiting">${spinner()}<span>${te('whatsapp.waiting')}</span></p>
      </div>
    </section>${err}`);
    tickWa();
    return;
  }
  if (w.state === 'open' || w.state === 'connecting' || w.state === 'closed' || w.state === 'linking') {
    const open = w.state === 'open';
    const facts = [
      w.me?.number ? [t('whatsapp.number'), esc(w.me.number)] : null,
      open ? [t('whatsapp.chats'), w.syncing ? te('whatsapp.syncing') : esc(String(w.chats))] : null,
    ].filter(Boolean);
    setHtml(body, `${err}<div class="group">
      <div class="row dev-head">${waTile()}
        <div class="row-main"><div class="row-title">${open ? (w.me?.name ? te('whatsapp.linkedAs', { name: w.me.name }) : te('whatsapp.linked')) : te('whatsapp.connectingToBusy')}</div>
          <div class="row-sub">${open ? te('whatsapp.linkedText') : te('whatsapp.reaching')}</div></div>
        ${pill(open ? 'ok' : 'warn', open ? t('whatsapp.linked') : t('whatsapp.connecting'))}
      </div>
      ${facts.map(([k, v]) => `<div class="kv"><span>${esc(k)}</span><b>${v}</b></div>`).join('')}
      <div class="row-actions"><button class="btn btn-red-text push" type="button" data-act="waUnlink">${icon('close')}<span>${te('whatsapp.unlink')}</span></button></div>
    </div>`);
    return;
  }
  setHtml(body, `${err}<form class="group form-card" id="waForm" novalidate>
      <div class="field">
        <label class="field-label" for="waPhone">${te('whatsapp.numberLabel')}</label>
        <input type="tel" id="waPhone" autocomplete="tel" inputmode="tel" placeholder="+1 555 010 0000" spellcheck="false">
        <p class="field-help">${te('whatsapp.numberHelp')}</p>
      </div>
      <p class="form-err" id="waErr" role="alert"></p>
      <div class="form-actions"><button class="btn btn-primary" type="submit" id="waBtn">${icon('chat')}<span>${te('shared.getCode')}</span></button></div>
    </form>`);
}
function tickWa() {
  const cd = $('[data-wa-countdown]'), c = st0().whatsapp?.code;
  if (!cd || !c) return;
  const left = Math.max(0, Math.ceil((c.expiresAt - now()) / 1000));
  cd.textContent = left ? t('whatsapp.goodFor', { time: clock(left) }) : t('whatsapp.codeExpired');
  cd.classList.toggle('is-low', left <= 30);
}
$('#waBody').addEventListener('input', clearInvalid);
$('#waBody').addEventListener('submit', async (e) => {
  if (e.target.id !== 'waForm') return;
  e.preventDefault();
  const input = $('#waPhone'), err = $('#waErr');
  const digits = input.value.replace(/\D/g, '');
  if (digits.length < 8 || digits.length > 15) {
    input.classList.add('is-invalid');
    err.textContent = waError('number');
    return input.focus();
  }
  const btn = $('#waBtn'), was = btn.innerHTML;
  S.waBusy = true;
  btn.disabled = true;
  input.readOnly = true;
  btn.innerHTML = `${spinner()}<span>${te('shared.gettingCode')}</span>`;
  err.textContent = '';
  try {
    await api('POST', '/whatsapp/link', { phone: digits }, { timeout: 45000 });
    S.waBusy = false;
    await poll();
  } catch (x) {
    S.waBusy = false;
    if (x.status === 401) return;
    btn.disabled = false;
    input.readOnly = false;
    btn.innerHTML = was;
    err.textContent = waError(x.code) || t('shared.couldntGetCode', { reason: reason(x) });
  }
});
async function waUnlink(_b, _e, cancel = false) {
  if (!cancel) {
    const ok = await confirmSheet({
      title: t('whatsapp.unlinkTitle'),
      text: t('whatsapp.unlinkText'),
      action: t('whatsapp.unlink'),
    });
    if (!ok) return;
  }
  try {
    await api('POST', '/whatsapp/unlink', {}, { timeout: 30000 });
    if (!cancel) toast(t('whatsapp.unlinked'));
    await poll();
  } catch (x) { if (x.status !== 401) toast(t('whatsapp.notUnlinked', { reason: reason(x) }), 'err'); }
}

// ---------- device ----------
function renderDevice() {
  const a = areas(S.status), d = a.d;
  const parts = [];
  if (S.celebrate) parts.push(doneHtml());
  else if (S.pair) parts.push(pairHtml(d));
  if (d.source === 'env' && !S.celebrate) parts.push(`<div class="note info device-note">${icon('info')}<span>${th('device.envNote')}</span></div>`);
  if (d.source || !S.pair) parts.push(deviceCard(d));
  setHtml($('#deviceBody'), parts.join(''));
  tick();
}
function deviceCard(d) {
  if (!d.source) {
    return `<div class="group empty">${tileHtml('off', 'player')}
      <h3>${te('device.noRondo')}</h3>
      <p>${te('device.connectText')}</p>
      <button class="btn btn-primary" type="button" data-act="connect"${S.codeBusy ? ' disabled' : ''}>${icon('plus')}<span>${te('device.connectRondo')}</span></button></div>`;
  }
  const paired = d.source === 'paired';
  const facts = [
    [t('device.lastSeenLabel'), d.lastSeenAt ? `<time ${timeAttr(d.lastSeenAt)}>${esc(ago(d.lastSeenAt))}</time>` : te('device.notYet')],
    d.lastIp ? [t('device.ipAddress'), esc(d.lastIp)] : null,
    d.userAgent ? [t('device.userAgent'), esc(d.userAgent)] : null,
  ].filter(Boolean);
  const actions = paired
    ? `${S.pair ? '' : `<button class="btn" type="button" data-act="connect">${icon('plus')}<span>${te('device.connectDifferent')}</span></button>`}
       <button class="btn btn-red-text push" type="button" data-act="disconnect">${icon('close')}<span>${te('device.disconnect')}</span></button>`
    : S.pair ? '' : `<button class="btn btn-primary" type="button" data-act="connect">${icon('plus')}<span>${te('device.connectRondo')}</span></button>`;
  return `<div class="group">
    <div class="row dev-head">${tileHtml('device', 'player')}
      <div class="row-main"><div class="row-title">${esc(d.name || 'Rondo')}</div>
        <div class="row-sub">${paired ? (d.pairedAt ? te('device.connectedWhen', { when: when(d.pairedAt) }) : te('device.connected')) : te('device.usesApiTokenServer')}</div></div>
      ${pill(paired ? 'ok' : 'off', paired ? t('device.connected') : 'API_TOKEN')}
    </div>
    ${facts.map(([k, v]) => `<div class="kv"><span>${esc(k)}</span><b>${v}</b></div>`).join('')}
    ${actions.trim() ? `<div class="row-actions">${actions}</div>` : ''}
  </div>`;
}
function pairHtml(d) {
  const p = S.pair, code = String(p.code).padStart(6, '0');
  const notes = [];
  if (d.source === 'paired') notes.push(`<div class="note">${icon('alert')}<span>${te('pair.replaces')}</span></div>`);
  if (LOCAL) notes.push(`<div class="note">${icon('alert')}<span>${th('pair.localhost', { host: location.hostname })}</span></div>`);
  else if (!(S.status.server?.https ?? S.state?.https)) notes.push(`<div class="note">${icon('unlock')}<span>${te('shared.insecure')}</span></div>`);
  return `<section class="pairing" aria-labelledby="pcLabel">
    <div class="pc-code">
      <p class="pc-label" id="pcLabel">${te('pair.codeLabel')}</p>
      <div class="big-code${p.expired ? ' is-expired' : ''}" aria-label="${code.split('').join(' ')}"><span>${code.slice(0, 3)}</span><span>${code.slice(3)}</span></div>
      <p class="expiry" data-countdown role="timer" aria-live="off"></p>
      <div class="drain" aria-hidden="true"><span data-drain></span></div>
      <div class="pc-actions">
        ${p.expired ? '' : `<button class="btn btn-sm" type="button" data-act="copyCode">${icon('copy')}<span>${te('shared.copyCode')}</span></button>`}
        <button class="btn ${p.expired ? 'btn-primary' : 'btn-sm'}" type="button" data-act="newCode"${S.codeBusy ? ' disabled' : ''}>${icon('refresh')}<span>${te('pair.newCode')}</span></button>
        <button class="btn btn-sm btn-quiet" type="button" data-act="cancelCode">${p.expired ? te('pair.close') : te('shared.cancel')}</button>
      </div>
    </div>
    <div class="pc-steps">
      <ol class="steps">
        <li>${th('pair.step1', { menu: html(`<b class="nowrap">${te('pair.menuPath')}</b>`), dashboard: html(`<b class="nowrap">${te('pair.dashboardPath')}</b>`) })}</li>
        <li>${te('pair.step2')}
          <div class="addr"><span>${esc(ORIGIN)}</span><button class="ib ib-sm" type="button" data-act="copy" aria-label="${te('pair.copyAddress')}">${icon('copy')}</button></div></li>
        <li>${te('pair.step3')}</li>
      </ol>
      ${p.expired ? '' : `<p class="waiting">${spinner()}<span>${te('pair.waiting')}</span></p>`}
      ${notes.join('')}
    </div>
  </section>`;
}
function doneHtml() {
  return `<section class="pairing is-done" role="status">
    <div class="done-mark"><svg class="mk" viewBox="-66 -66 132 132" aria-hidden="true">${MARK_INNER}</svg><span class="done-check">${icon('check')}</span></div>
    <h2 class="done-title">${te('pair.connected')}</h2>
    <p class="done-text">${te('pair.connectedText')}</p>
    <button class="btn btn-primary" type="button" data-act="doneOk">${te('pair.done')}</button>
  </section>`;
}
async function newCode(confirmReplace) {
  if (S.codeBusy) return;
  const d = S.status.device || {};
  if (confirmReplace && d.source === 'paired') {
    const ok = await confirmSheet({
      title: t('pair.differentTitle'),
      text: t('pair.differentText'),
      action: t('shared.getCode'), danger: false,
    });
    if (!ok) return;
  }
  S.codeBusy = true;
  try {
    const r = await api('POST', '/device/code');
    S.celebrate = false;
    S.pair = { code: r.code, expiresAt: r.expiresAt, base: d.source === 'paired' ? d.pairedAt ?? null : null, total: Math.max(1000, r.expiresAt - now()) };
    if (S.route !== 'device') location.hash = '#device';
    schedulePoll(3000);
  } catch (e) {
    if (e.status !== 401) toast(t('shared.couldntGetCode', { reason: reason(e) }), 'err');
  } finally {
    S.codeBusy = false;
    renderView();
    renderChrome();
  }
}
async function cancelCode() {
  const p = S.pair;
  S.pair = null;
  renderDevice();
  if (!p || p.expired) return;
  try { await api('DELETE', '/device/code'); } catch (e) { if (e.status !== 401) toast(t('pair.couldntCancel', { reason: reason(e) }), 'err'); }
  schedulePoll();
}
async function disconnect() {
  const ok = await confirmSheet({
    title: t('device.disconnectTitle'),
    text: t('device.disconnectText'),
    action: t('device.disconnect'),
  });
  if (!ok) return;
  try {
    await api('DELETE', '/device');
    toast(t('device.disconnected'));
    await poll();
  } catch (e) { if (e.status !== 401) toast(t('device.notDisconnected', { reason: reason(e) }), 'err'); }
}
// to the clipboard; plain http on a LAN address has no Clipboard API, so the old way there
async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    return true;
  } catch {
    const ta = Object.assign(document.createElement('textarea'), { value: text });
    ta.setAttribute('readonly', '');
    ta.style.cssText = 'position:fixed;top:0;left:0;opacity:0';
    document.body.append(ta);
    ta.select();
    let ok = false;
    try { ok = document.execCommand('copy'); } catch { ok = false; }
    ta.remove();
    return ok;
  }
}
// what: 'address' (this server's) or 'code'
async function copyWith(btn, text, what) {
  if (!(await copyText(text))) return toast(what === 'address' ? t('copy.addressFailed') : t('copy.codeFailed'), 'err');
  const use = $('use', btn);
  if (use) {
    use.setAttribute('href', '#i-check');
    setTimeout(() => $('use', btn)?.setAttribute('href', '#i-copy'), 1600);
  }
  toast(what === 'address' ? t('copy.addressDone') : t('copy.codeDone'));
}
// a connect code the way it's shown and typed: 123 456
const spacedCode = (code) => { const c = String(code).padStart(6, '0'); return `${c.slice(0, 3)} ${c.slice(3)}`; };

// ---------- resolver ----------
// the server's own words for a failed test ("no formats"); yt-dlp's errors are shown as they are
const rzError = (msg) => (msg === 'no formats' ? t('resolver.noFormats') : msg);
function renderResolver() {
  const a = areas(S.status), R = S.rz;
  let upd = '';
  if (R.updating) upd = `<div class="rz-line wait">${spinner()}<span>${th('resolver.updatingElapsed', { time: html(`<span data-elapsed>${clock(Math.floor((Date.now() - R.updateStart) / 1000))}</span>`) })}</span></div><div class="progress" aria-hidden="true"></div>`;
  else if (R.update?.ok) upd = R.update.before && R.update.before === R.update.after
    ? `<div class="rz-line ok">${icon('check')}<span>${th('resolver.upToDate', { version: String(R.update.after) })}</span></div>`
    : `<div class="rz-line ok">${icon('check')}<span>${th('resolver.updated', { before: String(R.update.before || t('resolver.unknown')), after: String(R.update.after || t('resolver.unknown')) }).replace(' → ', '<span class="arrow"> → </span>')}</span></div>`;
  else if (R.update) upd = `<div class="rz-line bad">${icon('alert')}<span>${R.update.error ? th('resolver.updateFailedWhy', { error: String(R.update.error) }) : te('resolver.updateFailed')}</span></div>`;
  let test = '';
  if (R.testing) test = `<div class="rz-line wait">${spinner()}<span>${te('resolver.resolvingTest')}</span></div>`;
  else if (R.test?.ok) test = `<div class="rz-line ok">${icon('check')}<span>${th('resolver.testWorks', { ms: Number(R.test.ms || 0) })}</span></div>`;
  else if (R.test) test = `<div class="rz-line bad">${icon('alert')}<span>${R.test.error ? th('resolver.testFailedWhy', { error: rzError(String(R.test.error)) }) : te('resolver.testFailed')}</span></div>`;
  const out = st0().updates?.ytdlp ? ` · ${th('resolver.versionOut', { version: String(st0().updates.ytdlp.version) })}` : '';
  setHtml($('#resolverRows'), `
    <div class="row">${tileHtml('resolver', 'bolt')}
      <div class="row-main"><div class="row-title">yt-dlp</div><div class="row-sub ver">${te('resolver.version', { version: a.y.version || t('resolver.unknown') })}${out}</div></div>
      ${pill(a.rz.level, a.rz.pill)}
    </div>
    <div class="row rz">${tileHtml('settings', 'refresh')}
      <div class="row-main"><div class="row-title">${te('resolver.updateTitle')}</div><div class="row-sub">${te('resolver.updateText')}</div></div>
      <button class="btn${R.updating ? ' is-busy' : ''}" type="button" data-act="updateYtdlp"${R.updating ? ' disabled' : ''}>${R.updating ? te('resolver.updating') : te('resolver.updateNow')}</button>
      ${upd ? `<div class="rz-res" aria-live="polite">${upd}</div>` : ''}
    </div>
    <div class="row rz">${tileHtml('ok', 'play')}
      <div class="row-main"><div class="row-title">${te('resolver.testTitle')}</div><div class="row-sub">${te('resolver.testText')}</div></div>
      <button class="btn${R.testing ? ' is-busy' : ''}" type="button" data-act="testYtdlp"${R.testing ? ' disabled' : ''}>${R.testing ? te('resolver.testing') : R.test ? te('resolver.testAgain') : te('resolver.testButton')}</button>
      ${test ? `<div class="rz-res" aria-live="polite">${test}</div>` : ''}
    </div>`);
}
async function updateYtdlp() {
  const R = S.rz;
  if (R.updating) return;
  R.updating = true;
  R.updateStart = Date.now();
  R.update = null;
  renderResolver();
  try {
    R.update = await api('POST', '/ytdlp/update', undefined, { timeout: 180000 });
    if (R.update?.ok) {
      const changed = R.update.before !== R.update.after;
      if (changed) R.test = null; // an old test result says nothing about the new version
      toast(changed ? t('resolver.updatedToast') : t('resolver.upToDateToast'));
    }
  } catch (e) {
    if (e.status === 401) { R.updating = false; return; }
    R.update = { ok: false, error: e.code === 'timeout' ? t('resolver.noAnswer') : reason(e).replace(/\.$/, '') };
  }
  R.updating = false;
  await poll();
  if (S.route === 'resolver') renderResolver();
}
async function testYtdlp() {
  const R = S.rz;
  if (R.testing) return;
  R.testing = true;
  R.test = null;
  renderResolver();
  try { R.test = await api('POST', '/ytdlp/test', undefined, { timeout: 90000 }); } catch (e) {
    if (e.status === 401) { R.testing = false; return; }
    R.test = { ok: false, error: reason(e).replace(/\.$/, '') };
  }
  R.testing = false;
  await poll();
  if (S.route === 'resolver') renderResolver();
}

// ---------- activity ----------
const logList = $('#logList');
async function loadLogs() {
  const btn = $('#logsRefresh');
  if (!S.logs) setHtml(logList, '<div class="log-skel"></div><div class="log-skel"></div><div class="log-skel"></div>');
  btn.disabled = true;
  btn.classList.add('is-busy');
  try {
    const r = await api('GET', '/logs');
    S.logs = Array.isArray(r?.lines) ? r.lines : [];
    renderLogs();
  } catch (e) {
    if (e.status !== 401 && !S.logs) setHtml(logList, `<div class="row-empty">${te('activity.couldntLoad', { reason: reason(e) })}</div>`);
    else if (e.status !== 401) toast(t('activity.couldntRefresh', { reason: reason(e) }), 'err');
  } finally {
    btn.disabled = false;
    btn.classList.remove('is-busy');
  }
}
$('#logsRefresh').addEventListener('click', loadLogs);
function dayLabel(ms) {
  const d = new Date(ms), today = new Date(now());
  const start = (x) => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime();
  const diff = Math.round((start(today) - start(d)) / 86400000);
  return diff === 0 ? t('activity.today') : diff === 1 ? t('activity.yesterday') : dfWeekday.format(ms);
}
function renderLogs() {
  const lines = S.logs;
  if (!lines.length) {
    setHtml(logList, `<div class="empty">${tileHtml('ok', 'check')}<h3>${te('activity.none')}</h3><p>${te('activity.noneText')}</p></div>`);
    return;
  }
  // the lines are the server's log, in English as it wrote them
  let out = '', last = '';
  for (const l of lines) {
    const label = dayLabel(l.time);
    if (label !== last) { out += `<div class="lhdr">${esc(label)}</div>`; last = label; }
    const recent = now() - l.time < 3600000;
    const level = l.level === 'error' ? 'error' : 'warn';
    out += `<div class="log ${level}">${icon('alert')}
      <div class="log-msg"><span class="sr-only">${level === 'error' ? te('activity.error') : te('activity.warning')} </span>${esc(l.msg)}</div>
      <time class="log-time" ${timeAttr(l.time)}>${esc(recent ? ago(l.time) : dfTime.format(l.time))}</time></div>`;
  }
  setHtml(logList, out);
}

// ---------- admin account ----------
const pwForm = $('#pwForm'), pwErr = $('#pwErr'), pwBtn = $('#pwBtn');
bindStrength($('#pwNext'), $('#pwStrength'));
pwForm.addEventListener('input', (e) => { clearInvalid(e); pwErr.textContent = ''; });
function pwFail(msg, field) {
  pwErr.textContent = msg;
  if (field) { field.classList.add('is-invalid'); field.focus(); }
}
pwForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  if (pwBtn.disabled) return;
  const cur = $('#pwCurrent'), next = $('#pwNext'), next2 = $('#pwNext2');
  if (!cur.value) return pwFail(t('account.enterCurrent'), cur);
  if ([...next.value].length < MIN_PASSWORD) return pwFail(t('account.newNeedsAtLeast', { count: MIN_PASSWORD }), next);
  if (next.value !== next2.value) return pwFail(t('account.noMatch'), next2);
  pwBtn.disabled = true;
  pwBtn.textContent = t('account.changing');
  try {
    await api('POST', '/password', { current: cur.value, next: next.value });
    pwForm.reset();
    next.dispatchEvent(new Event('input'));
    toast(t('account.changed'));
  } catch (err) {
    if (err.status === 403) pwFail(t('account.wrongCurrent'), cur);
    else if (err.status === 400) pwFail(t('account.newNeedsAtLeast', { count: MIN_PASSWORD }), next);
    else if (err.status === 429) pwFail(t('account.tooMany', { count: Number(err.data?.retryAfterSec ?? 60) }), cur);
    else if (err.status !== 401) pwFail(t('account.notChanged', { reason: reason(err) }));
  } finally {
    pwBtn.disabled = false;
    pwBtn.textContent = t('account.changePassword');
  }
});
$('#signOut').addEventListener('click', async () => {
  try { await api('POST', '/logout', undefined, { auth: false }); } catch { /* signed out locally anyway */ }
  S.logs = null;
  showSignIn(t('signin.signedOut'));
});

// ---------- actions ----------
const ACTIONS = {
  connect: () => newCode(true),
  newCode: () => newCode(false),
  cancelCode,
  disconnect,
  copy: (b) => copyWith(b, ORIGIN, 'address'),
  copyCode: (b) => S.pair && copyWith(b, spacedCode(S.pair.code), 'code'),
  doneOk: () => { S.celebrate = false; renderDevice(); renderChrome(); },
  checkCookies,
  removeCookies,
  updateYtdlp,
  testYtdlp,
  tvStart,
  tvCancel,
  tvSignOut,
  tvCopy: (b) => copyWith(b, st0().tv?.pending?.code || '', 'code'),
  waCopy: (b) => copyWith(b, st0().whatsapp?.code?.code || '', 'code'),
  waCancel: (b, e) => waUnlink(b, e, true),
  waUnlink,
};
document.addEventListener('click', (e) => {
  const b = e.target.closest('[data-act]');
  if (b && !b.disabled) ACTIONS[b.dataset.act]?.(b, e);
});

// ---------- sheet (confirmations) ----------
const sheet = $('#sheet');
sheet.addEventListener('click', (e) => { if (e.target === sheet) sheet.close(); });
function confirmSheet({ title, text, action, danger = true }) {
  return new Promise((resolve) => {
    sheet.innerHTML = `<div class="sheet-body"><div class="sheet-grab" aria-hidden="true"></div><h2>${esc(title)}</h2><p>${esc(text)}</p>
      <div class="sheet-actions"><button class="btn" type="button" data-r="0">${te('shared.cancel')}</button><button class="btn ${danger ? 'btn-danger' : 'btn-primary'}" type="button" data-r="1">${esc(action)}</button></div></div>`;
    if (!sheet.open) sheet.showModal();
    $('[data-r="0"]', sheet).focus();
    const onClick = (e) => { const b = e.target.closest('[data-r]'); if (b) finish(b.dataset.r === '1'); };
    const finish = (v) => { sheet.removeEventListener('click', onClick); sheet.removeEventListener('close', onClose); if (sheet.open) sheet.close(); resolve(v); };
    const onClose = () => finish(false);
    sheet.addEventListener('click', onClick);
    sheet.addEventListener('close', onClose);
  });
}

// ---------- updating this server ----------
// With the updater container beside the server (docs/server.md), Update downloads the new server
// image and restarts the server on it (server/src/serverUpdate.ts, server/src/updater/). The
// restart signs everyone out, so the page watches /state, which answers without signing in, until
// the server comes back with a new version, then reloads.
async function loadServerUpdate() {
  S.serverUpdateAt = Date.now();
  try { S.serverUpdate = await api('GET', '/server/update', undefined, { timeout: 6000 }); } catch { S.serverUpdate = null; }
  if (S.route === 'overview') renderOverview();
}
document.addEventListener('click', (e) => {
  if (!e.target.closest('a[href="#server-update"]')) return;
  e.preventDefault();
  startServerUpdate();
});
async function startServerUpdate() {
  const next = st0().updates?.server?.version;
  const before = st0().server?.version;
  if (!next) return;
  const ok = await confirmSheet({
    title: t('update.title', { version: String(next) }),
    text: t('update.text'),
    action: t('overview.update'),
    danger: false,
  });
  if (!ok) return;
  try { await api('POST', '/server/update', undefined, { auth: false }); } catch (e) {
    return toast(e.status === 409 ? t('update.busy') : t('update.couldntStart', { reason: reason(e) }), 'err');
  }
  S.updatingServer = true;
  clearTimeout(pollTimer);
  sheet.innerHTML = `<div class="sheet-body"><div class="sheet-grab" aria-hidden="true"></div><h2>${te('update.updating', { version: String(next) })}</h2>
    <p id="suText">${te('update.downloading')}</p><p class="muted">${te('update.carriesOn')}</p></div>`;
  if (!sheet.open) sheet.showModal();
  const started = Date.now();
  const finish = (ok, msg) => {
    S.updatingServer = false;
    if (ok) { if (sheet.open) sheet.close(); toast(t('update.done', { version: String(next) })); setTimeout(() => location.reload(), 1200); return; }
    sheet.innerHTML = `<div class="sheet-body"><div class="sheet-grab" aria-hidden="true"></div><h2>${te('update.failedTitle')}</h2><p>${esc(msg)}</p>
      <p class="muted">${te('update.keepsOld')}</p><div class="sheet-actions"><button class="btn btn-primary" type="button" data-r="ok">${te('update.ok')}</button></div></div>`;
    $('[data-r="ok"]', sheet).addEventListener('click', () => sheet.close(), { once: true });
    poll();
  };
  const tick = async () => {
    const line = $('#suText', sheet);
    try {
      const su = await api('GET', '/server/update', undefined, { auth: false, timeout: 5000 });
      if (su.state === 'failed') return finish(false, su.message || t('update.gaveUp')); // the updater's own words, as they came
      if (su.state === 'current') return finish(false, t('update.alreadyNewest'));
      if (line) line.textContent = su.state === 'restarting' ? t('update.restarting') : t('update.downloading');
    } catch {
      if (line) line.textContent = t('update.restarting');
    }
    try {
      const st = await api('GET', '/state', undefined, { auth: false, timeout: 5000 });
      if (st.version && st.version !== before) return finish(true);
    } catch { /* still restarting */ }
    if (Date.now() - started > 10 * 60000) return finish(false, t('update.tooLong'));
    setTimeout(tick, 3000);
  };
  setTimeout(tick, 3000);
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
boot();
