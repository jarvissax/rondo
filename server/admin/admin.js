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
const dfDay = new Intl.DateTimeFormat('en-US', { month: 'short', day: 'numeric' });
const dfDayYear = new Intl.DateTimeFormat('en-US', { month: 'short', day: 'numeric', year: 'numeric' });
const dfTime = new Intl.DateTimeFormat('en-US', { hour: 'numeric', minute: '2-digit' });
const dfWeekday = new Intl.DateTimeFormat('en-US', { weekday: 'long', month: 'short', day: 'numeric' });
const dfFull = new Intl.DateTimeFormat('en-US', { dateStyle: 'medium', timeStyle: 'medium' });
const day = (t) => (new Date(t).getFullYear() === new Date().getFullYear() ? dfDay : dfDayYear).format(t);
const when = (t) => `${day(t)} at ${dfTime.format(t)}`;
function ago(t) {
  const s = Math.round((now() - t) / 1000);
  if (s < 45) return 'just now';
  const m = Math.round(s / 60);
  if (m < 60) return `${m} min ago`;
  const h = Math.round(m / 60);
  if (h < 24) return `${h} h ago`;
  const d = Math.round(h / 24);
  if (d === 1) return 'yesterday';
  if (d < 7) return `${d} days ago`;
  return day(t);
}
function uptime(sec) {
  const m = Math.floor(sec / 60), h = Math.floor(m / 60), d = Math.floor(h / 24);
  if (d) return `${d} ${d === 1 ? 'day' : 'days'}${h % 24 ? ` ${h % 24} h` : ''}`;
  if (h) return `${h} h${m % 60 ? ` ${m % 60} min` : ''}`;
  if (m) return `${m} min`;
  return 'less than a minute';
}
const clock = (sec) => `${Math.floor(sec / 60)}:${String(sec % 60).padStart(2, '0')}`;
const timeAttr = (t) => `datetime="${new Date(t).toISOString()}" title="${esc(dfFull.format(t))}"`;
const signedInAs = (a) => (a?.name ? `Signed in as ${a.name}${a.handle ? ` (${a.handle})` : ''}` : 'Signed in');
const ORIGIN = location.origin;
const LOCAL = ['localhost', '127.0.0.1', '[::1]', '::1'].includes(location.hostname);

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
const reason = (e) => (e?.code === 'timeout' ? 'The server took too long to answer.'
  : e?.status === 0 ? 'Can’t reach the server.'
  : e?.code ? `The server said “${e.code}”.` : `Something went wrong (${e?.status ?? 'error'}).`);

// ---------- screens ----------
const screens = { splash: $('#splash'), offline: $('#offline'), setup: $('#setup'), signin: $('#signin'), app: $('#app') };
function showScreen(name) {
  for (const [k, el] of Object.entries(screens)) el.hidden = k !== name;
  if (name !== 'app') { stopPolling(); sheet.open && sheet.close(); }
}
function markInsecure(https) { $$('[data-insecure]').forEach((el) => { el.hidden = !!https; }); }

let offlineTimer = 0;
function showOffline(e) {
  showScreen('offline');
  $('#offlineText').textContent = e?.status
    ? `The server answered with an error (${e.code || e.status}). Check its log with docker logs y1.`
    : 'Check that the Rondo server is running and that this address is right.';
  $('#offlineNote').textContent = 'Trying again every few seconds.';
  clearTimeout(offlineTimer);
  offlineTimer = setTimeout(() => { if (!document.hidden) boot(); }, 5000);
}
$('#offlineRetry').addEventListener('click', () => { $('#offlineNote').textContent = 'Trying…'; boot(); });

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
  if (!n) return { level: 0, text: `At least ${MIN_PASSWORD} characters` };
  if (n < MIN_PASSWORD) { const left = MIN_PASSWORD - n; return { level: 1, text: `Too short: ${left} more ${left === 1 ? 'character' : 'characters'}` }; }
  const classes = [/[a-z]/, /[A-Z]/, /\d/, /[^A-Za-z0-9]/].filter((r) => r.test(pw)).length;
  const unique = new Set(pw.toLowerCase()).size;
  const guessable = unique <= 4 || /^\d+$/.test(pw) || /^(?:password|qwerty|letmein|admin|welcome|rondo|123456|abcdef)/i.test(pw)
    || /(?:0123|1234|2345|3456|4567|5678|6789|abcd|bcde|cdef)/i.test(pw) && unique < 8;
  if (guessable) return { level: 2, text: 'Easy to guess: try a few random words' };
  let score = Math.max(0, classes - 1);
  if (n >= 14) score++;
  if (n >= 18) score++;
  if (n >= 24) score++;
  if (unique < n / 3) score = Math.min(score, 1);
  if (score <= 1) return { level: 2, text: 'Okay: a longer phrase is safer' };
  if (score <= 3) return { level: 3, text: 'Good' };
  return { level: 4, text: 'Strong' };
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
  eye.setAttribute('aria-label', show ? 'Hide password' : 'Show password');
  $('use', eye).setAttribute('href', show ? '#i-eye-off' : '#i-eye');
  input.focus();
});
const clearInvalid = (e) => { if (e.target.matches('input')) e.target.classList.remove('is-invalid'); };

