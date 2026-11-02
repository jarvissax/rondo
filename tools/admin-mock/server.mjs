// Mock of the Rondo server's admin API (docs/dev/server-api.md) for working on the admin page without the
// real server. Serves server/admin/ at /admin/ and the app's Roboto at /admin/fonts/, keeps
// everything in memory. Node 22, no dependencies.
//
//   node tools/admin-mock/server.mjs              port 8095, set up, cookies signed in, a Rondo paired
//   node tools/admin-mock/server.mjs --fresh      first run: setup code in the console, nothing set up
//   node tools/admin-mock/server.mjs --env        the Rondo uses API_TOKEN from the environment
//   node tools/admin-mock/server.mjs --signed-out cookies present but Google signed them out
//   node tools/admin-mock/server.mjs --broken     yt-dlp failing until "Update now"
//   node tools/admin-mock/server.mjs --https      report the connection as HTTPS (as behind Caddy)
//   node tools/admin-mock/server.mjs --no-autopair  codes wait for a real POST /device/pair
//   node tools/admin-mock/server.mjs --slow       answers take 0.4-1.2 s
//   node tools/admin-mock/server.mjs --tv         signed in with a code (otherwise "Get a code" approves itself after 8 s)
//   node tools/admin-mock/server.mjs --updates    a newer Rondo and yt-dlp are out (updates.ts)
//   PORT=9000 CODE_SECONDS=40 node tools/admin-mock/server.mjs
//
// Cookies upload: any file with a line containing youtube.com is accepted; put "signedout" in it to
// get a signed-out answer. Without --fresh the admin password is printed at start.
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WEB = path.join(ROOT, 'server/admin');
const FONT = path.join(ROOT, 'app/app/src/main/assets/fonts/Roboto-Regular.ttf');
const PORT = Number(process.env.PORT) || 8095;
const CODE_MS = (Number(process.env.CODE_SECONDS) || 600) * 1000;
const argv = new Set(process.argv.slice(2));
const FRESH = argv.has('--fresh');
const ENV = argv.has('--env');
const SIGNED_OUT = argv.has('--signed-out');
const BROKEN = argv.has('--broken');
const HTTPS = argv.has('--https');
const AUTOPAIR = !argv.has('--no-autopair');
const SLOW = argv.has('--slow');
const UPDATES = argv.has('--updates');
const PASSWORD = 'rondo-admin-pass';
const SESSION_MS = 12 * 60 * 60 * 1000;

const NOW = Date.now();
const BOOT = NOW - 5400 * 1000; // pretend the server has been up for 1.5 h
const code6 = () => String(crypto.randomInt(0, 1_000_000)).padStart(6, '0');
const shown = (c) => `${c.slice(0, 3)}-${c.slice(3)}`;

// ---------- state ----------
const S = {
  tv: process.argv.includes('--tv') ? { signedIn: true, account: { name: 'Alex', handle: '@alex' }, pending: null, error: null } : { signedIn: false, account: null, pending: null, error: null },
  password: FRESH ? null : PASSWORD,
  setupCode: FRESH ? code6() : null,
  sessions: new Map(),
  failures: [],
  cookies: FRESH ? null : SIGNED_OUT
    ? { signedIn: false, account: null, checkedAt: NOW - 4 * 60000 }
    : { signedIn: true, account: { name: 'Alex', handle: '@alex' }, checkedAt: NOW - 3 * 60000 },
  device: FRESH || ENV ? null : { pairedAt: NOW - 2 * 86400000 - 3 * 3600000, name: 'Rondo' },
  apiToken: ENV,
  lastSeen: FRESH ? null : { at: NOW - 95 * 1000, ip: '192.168.1.118', userAgent: 'Rondo/1.0.0 (Linux; Android 4.2.2; Innioasis Y1) ExoPlayerLib/2.19.1' },
  pairing: null,
  ytdlp: { version: BROKEN ? '2026.09.14.232015' : '2026.10.29.232701', lastResolveOk: FRESH ? null : !BROKEN },
  logs: FRESH ? [] : [
    { time: NOW - 6 * 60000, level: 'warn', msg: 'stream 4NRXx6U8ABQ: googlevideo answered 403, resolving again' },
    { time: NOW - 52 * 60000, level: 'warn', msg: 'range request to rr3---sn-4g5e6nzz.googlevideo.com timed out after 15 s, retrying' },
    { time: NOW - 3 * 3600000, level: 'error', msg: 'resolve xvFZjo5PgG0 failed: ERROR: [youtube] xvFZjo5PgG0: Video unavailable. This video is not available in your country' },
    { time: NOW - 26 * 3600000, level: 'warn', msg: 'thumb i.ytimg.com/vi/kXYiU_JCYtU/hqdefault.jpg: 404' },
    { time: NOW - 50 * 3600000, level: 'error', msg: 'browse FEmusic_home failed: Request to https://music.youtube.com/youtubei/v1/browse failed with status code 400 (Precondition check failed)' },
    { time: NOW - 51 * 3600000, level: 'warn', msg: 'youtube account check: cookies are signed out' },
  ],
};
if (BROKEN && !FRESH) S.logs.unshift({ time: NOW - 2 * 60000, level: 'error', msg: 'resolve dQw4w9WgXcQ failed: ERROR: [youtube] dQw4w9WgXcQ: Sign in to confirm you’re not a bot' });
const log = (level, msg) => { S.logs.unshift({ time: Date.now(), level, msg }); S.logs.length = Math.min(S.logs.length, 100); };