// ---------- setup ----------
const setupForm = $('#setupForm'), setupCode = $('#setupCode'), setupBox = $('#setupCodeBox');
const setupPw = $('#setupPw'), setupPw2 = $('#setupPw2'), setupErr = $('#setupErr'), setupBtn = $('#setupBtn');
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
  if (code.length !== 6) return setupFail('Enter all 6 digits of the setup code.', 'code');
  if ([...pw].length < MIN_PASSWORD) return setupFail(`The password needs at least ${MIN_PASSWORD} characters.`, setupPw);
  if (pw !== setupPw2.value) return setupFail('The passwords don’t match.', setupPw2);
  setupErr.textContent = '';
  setupBtn.disabled = true;
  setupBtn.textContent = 'Setting up…';
  $('#setupMark').classList.add('is-turning');
  try {
    await api('POST', '/setup', { code, password: pw }, { auth: false });
    setupForm.reset();
    setupCode.dataset.len = 0;
    await boot();
    toast('Your server is set up');
  } catch (err) {
    if (err.status === 403) { setupCode.value = ''; setupCode.dataset.len = 0; renderSetupCode(); setupFail('That code didn’t match. Use the code from the newest “Rondo setup” line: it changes when the server restarts.', 'code'); }
    else if (err.status === 400) setupFail(`The password needs at least ${MIN_PASSWORD} characters.`, setupPw);
    else if (err.status === 409) { setupFail('This server is already set up. Sign in with its admin password.'); setTimeout(boot, 1600); }
    else if (err.status === 429) setupFail(`Too many tries. Wait ${err.data?.retryAfterSec ?? 60} seconds and try again.`);
    else setupFail(err.status === 0 ? 'Can’t reach the server. Try again.' : `Setup didn’t work. ${reason(err)}`);
  } finally {
    setupBtn.disabled = false;
    setupBtn.textContent = 'Set up and sign in';
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
  showSignIn(S.booted ? 'Your session ended. Sign in again.' : '');
  // a server that lost its config wants setup again
  api('GET', '/state', undefined, { auth: false }).then((st) => { S.state = st; markInsecure(st.https); if (st.setup) showSetup(); }).catch(() => {});
}
function startWait(sec) {
  waitUntil = Date.now() + sec * 1000;
  signinBtn.disabled = true;
  signinErr.className = 'form-err';
  signinErr.textContent = 'Too many wrong passwords. Wait a moment, then try again.';
  const tickWait = () => {
    const left = Math.ceil((waitUntil - Date.now()) / 1000);
    if (left <= 0) {
      clearInterval(waitTimer);
      waitUntil = 0;
      signinBtn.disabled = false;
      signinBtn.textContent = 'Sign in';
      signinErr.className = 'form-err is-quiet';
      signinErr.textContent = 'You can try again now.';
      return;
    }
    signinBtn.textContent = `Try again in ${clock(left)}`;
  };
  clearInterval(waitTimer);
  tickWait();
  waitTimer = setInterval(tickWait, 1000);
}
signinForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  if (signinBtn.disabled) return;
  const pw = signinPw.value;
  if (!pw) { signinErr.className = 'form-err'; signinErr.textContent = 'Enter the admin password.'; return signinPw.focus(); }
  signinBtn.disabled = true;
  signinBtn.textContent = 'Signing in…';
  $('#signinMark').classList.add('is-turning');
  let wait = 0;
  try {
    await api('POST', '/login', { password: pw }, { auth: false });
    signinErr.textContent = '';
    signinPw.value = '';
    await boot();
  } catch (err) {
    signinErr.className = 'form-err';
    if (err.status === 403) { signinErr.textContent = 'That password isn’t right.'; signinPw.classList.add('is-invalid'); signinPw.select(); }
    else if (err.status === 429) wait = Math.max(1, Number(err.data?.retryAfterSec) || 60);
    else signinErr.textContent = err.status === 0 ? 'Can’t reach the server. Try again.' : `Sign-in didn’t work. ${reason(err)}`;
  } finally {
    $('#signinMark').classList.remove('is-turning');
    signinBtn.disabled = false;
    signinBtn.textContent = 'Sign in';
    if (wait) startWait(wait);
  }
});