// ---------- http helpers ----------
const MIME = { '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.svg': 'image/svg+xml', '.ttf': 'font/ttf', '.png': 'image/png', '.ico': 'image/x-icon' };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const delay = () => sleep(SLOW ? 400 + Math.random() * 800 : 40 + Math.random() * 120);
function send(res, status, body, headers = {}) {
  const data = typeof body === 'string' || Buffer.isBuffer(body) ? body : JSON.stringify(body);
  res.writeHead(status, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...headers });
  res.end(data);
}
const err = (res, status, error, extra = {}) => send(res, status, { error, ...extra });
async function readBody(req, limit = 1024 * 1024) {
  const chunks = [];
  let size = 0;
  for await (const c of req) { size += c.length; if (size > limit) return null; chunks.push(c); }
  return Buffer.concat(chunks).toString('utf8');
}
async function readJson(req) {
  const raw = await readBody(req, 64 * 1024);
  if (!raw) return {};
  try { return JSON.parse(raw); } catch { return null; }
}
function serveFile(res, file) {
  fs.readFile(file, (e, data) => {
    if (e) return send(res, 404, 'Not found', { 'Content-Type': 'text/plain' });
    res.writeHead(200, { 'Content-Type': MIME[path.extname(file)] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
    res.end(data);
  });
}
const sessionOf = (req) => /(?:^|;\s*)rondo_admin=([^;]+)/.exec(req.headers.cookie || '')?.[1];
function signedIn(req) {
  const t = sessionOf(req);
  const exp = t && S.sessions.get(t);
  if (!exp) return false;
  if (exp < Date.now()) { S.sessions.delete(t); return false; }
  return true;
}
function startSession(res) {
  const t = crypto.randomBytes(24).toString('hex');
  S.sessions.set(t, Date.now() + SESSION_MS);
  return `rondo_admin=${t}; Path=/admin; HttpOnly; SameSite=Strict; Max-Age=${SESSION_MS / 1000}`;
}
function waitSec() {
  const t = Date.now();
  S.failures = S.failures.filter((x) => t - x < 60000);
  return S.failures.length >= 5 ? Math.ceil((S.failures[0] + 60000 - t) / 1000) : 0;
}
const source = () => (S.device ? 'paired' : S.apiToken ? 'env' : null);
function status() {
  const p = S.pairing && S.pairing.expiresAt > Date.now() ? S.pairing : null;
  if (!p) S.pairing = null;
  return {
    server: { version: '1.0.0', uptimeSec: Math.floor((Date.now() - BOOT) / 1000), https: HTTPS },
    ytdlp: { ...S.ytdlp },
    cookies: { present: !!S.cookies, signedIn: S.cookies ? S.cookies.signedIn : null, account: S.cookies?.account ?? null, checkedAt: S.cookies?.checkedAt ?? null },
    device: {
      paired: source() !== null, source: source(), pairedAt: S.device?.pairedAt ?? null,
      lastSeenAt: source() ? S.lastSeen?.at ?? null : null, lastIp: source() ? S.lastSeen?.ip ?? null : null, userAgent: source() ? S.lastSeen?.userAgent ?? null : null,
    },
    pairing: p ? { code: p.code, expiresAt: p.expiresAt } : null,
    tv: { ...S.tv, pending: S.tv.pending && S.tv.pending.expiresAt > Date.now() ? S.tv.pending : null },
    updates: UPDATES
      ? { server: { version: '1.0.1', url: 'https://github.com/jarvissax/rondo/releases/tag/v1.0.1', publishedAt: NOW - 86400000 }, ytdlp: S.ytdlp.version < '2026.11.01' ? { version: '2026.11.01.233542', url: 'https://github.com/yt-dlp/yt-dlp-nightly-builds/releases', publishedAt: NOW - 3600000 } : null, checkedAt: NOW }
      : { server: null, ytdlp: null, checkedAt: NOW },
  };
}
function pairDevice(name = 'Rondo') {
  S.device = { pairedAt: Date.now(), name };
  S.pairing = null;
  S.lastSeen = { at: Date.now(), ip: `192.168.1.${20 + crypto.randomInt(0, 180)}`, userAgent: 'Rondo/1.0.0 (Linux; Android 4.2.2; Innioasis Y1) ExoPlayerLib/2.19.1' };
  console.log(`  device paired (${name})`);
}
function isCookiesFile(text) {
  return text.split(/\r?\n/).some((line) => line.trim() && (!line.startsWith('#') || line.startsWith('#HttpOnly_')) && line.includes('youtube.com'));
}

// ---------- the API ----------
async function api(req, res, url) {
  const { pathname } = url;
  const method = req.method;

  if (pathname === '/device/pair' && method === 'POST') {
    const b = await readJson(req);
    const p = S.pairing;
    if (!p || p.expiresAt < Date.now() || String(b?.code ?? '').replace(/\D/g, '') !== p.code) {
      if (p) { p.tries = (p.tries || 0) + 1; if (p.tries >= 5) S.pairing = null; }
      return err(res, 403, 'code');
    }
    pairDevice(typeof b.name === 'string' ? b.name.slice(0, 40) : 'Rondo');
    return send(res, 200, { token: crypto.randomBytes(32).toString('hex') });
  }

  if (pathname === '/admin/api/state' && method === 'GET') return send(res, 200, { setup: !S.password, signedIn: signedIn(req), https: HTTPS });

  if (pathname === '/admin/api/setup' && method === 'POST') {
    if (S.password) return err(res, 409, 'done');
    const wait = waitSec();
    if (wait) return err(res, 429, 'wait', { retryAfterSec: wait });
    const b = await readJson(req);
    if (String(b?.code ?? '').replace(/\D/g, '') !== S.setupCode) { S.failures.push(Date.now()); return err(res, 403, 'code'); }
    if (typeof b.password !== 'string' || [...b.password].length < 10) return err(res, 400, 'password');
    S.password = b.password;
    S.setupCode = null;
    console.log('  setup done');
    return send(res, 200, { ok: true }, { 'Set-Cookie': startSession(res) });
  }

  if (pathname === '/admin/api/login' && method === 'POST') {
    const wait = waitSec();
    if (wait) return err(res, 429, 'wait', { retryAfterSec: wait });
    const b = await readJson(req);
    if (!S.password || b?.password !== S.password) { S.failures.push(Date.now()); return err(res, 403, 'password'); }
    S.failures = [];
    return send(res, 200, { ok: true }, { 'Set-Cookie': startSession(res) });
  }

  if (!pathname.startsWith('/admin/api/')) return err(res, 404, 'not found');
  if (!signedIn(req)) return err(res, 401, 'signin');

  if (pathname === '/admin/api/logout' && method === 'POST') {
    S.sessions.delete(sessionOf(req));
    return send(res, 200, { ok: true }, { 'Set-Cookie': 'rondo_admin=; Path=/admin; HttpOnly; SameSite=Strict; Max-Age=0' });
  }
  if (pathname === '/admin/api/password' && method === 'POST') {
    const b = await readJson(req);
    if (b?.current !== S.password) return err(res, 403, 'password');
    if (typeof b.next !== 'string' || [...b.next].length < 10) return err(res, 400, 'password');
    S.password = b.next;
    console.log('  admin password changed');
    return send(res, 200, { ok: true });
  }
  if (pathname === '/admin/api/status' && method === 'GET') return send(res, 200, status());

  if (pathname === '/admin/api/cookies' && method === 'POST') {
    const text = await readBody(req);
    if (text === null) return err(res, 413, 'size');
    if (!isCookiesFile(text)) return err(res, 400, 'format');
    await sleep(1500);
    const out = /signedout/i.test(text);
    S.cookies = out ? { signedIn: false, account: null, checkedAt: Date.now() } : { signedIn: true, account: { name: 'Rondo Music', handle: '@rondomusic' }, checkedAt: Date.now() };
    if (out) log('warn', 'youtube account check: cookies are signed out');
    console.log(`  cookies uploaded (${text.length} bytes, ${out ? 'signed out' : 'signed in'})`);
    return send(res, 200, { ok: true, signedIn: S.cookies.signedIn, account: S.cookies.account });
  }
  if (pathname === '/admin/api/cookies' && method === 'DELETE') {
    S.cookies = null;
    return send(res, 200, { ok: true });
  }
  if (pathname === '/admin/api/cookies/check' && method === 'POST') {
    await sleep(1000);
    if (!S.cookies) return send(res, 200, { signedIn: false, account: null });
    S.cookies.checkedAt = Date.now();
    return send(res, 200, { signedIn: S.cookies.signedIn, account: S.cookies.account });
  }

  if (pathname === '/admin/api/ytdlp/update' && method === 'POST') {
    await sleep(5000);
    const before = S.ytdlp.version;
    const after = '2026.10.29.232701';
    S.ytdlp.version = after;
    if (S.ytdlp.lastResolveOk === false) S.ytdlp.lastResolveOk = null;
    console.log(`  yt-dlp ${before} -> ${after}`);
    return send(res, 200, { ok: true, before, after });
  }
  if (pathname === '/admin/api/ytdlp/test' && method === 'POST') {
    await sleep(900 + Math.random() * 900);
    if (S.ytdlp.version !== '2026.10.29.232701') {
      S.ytdlp.lastResolveOk = false;
      log('error', 'resolve dQw4w9WgXcQ failed: ERROR: [youtube] dQw4w9WgXcQ: Sign in to confirm you’re not a bot');
      return send(res, 200, { ok: false, error: 'ERROR: [youtube] dQw4w9WgXcQ: Sign in to confirm you’re not a bot. Use --cookies-from-browser or --cookies for the authentication.' });
    }
    S.ytdlp.lastResolveOk = true;
    return send(res, 200, { ok: true, ms: 1200 + Math.floor(Math.random() * 1200) });
  }

  if (pathname === '/admin/api/device/code' && method === 'POST') {
    const code = code6();
    S.pairing = { code, expiresAt: Date.now() + CODE_MS, tries: 0 };
    console.log(`  connect code ${shown(code)}${AUTOPAIR ? ' (a fake Rondo pairs in 10 s)' : ''}`);
    if (AUTOPAIR) setTimeout(() => { if (S.pairing?.code === code && S.pairing.expiresAt > Date.now()) pairDevice(); }, 10000);
    return send(res, 200, { code, expiresAt: S.pairing.expiresAt });
  }
  if (pathname === '/admin/api/device/code' && method === 'DELETE') {
    S.pairing = null;
    return send(res, 200, { ok: true });
  }
  if (pathname === '/admin/api/device' && method === 'DELETE') {
    S.device = null;
    S.apiToken = false;
    S.lastSeen = null;
    console.log('  device disconnected');
    return send(res, 200, { ok: true });
  }
  if (pathname === '/admin/api/logs' && method === 'GET') return send(res, 200, { lines: S.logs });
  if (pathname === '/admin/api/tv' && method === 'POST') {
    const pending = { url: 'https://www.google.com/device', code: 'DFK-ZVL-KYX', expiresAt: Date.now() + 1800_000 };
    S.tv = { signedIn: false, account: null, pending, error: null };
    clearTimeout(S.tvTimer);
    S.tvTimer = setTimeout(() => { if (S.tv.pending === pending) S.tv = { signedIn: true, account: { name: 'Alex', handle: '@alex' }, pending: null, error: null }; }, 8000);
    return send(res, 200, pending);
  }
  if (pathname === '/admin/api/tv/code' && method === 'DELETE') { clearTimeout(S.tvTimer); S.tv.pending = null; return send(res, 200, { ok: true }); }
  if (pathname === '/admin/api/tv' && method === 'DELETE') { clearTimeout(S.tvTimer); S.tv = { signedIn: false, account: null, pending: null, error: null }; return send(res, 200, { ok: true }); }

  return err(res, 404, 'not found');
}

http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  try {
    if (url.pathname.startsWith('/admin/api/') || url.pathname === '/device/pair') {
      await delay();
      return await api(req, res, url);
    }
    if (url.pathname === '/' || url.pathname === '/admin') return send(res, 302, '', { Location: '/admin/' });
    if (url.pathname === '/admin/fonts/Roboto-Regular.ttf') return serveFile(res, FONT);
    if (url.pathname === '/admin/') return serveFile(res, path.join(WEB, 'index.html'));
    const m = /^\/admin\/([\w.-]+)$/.exec(url.pathname);
    if (m && !m[1].startsWith('.')) return serveFile(res, path.join(WEB, m[1]));
    return send(res, 404, 'Not found', { 'Content-Type': 'text/plain' });
  } catch (e) {
    console.error(e);
    if (!res.headersSent) err(res, 500, 'mock error');
  }
}).listen(PORT, () => {
  const flags = [...argv].join(' ');
  console.log(`Rondo admin mock on http://localhost:${PORT}/admin/${flags ? ` (${flags})` : ''}`);
  if (S.setupCode) console.log(`Rondo setup: open http://localhost:${PORT}/admin and enter ${shown(S.setupCode)}`);
  else console.log(`  admin password: ${PASSWORD}`);
});