// ---------- navigation ----------
const NAV = [
  { id: 'overview', label: 'Overview', short: 'Overview', tile: 'rondo' },
  { id: 'youtube', label: 'YouTube account', short: 'YouTube', tile: 'youtube', glyph: 'account' },
  { id: 'device', label: 'Device', short: 'Device', tile: 'device', glyph: 'player' },
  { id: 'resolver', label: 'Resolver', short: 'Resolver', tile: 'resolver', glyph: 'bolt' },
  { id: 'activity', label: 'Activity', short: 'Activity', tile: 'activity', glyph: 'pulse' },
  { id: 'admin', label: 'Admin account', short: 'Admin', tile: 'settings', glyph: 'lock' },
];
const tileHtml = (tile, glyph, extra = '') => `<span class="tile t-${tile}${extra}">${glyph ? icon(glyph) : markTile}</span>`;
// the official YouTube logo (28.57 × 20 grid) on a white app-icon tile, as on the Rondo
const ytTile = () => '<span class="tile t-brand"><svg class="brand-logo" viewBox="0 0 28.57 20" aria-hidden="true"><path d="M27.9727 3.12324C27.6435 1.89323 26.6768 .926623 25.4468 .597366C23.2197 0 14.285 0 14.285 0C14.285 0 5.35042 0 3.12323 .597366C1.89323 .926623 .926623 1.89323 .597366 3.12324C0 5.35042 0 10 0 10C0 10 0 14.6496 .597366 16.8768C.926623 18.1068 1.89323 19.0734 3.12323 19.4026C5.35042 20 14.285 20 14.285 20C14.285 20 23.2197 20 25.4468 19.4026C26.6768 19.0734 27.6435 18.1068 27.9727 16.8768C28.5701 14.6496 28.5701 10 28.5701 10C28.5701 10 28.5677 5.35042 27.9727 3.12324Z" fill="#f00"/><path d="M11.4253 14.2854L18.8477 10.0004L11.4253 5.71533Z" fill="#fff"/></svg></span>';
function buildNav() {
  $('#nav').innerHTML = NAV.map((n) => `<a href="#${n.id}" data-id="${n.id}">${tileHtml(n.tile, n.glyph)}<span>${n.label}</span><span class="nav-badge"></span>${icon('chevron', 'chev')}</a>`).join('');
  $('#tabs').innerHTML = NAV.map((n) => `<a class="tab" href="#${n.id}" data-id="${n.id}"><span class="tab-wrap">${tileHtml(n.tile, n.glyph)}<span class="nav-badge"></span></span><span>${n.short}</span></a>`).join('');
}
function route(force = false) {
  const id = location.hash.slice(1);
  const next = NAV.some((n) => n.id === id) ? id : 'overview';
  const changed = next !== S.route || $(`#view-${next}`).hidden;
  S.route = next;
  for (const n of NAV) $(`#view-${n.id}`).hidden = n.id !== next;
  $$('#nav a, #tabs a').forEach((a) => (a.dataset.id === next ? a.setAttribute('aria-current', 'page') : a.removeAttribute('aria-current')));
  document.title = next === 'overview' ? 'Rondo server' : `${NAV.find((n) => n.id === next).label} · Rondo server`;
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
const pollEvery = () => (!S.online ? 5000 : (S.pair && !S.pair.expired) || S.status?.tv?.pending ? 3000 : 10000);
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
  if (polling) return;
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
  if (S.status?.tv?.pending && st.tv?.signedIn && !S.status.tv.signedIn) toast('Signed in to YouTube');
  S.status = st;
  S.statusAt = Date.now();
  const d = st.device || {};
  const pairedAt = d.source === 'paired' ? d.pairedAt ?? null : null;
  if (S.pair) {
    if (pairedAt && pairedAt !== S.pair.base) {
      S.pair = null;
      S.celebrate = true;
      if (S.route !== 'device') toast('Your Rondo is connected');
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
    cd.textContent = S.pair.expired ? 'This code expired. Get a new one.' : `Expires in ${clock(left)}`;
    cd.classList.toggle('is-low', !S.pair.expired && left <= 60);
    const drain = $('[data-drain]');
    if (drain) drain.style.setProperty('--p', S.pair.expired ? 0 : Math.min(1, (left * 1000) / Math.max(S.pair.total || CODE_MS, left * 1000)).toFixed(4));
  }
  tickTv();
  const el = $('[data-elapsed]');
  if (el && S.rz.updating) el.textContent = clock(Math.floor((Date.now() - S.rz.updateStart) / 1000));
}

// ---------- what's ok, what isn't ----------
const RANK = { bad: 3, warn: 2, info: 1, off: 1, ok: 0 };
const worst = (...xs) => xs.reduce((a, b) => (RANK[b] > RANK[a] ? b : a), 'ok');
function areas(st) {
  const c = st.cookies || {}, d = st.device || {}, y = st.ytdlp || {}, s = st.server || {}, tv = st.tv || {};
  const https = s.https ?? S.state?.https ?? false;
  const cookiesOk = c.present && c.signedIn !== false;
  const yt = tv.signedIn
    ? { level: c.present && c.signedIn === false ? 'warn' : 'ok', pill: 'Signed in', node: 'Signed in', title: signedInAs(tv.account), sub: c.present && c.signedIn === false ? 'With a code. cookies.txt is signed out: YouTube Music home and history are generic.' : 'With a code' }
    : !c.present
    ? { level: 'off', pill: 'Signed out', node: 'Signed out', title: 'Not signed in', sub: 'Search and playback still work.' }
    : !c.present
    ? { level: 'warn', pill: 'No cookies', node: 'No cookies', title: 'No cookies', sub: 'YouTube Music home, library and history need them.' }
    : c.signedIn === false
      ? { level: 'bad', pill: 'Signed out', node: 'Signed out', title: 'Signed out: upload new cookies', sub: c.checkedAt ? `Checked ${ago(c.checkedAt)}` : 'Google ended the session these cookies belong to.' }
      : c.signedIn === true
        ? { level: 'ok', pill: 'Signed in', node: 'Signed in', title: signedInAs(c.account), sub: c.checkedAt ? `Checked ${ago(c.checkedAt)}` : '' }
        : { level: 'off', pill: 'Not checked', node: 'Checking', title: 'Cookies uploaded', sub: 'Not checked yet. The server checks every 15 minutes.' };
  yt.detail = tv.signedIn ? `${signedInAs(tv.account)} · with a code`
    : !c.present ? 'Not signed in. Search and playback still work.'
    : c.signedIn === false ? 'Signed out: upload new cookies.'
    : c.signedIn === true ? `${signedInAs(c.account)}${c.checkedAt ? ` · checked ${ago(c.checkedAt)}` : ''}`
    : 'Cookies uploaded, not checked yet.';
  const seen = d.lastSeenAt ? `Last seen ${ago(d.lastSeenAt)}` : 'Not seen yet';
  const dev = d.source === 'paired'
    ? { level: 'ok', pill: 'Connected', node: d.lastSeenAt ? `Seen ${ago(d.lastSeenAt)}` : 'Connected', title: 'Rondo connected', sub: seen }
    : d.source === 'env'
      ? { level: 'info', pill: 'API_TOKEN', node: d.lastSeenAt ? `Seen ${ago(d.lastSeenAt)}` : 'API_TOKEN', title: 'Rondo uses API_TOKEN', sub: `${seen} · Connect it here to give it its own token.` }
      : { level: 'warn', pill: 'Not connected', node: 'Not connected', title: 'No Rondo connected', sub: 'Connect one with a code from the Device page.' };
  dev.detail = d.source === 'paired' ? `Rondo connected · ${seen.toLowerCase()}`
    : d.source === 'env' ? `Uses API_TOKEN from the environment · ${seen.toLowerCase()}`
    : 'No Rondo connected.';
  const rz = y.lastResolveOk === true
    ? { level: 'ok', pill: 'Working', sub: 'The last playback check worked.' }
    : y.lastResolveOk === false
      ? { level: 'bad', pill: 'Failing', sub: 'The last playback check failed. Update yt-dlp.' }
      : { level: 'off', pill: 'Not checked', sub: 'Nothing has played since the server started.' };
  const conn = https
    ? { level: 'ok', pill: 'HTTPS', sub: 'Encrypted: passwords, tokens and music travel over HTTPS.' }
    : { level: 'warn', pill: 'Not encrypted', sub: 'Plain HTTP: use it only on your home network.' };
  return { yt, dev, rz, conn, https, c, d, y, s };
}
const pill = (level, text) => `<span class="pill ${level === 'info' ? 'off' : level}">${esc(text)}</span>`;

// ---------- chrome: connection, version, badges ----------
function connHtml(https) {
  return https
    ? `<span class="conn">${icon('lock')}<span>HTTPS</span></span>`
    : `<span class="conn is-plain">${icon('unlock')}<span>Not encrypted</span></span>`;
}
function renderChrome() {
  if (!S.status) return;
  const a = areas(S.status);
  setHtml($('#topStatus'), connHtml(a.https));
  const up = (a.s.uptimeSec ?? 0) + Math.floor((Date.now() - S.statusAt) / 1000);
  setHtml($('#sideFoot'), `${connHtml(a.https)}<small>Rondo server ${esc(a.s.version || '')} · up ${uptime(up)}</small>`);
  const badges = { youtube: a.yt.level, device: a.dev.level, resolver: a.rz.level };
  $$('.nav-badge').forEach((b) => {
    const level = badges[b.closest('[data-id]').dataset.id];
    const html = level === 'bad' || level === 'warn'
      ? `<span class="dot-badge ${level}" role="img" aria-label="${level === 'bad' ? 'Problem' : 'Needs attention'}"></span>` : '';
    setHtml(b, html);
  });
}

// ---------- overview ----------
function renderOverview() {
  const a = areas(S.status);
  const level = worst(a.yt.level === 'off' ? 'ok' : a.yt.level, a.dev.level === 'info' ? 'ok' : a.dev.level, a.rz.level === 'off' ? 'ok' : a.rz.level);
  const todo = [];
  if (a.rz.level === 'bad') todo.push(['bad', 'The last playback check failed. Update yt-dlp, then test playback again.', '#resolver', 'Open resolver']);
  if (a.yt.level === 'bad') todo.push(['bad', 'YouTube signed the server out. Upload new cookies.', '#youtube', 'Upload cookies']);
  if (a.yt.level === 'off' && !st0().cookies?.present && !st0().tv?.signedIn) todo.push(['info', 'Signed out. Sign in with a code for your home, subscriptions, history and playlists.', '#youtube', 'Sign in']);
  if (a.yt.level === 'warn') todo.push(['warn', 'cookies.txt is signed out: YouTube Music home and history are generic. Upload new cookies.', '#youtube', 'Upload cookies']);
  if (a.dev.level === 'warn') todo.push(['warn', S.pair && !S.pair.expired ? 'A connect code is waiting for your Rondo.' : 'Connect your Rondo so it can stream from this server.', '#device', S.pair && !S.pair.expired ? 'Show code' : 'Connect']);
  if (a.dev.level === 'info') todo.push(['info', 'Your Rondo uses API_TOKEN from the server’s environment. Connect it here to give it its own token.', '#device', 'Connect']);
  const problems = todo.filter((t) => t[0] !== 'info').length;
  // a fresh server with nothing broken: the two setup steps
  const fresh = !a.c.present && !a.d.source && problems === 2;
  const title = level === 'ok' ? 'Everything works'
    : fresh ? 'Two steps left'
    : `${problems} ${problems === 1 ? 'thing needs' : 'things need'} attention`;
  const sub = level === 'ok'
    ? 'Your Rondo streams YouTube Music and YouTube through this server.'
    : level === 'bad' ? 'Songs and videos may not play until this is fixed.'
    : fresh ? 'Add a YouTube account and connect your Rondo, and it’s ready to stream.'
    : 'Your Rondo can stream, with a few gaps until this is done.';

  const link = (lv) => {
    const dot = lv === 'ok' ? icon('check') : lv === 'bad' || lv === 'warn' ? icon('alert') : '';
    return `<div class="link ${lv}" aria-hidden="true"><span class="link-dot">${dot}</span></div>`;
  };
  const devLevel = a.d.source ? 'ok' : 'off';
  const ytLink = worst(a.yt.level === 'off' ? 'ok' : a.yt.level, a.rz.level === 'off' ? 'ok' : a.rz.level);
  const ytNode = a.rz.level === 'bad' ? { level: 'bad', text: 'Playback failing' } : { level: a.yt.level === 'off' ? '' : a.yt.level, text: a.yt.node };
  const chain = `
    <div class="chain" role="img" aria-label="Rondo: ${esc(a.d.source ? 'connected' : 'not connected')}. This server: ${a.https ? 'HTTPS' : 'not encrypted'}. YouTube: ${esc(ytNode.text)}.">
      <div class="node">${tileHtml(a.d.source ? 'device' : 'off', 'player')}<span class="node-name">Rondo</span><span class="node-state ${a.d.source ? '' : 'warn'}">${esc(a.dev.node)}</span></div>
      ${link(devLevel)}
      <div class="node">${tileHtml('rondo')}<span class="node-name">This server</span><span class="node-state ${a.https ? '' : 'warn'}">${a.https ? 'HTTPS' : 'Not encrypted'}</span></div>
      ${link(ytLink)}
      <div class="node">${ytTile()}<span class="node-name">YouTube</span><span class="node-state ${ytNode.level}">${esc(ytNode.text)}</span></div>
    </div>`;
  const sum = $('#sum');
  sum.className = `sum ${level === 'ok' ? '' : level}`;
  setHtml(sum, `
    <div class="sum-text">
      <h2 class="sum-title">${title}</h2>
      <p class="sum-sub">${sub}</p>
    </div>
    ${todo.length ? `<ul class="todo">${todo.map(([lv, text, href, label]) => `<li class="${lv}">${icon(lv === 'info' ? 'info' : 'alert')}<span>${esc(text)}</span><a class="btn btn-sm" href="${href}">${label}</a></li>`).join('')}</ul>` : ''}
    ${chain}`);

  const up = (a.s.uptimeSec ?? 0) + Math.floor((Date.now() - S.statusAt) / 1000);
  const row = ({ href, tile, glyph, title, sub, level, pillText, extra = '' }) => {
    const tag = href ? 'a' : 'div';
    return `<${tag} class="row"${href ? ` href="${href}"` : ''}>${tileHtml(tile, glyph, extra)}
      <div class="row-main"><div class="row-title">${esc(title)}</div>${sub ? `<div class="row-sub">${esc(sub)}</div>` : ''}</div>
      <div class="row-end">${pill(level, pillText)}${href ? icon('chevron', 'row-chev') : ''}</div></${tag}>`;
  };
  setHtml($('#statusRows'), [
    row({ tile: 'rondo', title: 'Server', sub: `Rondo server ${a.s.version || ''} · up ${uptime(up)}`, level: 'ok', pillText: 'Running' }),
    row({ tile: a.https ? 'ok' : 'warn', glyph: a.https ? 'lock' : 'unlock', title: 'Connection', sub: a.conn.sub, level: a.conn.level, pillText: a.conn.pill }),
    row({ href: '#resolver', tile: 'resolver', glyph: 'bolt', title: 'Resolver', sub: `yt-dlp ${a.y.version || '(version unknown)'}. ${a.rz.sub}`, level: a.rz.level, pillText: a.rz.pill }),
    row({ href: '#youtube', tile: 'youtube', glyph: 'account', title: 'YouTube account', sub: a.yt.detail, level: a.yt.level, pillText: a.yt.pill }),
    row({ href: '#device', tile: 'device', glyph: 'player', title: 'Device', sub: a.dev.detail, level: a.dev.level, pillText: a.dev.pill }),
  ].join(''));
}

// ---------- YouTube account ----------
const FORMAT_MSG = 'That isn’t a cookies.txt file with youtube.com cookies. Export it again while youtube.com is open.';
const st0 = () => S.status || {};

// ---------- sign in with a code (tv.ts on the server) ----------
const TV_ERRORS = {
  denied: 'Access wasn’t allowed. Get a new code to try again.',
  expired: 'That code expired. Get a new one.',
  revoked: 'The sign-in stopped working (it was removed from the Google account). Sign in again.',
  failed: 'Signing in didn’t work. Get a new code to try again.',
};
function renderTv() {
  const tv = st0().tv;
  const body = $('#tvBody');
  if (!tv) { setHtml(body, ''); return; }
  if (tv.pending) {
    const groups = String(tv.pending.code).split('-');
    setHtml(body, `<section class="pairing" aria-labelledby="tvLabel">
      <div class="pc-code">
        <p class="pc-label" id="tvLabel">Sign-in code</p>
        <div class="big-code is-letters" aria-label="${esc(tv.pending.code.split('').join(' '))}">${groups.map((g) => `<span>${esc(g)}</span>`).join('')}</div>
        <p class="expiry" data-tv-countdown role="timer" aria-live="off"></p>
        <div class="pc-actions">
          <button class="btn btn-sm" type="button" data-act="tvCopy">${icon('copy')}<span>Copy code</span></button>
          <button class="btn btn-sm btn-quiet" type="button" data-act="tvCancel">Cancel</button>
        </div>
      </div>
      <div class="pc-steps">
        <ol class="steps">
          <li>On your phone or computer, open <a href="${esc(tv.pending.url)}" target="_blank" rel="noopener noreferrer"><b>${esc(tv.pending.url.replace(/^https?:\/\//, ''))}</b></a>.</li>
          <li>Enter the code and pick the Google account for this server.</li>
          <li>Allow access. This page updates by itself.</li>
        </ol>
        <p class="waiting">${spinner()}<span>Waiting for Google…</span></p>
        <div class="note info">${icon('info')}<span>Like signing in a smart TV. A separate Google account for the server is safest.</span></div>
      </div>
    </section>`);
    tickTv();
    return;
  }
  if (tv.signedIn) {
    const name = tv.account?.name;
    const lead = name ? `<span class="avatar" aria-hidden="true">${esc([...name.trim()][0]?.toUpperCase() || '?')}</span>` : tileHtml('youtube', 'account');
    setHtml(body, `<div class="group"><div class="row acct">${lead}
        <div class="row-main"><div class="row-title">${esc(signedInAs(tv.account))}</div><div class="row-sub">YouTube home, subscriptions and history; YouTube Music playlists and liked songs.</div></div>
        ${pill('ok', 'Signed in')}
      </div>
      <div class="row-actions"><button class="btn btn-red-text push" type="button" data-act="tvSignOut">${icon('close')}<span>Sign out</span></button></div></div>`);
    return;
  }
  const err = tv.error && TV_ERRORS[tv.error] ? `<div class="note err">${icon('alert')}<span>${TV_ERRORS[tv.error]}</span></div>` : '';
  setHtml(body, `${err}<div class="group"><div class="row">${ytTile()}
      <div class="row-main"><div class="row-title">Not signed in</div><div class="row-sub">Get a code, enter it at google.com/device, and the server uses that account for your home, subscriptions, history and playlists.</div></div>
      <button class="btn btn-primary${S.tvBusy ? ' is-busy' : ''}" type="button" data-act="tvStart"${S.tvBusy ? ' disabled' : ''}>${S.tvBusy ? spinner() : icon('account')}<span>Get a code</span></button>
    </div></div>`);
}
function tickTv() {
  const cd = $('[data-tv-countdown]'), p = st0().tv?.pending;
  if (!cd || !p) return;
  const left = Math.max(0, Math.ceil((p.expiresAt - now()) / 1000));
  cd.textContent = left ? `Expires in ${clock(left)}` : 'This code expired. Get a new one.';
  cd.classList.toggle('is-low', left <= 60);
}
async function tvStart() {
  S.tvBusy = true;
  renderTv();
  try {
    const pending = await api('POST', '/tv', undefined, { timeout: 30000 });
    if (S.status) S.status.tv = { ...(S.status.tv || {}), signedIn: false, pending, error: null };
  } catch (err) {
    if (err.status !== 401) toast(`No code from Google. ${reason(err)}`, 'err');
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
    title: 'Sign out of YouTube?',
    text: 'Your home, subscriptions, history and playlists go away on your Rondo. Search and playback keep working without an account.',
    action: 'Sign out', danger: true,
  });
  if (!ok) return;
  try {
    await api('DELETE', '/tv');
    toast('Signed out');
  } catch (err) {
    if (err.status !== 401) toast(`Not signed out. ${reason(err)}`, 'err');
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
    const t = !c.present ? 'off' : c.signedIn === false ? 'bad' : c.signedIn === true ? 'youtube' : 'off';
    lead = tileHtml(t, c.signedIn === false ? 'alert' : 'account');
  }
  const title = !c.present ? 'No cookies.txt' : c.signedIn === false ? 'cookies.txt is signed out' : c.signedIn === true ? signedInAs(c.account) : 'Cookies uploaded';
  const sub = !c.present ? 'YouTube Music home and history are generic until you upload one.'
    : c.signedIn === false ? `Google ended the session these cookies belong to.${c.checkedAt ? ` Checked ${ago(c.checkedAt)}.` : ''}`
    : c.signedIn === true ? (c.checkedAt ? `Checked ${ago(c.checkedAt)}` : '')
    : 'Not checked yet. The server checks every 15 minutes.';
  // this group is about cookies.txt only (the code sign-in has its own above)
  const [level, pillText] = !c.present ? ['off', 'No cookies'] : c.signedIn === false ? ['bad', 'Signed out'] : c.signedIn === true ? ['ok', 'Signed in'] : ['off', 'Not checked'];
  const actions = c.present ? `
    <div class="row-actions">
      <button class="btn${S.checking ? ' is-busy' : ''}" type="button" data-act="checkCookies"${S.checking ? ' disabled' : ''}>${icon('refresh')}<span>${S.checking ? 'Checking…' : c.signedIn == null ? 'Check now' : 'Check again'}</span></button>
      <button class="btn btn-red-text push" type="button" data-act="removeCookies">${icon('trash')}<span>Remove cookies</span></button>
    </div>` : '';
  setHtml($('#ytState'), `
    <div class="row acct">${lead}
      <div class="row-main"><div class="row-title">${esc(title)}</div>${sub ? `<div class="row-sub">${esc(sub)}</div>` : ''}</div>
      ${pill(level, pillText)}
    </div>${actions}`);
  $('#h-upload').textContent = c.present ? 'Replace cookies.txt' : 'Upload cookies.txt';
  renderDropzone();
}
function renderDropzone() {
  const dz = $('#cookieDrop');
  dz.classList.toggle('is-busy', S.cookieBusy);
  $('.dz-ico', dz).innerHTML = S.cookieBusy ? spinner() : icon('file');
  const touch = matchMedia('(hover: none)').matches;
  $('#dzTitle').textContent = S.cookieBusy ? 'Checking the cookies with YouTube…' : touch ? 'Choose cookies.txt' : 'Drop cookies.txt here';
  $('#dzSub').innerHTML = S.cookieBusy ? 'This takes a few seconds.'
    : `${touch ? 'Tap to pick the file you exported.' : 'or <u>choose a file</u>.'} It goes straight to your server; this page never shows what’s inside.`;
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
  if (file.size > 1024 * 1024) return cookieError('That file is larger than 1 MB, so it isn’t a cookies.txt file. Export it again.');
  if (!file.size) return cookieError('That file is empty. Export the cookies again.');
  let text;
  try { text = await file.text(); } catch { return cookieError('Couldn’t read that file. Try choosing it again.'); }
  if (!/youtube\.com/i.test(text)) return cookieError(FORMAT_MSG);
  S.cookieBusy = true;
  renderDropzone();
  try {
    const r = await api('POST', '/cookies', text, { text: true, timeout: 60000 });
    text = null;
    S.status.cookies = { ...S.status.cookies, present: true, signedIn: r.signedIn ?? null, account: r.account ?? null, checkedAt: now() };
    if (r.signedIn) toast(r.account?.name ? `Cookies saved. Signed in as ${r.account.name}.` : 'Cookies saved. Signed in.');
    else if (r.signedIn === false) cookieError('Saved, but YouTube says these cookies are signed out. Export them again from a new private window.');
  } catch (e) {
    if (e.status === 401) return;
    if (e.code === 'format') cookieError(FORMAT_MSG);
    else if (e.status === 413) cookieError('That file is larger than 1 MB, so it isn’t a cookies.txt file. Export it again.');
    else cookieError(`Not uploaded. ${reason(e)}`);
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
    if (r.signedIn) toast(r.account?.name ? `Still signed in as ${r.account.name}` : 'Still signed in');
    else toast('YouTube says these cookies are signed out', 'err');
  } catch (e) {
    if (e.status !== 401) toast(`Couldn’t check. ${reason(e)}`, 'err');
  } finally {
    S.checking = false;
    renderChrome();
    if (S.route === 'youtube') renderYouTube();
  }
}
async function removeCookies() {
  const ok = await confirmSheet({
    title: 'Remove the YouTube cookies?',
    text: 'YouTube Music home, library and history stop working on your Rondo until you upload new cookies.',
    action: 'Remove',
  });
  if (!ok) return;
  try {
    await api('DELETE', '/cookies');
    S.status.cookies = { present: false, signedIn: null, account: null, checkedAt: null };
    cookieError('');
    toast('Cookies removed');
    renderChrome();
    renderYouTube();
  } catch (e) { if (e.status !== 401) toast(`Not removed. ${reason(e)}`, 'err'); }
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
  if (files.length > 1) return cookieError('Drop one file: the cookies.txt you exported.');
  uploadCookies(files[0]);
});
dropzone.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); $('#cookieFile').click(); } });

// ---------- device ----------
function renderDevice() {
  const a = areas(S.status), d = a.d;
  const parts = [];
  if (S.celebrate) parts.push(doneHtml());
  else if (S.pair) parts.push(pairHtml(d));
  if (d.source === 'env' && !S.celebrate) parts.push(`<div class="note info device-note">${icon('info')}<span>Your Rondo uses the token from the server’s environment (<code>API_TOKEN</code>) until a Rondo is connected here. Connecting gives it its own token; after that, <code>API_TOKEN</code> isn’t used.</span></div>`);
  if (d.source || !S.pair) parts.push(deviceCard(d));
  setHtml($('#deviceBody'), parts.join(''));
  tick();
}
function deviceCard(d) {
  if (!d.source) {
    return `<div class="group empty">${tileHtml('off', 'player')}
      <h3>No Rondo connected</h3>
      <p>Connect your Rondo so it can stream from this server. You’ll get a code to enter in the Rondo’s dashboard.</p>
      <button class="btn btn-primary" type="button" data-act="connect"${S.codeBusy ? ' disabled' : ''}>${icon('plus')}<span>Connect a Rondo</span></button></div>`;
  }
  const paired = d.source === 'paired';
  const facts = [
    ['Last seen', d.lastSeenAt ? `<time ${timeAttr(d.lastSeenAt)}>${esc(ago(d.lastSeenAt))}</time>` : 'Not yet'],
    d.lastIp ? ['IP address', esc(d.lastIp)] : null,
    d.userAgent ? ['User agent', esc(d.userAgent)] : null,
  ].filter(Boolean);
  const actions = paired
    ? `${S.pair ? '' : `<button class="btn" type="button" data-act="connect">${icon('plus')}<span>Connect a different Rondo</span></button>`}
       <button class="btn btn-red-text push" type="button" data-act="disconnect">${icon('close')}<span>Disconnect</span></button>`
    : S.pair ? '' : `<button class="btn btn-primary" type="button" data-act="connect">${icon('plus')}<span>Connect a Rondo</span></button>`;
  return `<div class="group">
    <div class="row dev-head">${tileHtml('device', 'player')}
      <div class="row-main"><div class="row-title">${esc(d.name || 'Rondo')}</div>
        <div class="row-sub">${paired ? (d.pairedAt ? `Connected ${esc(when(d.pairedAt))}` : 'Connected') : 'Uses API_TOKEN from the server’s environment'}</div></div>
      ${pill(paired ? 'ok' : 'off', paired ? 'Connected' : 'API_TOKEN')}
    </div>
    ${facts.map(([k, v]) => `<div class="kv"><span>${k}</span><b>${v}</b></div>`).join('')}
    ${actions.trim() ? `<div class="row-actions">${actions}</div>` : ''}
  </div>`;
}
function pairHtml(d) {
  const p = S.pair, code = String(p.code).padStart(6, '0');
  const notes = [];
  if (d.source === 'paired') notes.push(`<div class="note">${icon('alert')}<span>Connecting a new Rondo disconnects the current one.</span></div>`);
  if (LOCAL) notes.push(`<div class="note">${icon('alert')}<span>This page is open as <b>${esc(location.hostname)}</b>, which your Rondo can’t reach. Use the server’s network address instead, like <b>http://192.168.1.10:3000</b>.</span></div>`);
  else if (!(S.status.server?.https ?? S.state?.https)) notes.push(`<div class="note">${icon('unlock')}<span>Not encrypted: use it only on your home network.</span></div>`);
  return `<section class="pairing" aria-labelledby="pcLabel">
    <div class="pc-code">
      <p class="pc-label" id="pcLabel">Connect code</p>
      <div class="big-code${p.expired ? ' is-expired' : ''}" aria-label="${code.split('').join(' ')}"><span>${code.slice(0, 3)}</span><span>${code.slice(3)}</span></div>
      <p class="expiry" data-countdown role="timer" aria-live="off"></p>
      <div class="drain" aria-hidden="true"><span data-drain></span></div>
      <div class="pc-actions">
        <button class="btn ${p.expired ? 'btn-primary' : 'btn-sm'}" type="button" data-act="newCode"${S.codeBusy ? ' disabled' : ''}>${icon('refresh')}<span>New code</span></button>
        <button class="btn btn-sm btn-quiet" type="button" data-act="cancelCode">${p.expired ? 'Close' : 'Cancel'}</button>
      </div>
    </div>
    <div class="pc-steps">
      <ol class="steps">
        <li>On your Rondo, open <b class="nowrap">Settings › Dashboard</b>, then open the dashboard it shows in a browser.</li>
        <li>In the dashboard’s <b>Server</b> page, enter this server’s address:
          <div class="addr"><span>${esc(ORIGIN)}</span><button class="ib ib-sm" type="button" data-act="copy" aria-label="Copy the address">${icon('copy')}</button></div></li>
        <li>Then enter the code. Your Rondo connects right away.</li>
      </ol>
      ${p.expired ? '' : `<p class="waiting">${spinner()}<span>Waiting for your Rondo…</span></p>`}
      ${notes.join('')}
    </div>
  </section>`;
}
function doneHtml() {
  return `<section class="pairing is-done" role="status">
    <div class="done-mark"><svg class="mk" viewBox="-66 -66 132 132" aria-hidden="true">${MARK_INNER}</svg><span class="done-check">${icon('check')}</span></div>
    <h2 class="done-title">Your Rondo is connected</h2>
    <p class="done-text">It streams from this server now, with its own token.</p>
    <button class="btn btn-primary" type="button" data-act="doneOk">Done</button>
  </section>`;
}
async function newCode(confirmReplace) {
  if (S.codeBusy) return;
  const d = S.status.device || {};
  if (confirmReplace && d.source === 'paired') {
    const ok = await confirmSheet({
      title: 'Connect a different Rondo?',
      text: 'The Rondo connected now stops working with this server as soon as the new one connects.',
      action: 'Get a code', danger: false,
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
    if (e.status !== 401) toast(`Couldn’t get a code. ${reason(e)}`, 'err');
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
  try { await api('DELETE', '/device/code'); } catch (e) { if (e.status !== 401) toast(`Couldn’t cancel the code. ${reason(e)}`, 'err'); }
  schedulePoll();
}
async function disconnect() {
  const ok = await confirmSheet({
    title: 'Disconnect your Rondo?',
    text: 'It stops streaming from this server right away. You can connect it again with a new code.',
    action: 'Disconnect',
  });
  if (!ok) return;
  try {
    await api('DELETE', '/device');
    toast('Rondo disconnected');
    await poll();
  } catch (e) { if (e.status !== 401) toast(`Not disconnected. ${reason(e)}`, 'err'); }
}
async function copyAddress(btn) {
  let ok = false;
  try { await navigator.clipboard.writeText(ORIGIN); ok = true; } catch {
    // http on a LAN address: no Clipboard API outside secure contexts
    const ta = Object.assign(document.createElement('textarea'), { value: ORIGIN });
    ta.setAttribute('readonly', '');
    ta.style.cssText = 'position:fixed;top:0;left:0;opacity:0';
    document.body.append(ta);
    ta.select();
    try { ok = document.execCommand('copy'); } catch { ok = false; }
    ta.remove();
  }
  if (ok) {
    $('use', btn).setAttribute('href', '#i-check');
    setTimeout(() => $('use', btn)?.setAttribute('href', '#i-copy'), 1600);
    toast('Address copied');
  } else toast('Couldn’t copy. Select the address and copy it.', 'err');
}

// ---------- resolver ----------
function renderResolver() {
  const a = areas(S.status), R = S.rz;
  let upd = '';
  if (R.updating) upd = `<div class="rz-line wait">${spinner()}<span>Updating… <span data-elapsed>${clock(Math.floor((Date.now() - R.updateStart) / 1000))}</span></span></div><div class="progress" aria-hidden="true"></div>`;
  else if (R.update?.ok) upd = R.update.before && R.update.before === R.update.after
    ? `<div class="rz-line ok">${icon('check')}<span>Already up to date: <b>${esc(R.update.after)}</b></span></div>`
    : `<div class="rz-line ok">${icon('check')}<span>Updated <b>${esc(R.update.before || 'unknown')}</b><span class="arrow"> → </span><b>${esc(R.update.after || 'unknown')}</b></span></div>`;
  else if (R.update) upd = `<div class="rz-line bad">${icon('alert')}<span>The update failed${R.update.error ? `: <b>${esc(R.update.error)}</b>` : '.'}</span></div>`;
  let test = '';
  if (R.testing) test = `<div class="rz-line wait">${spinner()}<span>Resolving a test video…</span></div>`;
  else if (R.test?.ok) test = `<div class="rz-line ok">${icon('check')}<span>Playback works. Resolved in <b>${Number(R.test.ms || 0).toLocaleString('en-US')} ms</b>.</span></div>`;
  else if (R.test) test = `<div class="rz-line bad">${icon('alert')}<span>Playback failed${R.test.error ? `: <b>${esc(R.test.error)}</b>` : '.'}</span></div>`;
  setHtml($('#resolverRows'), `
    <div class="row">${tileHtml('resolver', 'bolt')}
      <div class="row-main"><div class="row-title">yt-dlp</div><div class="row-sub ver">Version ${esc(a.y.version || 'unknown')}</div></div>
      ${pill(a.rz.level, a.rz.pill)}
    </div>
    <div class="row rz">${tileHtml('settings', 'refresh')}
      <div class="row-main"><div class="row-title">Update yt-dlp</div><div class="row-sub">Downloads the latest yt-dlp. It can take up to 2 minutes.</div></div>
      <button class="btn${R.updating ? ' is-busy' : ''}" type="button" data-act="updateYtdlp"${R.updating ? ' disabled' : ''}>${R.updating ? 'Updating…' : 'Update now'}</button>
      ${upd ? `<div class="rz-res" aria-live="polite">${upd}</div>` : ''}
    </div>
    <div class="row rz">${tileHtml('ok', 'play')}
      <div class="row-main"><div class="row-title">Test playback</div><div class="row-sub">Resolves a known video the way your Rondo does before it plays.</div></div>
      <button class="btn${R.testing ? ' is-busy' : ''}" type="button" data-act="testYtdlp"${R.testing ? ' disabled' : ''}>${R.testing ? 'Testing…' : R.test ? 'Test again' : 'Test playback'}</button>
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
      toast(changed ? 'yt-dlp updated' : 'yt-dlp is up to date');
    }
  } catch (e) {
    if (e.status === 401) { R.updating = false; return; }
    R.update = { ok: false, error: e.code === 'timeout' ? 'no answer after 3 minutes. Check the version again in a moment' : reason(e).replace(/\.$/, '') };
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
    if (e.status !== 401 && !S.logs) setHtml(logList, `<div class="row-empty">Couldn’t load the activity. ${esc(reason(e))}</div>`);
    else if (e.status !== 401) toast(`Couldn’t refresh. ${reason(e)}`, 'err');
  } finally {
    btn.disabled = false;
    btn.classList.remove('is-busy');
  }
}
$('#logsRefresh').addEventListener('click', loadLogs);
function dayLabel(t) {
  const d = new Date(t), today = new Date(now());
  const start = (x) => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime();
  const diff = Math.round((start(today) - start(d)) / 86400000);
  return diff === 0 ? 'Today' : diff === 1 ? 'Yesterday' : dfWeekday.format(t);
}
function renderLogs() {
  const lines = S.logs;
  if (!lines.length) {
    setHtml(logList, `<div class="empty">${tileHtml('ok', 'check')}<h3>No warnings</h3><p>When the server runs into a problem, such as a video that won’t resolve or cookies that stopped working, it shows up here.</p></div>`);
    return;
  }
  let html = '', last = '';
  for (const l of lines) {
    const label = dayLabel(l.time);
    if (label !== last) { html += `<div class="lhdr">${esc(label)}</div>`; last = label; }
    const recent = now() - l.time < 3600000;
    const level = l.level === 'error' ? 'error' : 'warn';
    html += `<div class="log ${level}">${icon('alert')}
      <div class="log-msg"><span class="sr-only">${level === 'error' ? 'Error' : 'Warning'}: </span>${esc(l.msg)}</div>
      <time class="log-time" ${timeAttr(l.time)}>${esc(recent ? ago(l.time) : dfTime.format(l.time))}</time></div>`;
  }
  setHtml(logList, html);
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
  if (!cur.value) return pwFail('Enter your current password.', cur);
  if ([...next.value].length < MIN_PASSWORD) return pwFail(`The new password needs at least ${MIN_PASSWORD} characters.`, next);
  if (next.value !== next2.value) return pwFail('The new passwords don’t match.', next2);
  pwBtn.disabled = true;
  pwBtn.textContent = 'Changing…';
  try {
    await api('POST', '/password', { current: cur.value, next: next.value });
    pwForm.reset();
    next.dispatchEvent(new Event('input'));
    toast('Password changed · other browsers are signed out');
  } catch (err) {
    if (err.status === 403) pwFail('The current password isn’t right.', cur);
    else if (err.status === 400) pwFail(`The new password needs at least ${MIN_PASSWORD} characters.`, next);
    else if (err.status === 429) pwFail(`Too many wrong passwords. Try again in ${err.data?.retryAfterSec ?? 60} seconds.`, cur);
    else if (err.status !== 401) pwFail(`Not changed. ${reason(err)}`);
  } finally {
    pwBtn.disabled = false;
    pwBtn.textContent = 'Change password';
  }
});
$('#signOut').addEventListener('click', async () => {
  try { await api('POST', '/logout', undefined, { auth: false }); } catch { /* signed out locally anyway */ }
  S.logs = null;
  showSignIn('You’re signed out.');
});

// ---------- actions ----------
const ACTIONS = {
  connect: () => newCode(true),
  newCode: () => newCode(false),
  cancelCode,
  disconnect,
  copy: (b) => copyAddress(b),
  doneOk: () => { S.celebrate = false; renderDevice(); renderChrome(); },
  checkCookies,
  removeCookies,
  updateYtdlp,
  testYtdlp,
  tvStart,
  tvCancel,
  tvSignOut,
  tvCopy: async () => {
    try { await navigator.clipboard.writeText(st0().tv?.pending?.code || ''); toast('Code copied'); } catch { toast('Couldn’t copy. Select the code instead.', 'err'); }
  },
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
      <div class="sheet-actions"><button class="btn" type="button" data-r="0">Cancel</button><button class="btn ${danger ? 'btn-danger' : 'btn-primary'}" type="button" data-r="1">${esc(action)}</button></div></div>`;
    if (!sheet.open) sheet.showModal();
    $('[data-r="0"]', sheet).focus();
    const onClick = (e) => { const b = e.target.closest('[data-r]'); if (b) finish(b.dataset.r === '1'); };
    const finish = (v) => { sheet.removeEventListener('click', onClick); sheet.removeEventListener('close', onClose); if (sheet.open) sheet.close(); resolve(v); };
    const onClose = () => finish(false);
    sheet.addEventListener('click', onClick);
    sheet.addEventListener('close', onClose);
  });
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
