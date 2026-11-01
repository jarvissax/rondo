// Mock of the Rondo dashboard API (docs/dev/dashboard-api.md) for working on the web UI without a Y1.
// Serves app/app/src/main/assets/dashboard/ at / and assets/fonts/ at /fonts/, keeps everything
// in memory, and prints the pairing code. Node 22, no dependencies.
//
//   node tools/dashboard-mock/server.mjs            port 8090, ~800 songs, something playing
//   node tools/dashboard-mock/server.mjs --empty    empty card, nothing playing, no Wi-Fi
//   node tools/dashboard-mock/server.mjs --slow     answers like a busy MT6572 (200-700 ms)
//   PORT=9000 node tools/dashboard-mock/server.mjs
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const WEB = path.join(ROOT, 'app/app/src/main/assets/dashboard');
const FONTS = path.join(ROOT, 'app/app/src/main/assets/fonts');
const PORT = Number(process.env.PORT) || 8090;
const argv = new Set(process.argv.slice(2));
const EMPTY = argv.has('--empty');
const SLOW = argv.has('--slow');
const UPLOAD_BYTES_PER_SEC = 3_000_000; // about what the Y1's 2.4 GHz Wi-Fi manages

// ---------- deterministic fake data ----------
function rng(seed) {
  return () => {
    seed |= 0; seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const rand = rng(1979);
const pick = (a) => a[Math.floor(rand() * a.length)];
const int = (lo, hi) => lo + Math.floor(rand() * (hi - lo + 1));

const ARTISTS = [
  'Nərgiz Əliyeva', 'Kaan Yıldız', 'Selin Aras', 'The Lantern Club', 'Mavi Deniz', 'Orhan Kılıç',
  'Paper Satellites', 'Lale Su', 'Northbound Choir', 'Elvin Qasımov', 'June Hollow', 'Rüzgar',
  'Odessa Rain', 'Tural & The Tide', 'Moss Arcade', 'Leyla Babayeva', 'Glass Harbour', 'Defne Koç',
  'Quiet Engines', 'Sabah Yolu', 'Ivory Coastline', 'Fidan Hüseynova', 'Low Orbit', 'Emre Tan',
  'Saltwater Radio', 'Aysel Məmmədova', 'Velvet Static', 'Cem Öztürk', 'Morning Bureau', 'Nilufar',
  'Copper Kites', 'Rauf Mirzə', 'Hazel & Pine', 'Beyaz Oda', 'Midnight Ferry', 'Zeynep Ak',
  'Echo Valley', 'Kamran Ağayev', 'Polar Garden', 'İpek Duman', 'Southern Lights', 'Günay',
  'Amber Room', 'Murat Aydın', 'Foxglove', 'Səbinə Rzayeva', 'Tin Palace', 'Deniz Ece',
  'Wild Atlas', 'Orkhan Novruz', 'Silver Thread', 'Kuzey', 'Second Avenue', 'Ulviyya',
];
const WORDS = [
  'Night', 'Sun', 'River', 'Glass', 'Home', 'Dream', 'Gold', 'Rain', 'Fire', 'Blue', 'Sea', 'Road',
  'Heart', 'City', 'Light', 'Winter', 'Summer', 'Echo', 'Paper', 'Silver', 'Morning', 'Shadow',
  'Garden', 'Window', 'Mountain', 'Ocean', 'Star', 'Moon', 'Wire', 'Velvet', 'Harbour', 'Signal',
  'Gecə', 'Yağış', 'Ürək', 'Deniz', 'Yol', 'Bahar', 'Rüya', 'Ayna', 'Sevda', 'Gönül', 'Işık',
];
const TITLE_SHAPES = [
  (a, b) => `${a} ${b}`, (a) => `The ${a}`, (a, b) => `${a} of ${b}`, (a, b) => `${a} and ${b}`,
  (a) => `${a}`, (a, b) => `Under the ${a} ${b}`, (a) => `${a} Again`, (a, b) => `${a}, ${b}`,
  (a) => `Last ${a}`, (a, b) => `${a} in the ${b}`, (a) => `Little ${a}`, (a) => `${a} (Live)`,
];
const title = () => pick(TITLE_SHAPES)(pick(WORDS), pick(WORDS));

const MUSIC = '/storage/sdcard0/Music';
const MOVIES = '/storage/sdcard0/Movies';
const FOLDERS = [MUSIC, MOVIES];
const AUDIO = ['flac', 'mp3', 'm4a', 'aac', 'ogg', 'opus', 'wav'];
const VIDEO = ['mp4', 'm4v', 'mkv', '3gp', 'webm'];
const ALLOWED = [...AUDIO, ...VIDEO, 'lrc'];
const clean = (s) => s.replace(/[\\/:*?"<>|]/g, '');

const NOW = Date.now();
let songs = [];
let videos = [];
if (!EMPTY) {
  for (const artist of ARTISTS) {
    const albums = int(1, 3);
    for (let a = 0; a < albums && songs.length < 820; a++) {
      const album = rand() < 0.1 ? `${artist} (Deluxe)` : title();
      const tracks = int(6, 11);
      const ext = pick(['flac', 'flac', 'mp3', 'm4a']);
      const art = rand() < 0.86;
      const modified = NOW - int(2, 900) * 86400000 - int(0, 86399) * 1000; // copied to the card together
      for (let t = 1; t <= tracks && songs.length < 820; t++) {
        const name = title();
        const durationSec = int(118, 412);
        songs.push({
          path: `${MUSIC}/${clean(artist)}/${clean(album)}/${String(t).padStart(2, '0')} ${clean(name)}.${ext}`,
          title: name, artist, album, track: t, durationSec,
          size: Math.round(durationSec * (ext === 'flac' ? 112000 : 40000)), art, modified: modified + t * 1000,
        });
      }
    }
  }
  // tracks with tags missing, like real cards have
  songs.push({ path: `${MUSIC}/voice memo 2024-03-11.m4a`, title: 'voice memo 2024-03-11', artist: '', album: '', track: 0, durationSec: 47, size: 760000, art: false, modified: NOW - 3 * 3600000 });
  songs.push({ path: `${MUSIC}/Günah Benim.mp3`, title: 'Günah Benim', artist: 'Eypio, Burak King', album: '', track: 0, durationSec: 210, size: 8400000, art: true, modified: NOW - 26 * 3600000 });
  videos = [
    { path: `${MOVIES}/Concert at the Harbour.mp4`, title: 'Concert at the Harbour', durationSec: 5412, size: 1_240_000_000, modified: NOW - 400 * 86400000 },
    { path: `${MOVIES}/Glass Harbour - Signal (Official Video).mp4`, title: 'Glass Harbour - Signal (Official Video)', durationSec: 244, size: 61_000_000, modified: NOW - 12 * 86400000 },
    { path: `${MOVIES}/Tiny Desk - Paper Satellites.mkv`, title: 'Tiny Desk - Paper Satellites', durationSec: 1083, size: 402_000_000, modified: NOW - 75 * 86400000 },
    { path: `${MOVIES}/Road trip 2025.3gp`, title: 'Road trip 2025', durationSec: 93, size: 18_000_000, modified: NOW - 160 * 86400000 },
    { path: `${MOVIES}/Velvet Static - Live in Baku.webm`, title: 'Velvet Static - Live in Baku', durationSec: 3620, size: 690_000_000, modified: NOW - 5 * 86400000 },
    { path: `${MOVIES}/Lessons/Guitar lesson 04.m4v`, title: 'Guitar lesson 04', durationSec: 812, size: 140_000_000, modified: NOW - 30 * 86400000 },
  ];
}
let pending = []; // uploaded files, picked up by the next scan
let scanning = false;

// ---------- player ----------
const YT_QUEUE = [
  { title: 'Günah Benim', subtitle: 'Eypio, Burak King', art: '/api/thumb?u=' + encodeURIComponent('https://i.ytimg.com/vi/aaa/hq.jpg'), durationMs: 210000 },
  { title: 'Sabah Yolu', subtitle: 'Sabah Yolu · Gecə', art: '/api/thumb?u=' + encodeURIComponent('https://i.ytimg.com/vi/bbb/hq.jpg'), durationMs: 187000 },
  { title: 'Northern Wire', subtitle: 'Low Orbit', art: null, durationMs: 243000 },
];
const player = {
  queue: EMPTY ? [] : YT_QUEUE.map((x) => ({ ...x, source: 'youtube' })),
  index: 0, playing: !EMPTY, positionMs: 41000, at: Date.now(), volume: 7, maxVolume: 15,
};
function position() {
  const item = player.queue[player.index];
  if (!item) return 0;
  let p = player.positionMs + (player.playing ? Date.now() - player.at : 0);
  while (p >= item.durationMs && player.playing) { // ran off the end: next song
    p -= item.durationMs;
    player.index = (player.index + 1) % player.queue.length;
    player.positionMs = 0; player.at = Date.now() - p;
    return position();
  }
  return Math.min(p, item.durationMs);
}
function playerJson() {
  const item = player.queue[player.index];
  if (!item) return null;
  const p = position();
  return {
    title: item.title, subtitle: item.subtitle, art: item.art, source: item.source, playing: player.playing,
    positionMs: Math.round(p), durationMs: item.durationMs, volume: player.volume, maxVolume: player.maxVolume,
  };
}
function setPosition(ms) { player.positionMs = ms; player.at = Date.now(); }
const songItem = (s) => ({
  title: s.title, subtitle: [s.artist, s.album].filter(Boolean).join(' · ') || 'Library',
  art: s.art ? `/api/art?path=${encodeURIComponent(s.path)}&size=300` : null, durationMs: s.durationSec * 1000, source: 'device',
});

// ---------- settings, theme, Wi-Fi, server ----------
let settings = { videoQuality: null, brightness: 60, screenTimeoutSec: 60, lyricsOnOpen: false, wheelAcceleration: 'normal', wheelVibration: 'normal', clickSound: true };

// ---------- FM radio and Bluetooth (docs/dev/dashboard-api.md) ----------
const radio = {
  headphones: !EMPTY, on: !EMPTY, frequency: 1005, muted: false, volume: 6, speaker: false, band: 'world',
  seeking: false, scanning: false,
  presets: [{ frequency: 1005, name: 'City FM' }, { frequency: 1021, name: 'Jazz 102' }, { frequency: 945, name: '' }],
  found: [886, 912, 945, 978, 1005, 1021, 1043, 1067],
};
const STATIONS = { 886: 'Classic', 912: 'News 91', 978: 'Pop Hits', 1005: 'City FM', 1021: 'Jazz 102', 1043: 'Rock 104' };
const radioRange = () => (radio.band === 'japan' ? { min: 760, max: 950 } : { min: 875, max: 1080 });
function radioJson() {
  const on = radio.on && radio.headphones;
  const station = STATIONS[radio.frequency] ?? '';
  const saved = new Set(radio.presets.map((p) => p.frequency));
  return {
    headphones: radio.headphones, on, starting: false, frequency: radio.frequency,
    name: on ? station : '', text: on && station ? `Now on ${station}` : '',
    signal: on ? (station ? 3 + (radio.frequency % 2) : 1) : 0, stereo: on && !!station,
    muted: radio.muted, volume: radio.volume, maxVolume: 13, speaker: radio.speaker,
    seeking: radio.seeking, scanning: radio.scanning, ...(radio.headphones ? {} : { error: 'headphones' }),
    band: radio.band, ...radioRange(),
    presets: radio.presets.map((p) => ({ frequency: p.frequency, name: p.name || STATIONS[p.frequency] || '' })),
    found: radio.found.filter((f) => !saved.has(f)).map((f) => ({ frequency: f, name: STATIONS[f] ?? '' })),
  };
}

const bt = {
  state: 'on', searching: false,
  mine: [{ address: '00:1A:7D:DA:71:13', name: 'Studio Headphones', kind: 'headphones', audio: true, status: 'connected' },
    { address: '00:1A:7D:DA:71:22', name: 'Kitchen Speaker', kind: 'speaker', audio: true, status: 'disconnected' }],
  others: [{ address: '00:1A:7D:DA:71:35', name: 'Running Buds', kind: 'headphones', audio: true, pairing: false }],
};
const btJson = () => ({ available: true, state: bt.state, searching: bt.searching, ...(bt.state === 'on' ? { mine: bt.mine, others: bt.others } : {}) });
const PRESETS = [
  { name: 'iPod Blue', accent: '#1c5ccc' }, { name: 'Rondo Red', accent: '#ff3b4f' }, { name: 'Mint', accent: '#11a683' },
  { name: 'Violet', accent: '#7b4fe0' }, { name: 'Amber', accent: '#e8860c' }, { name: 'Graphite', accent: '#4a4f59' },
];
// key order is the default menu order, as on the device
const LABELS = { nowPlaying: 'Now Playing', device: 'Library', videos: 'YouTube', music: 'YouTube Music', radio: 'FM Radio', settings: 'Settings' };
let theme = {
  accent: '#1c5ccc', appearance: 'light', textScale: 1.4,
  menu: Object.keys(LABELS).map((id) => ({ id, visible: true })),
};
const themeJson = () => ({
  ...theme,
  menu: theme.menu.map((m) => ({ id: m.id, label: LABELS[m.id], visible: m.visible, ...(m.id === 'settings' ? { locked: true } : {}) })),
  presets: PRESETS,
});

let wifi = {
  enabled: true,
  current: EMPTY ? null : { ssid: 'Home', signal: 3, ip: '192.168.1.118' },
  saved: EMPTY ? [] : [{ id: 2, ssid: 'Home' }, { id: 5, ssid: 'Kafe Nur' }, { id: 7, ssid: 'Office 2.4' }],
  nearby: [
    { ssid: 'Home', signal: 3, security: 'psk' }, { ssid: 'TP-Link_4F2A', signal: 4, security: 'psk' },
    { ssid: 'Kafe Nur', signal: 1, security: 'open' }, { ssid: 'AZERTELECOM_57', signal: 2, security: 'psk' },
    { ssid: 'Guest', signal: 2, security: 'open' }, { ssid: 'eduroam', signal: 1, security: 'eap' },
    { ssid: 'Old Router', signal: 0, security: 'wep' },
  ],
};
let nextWifiId = 9;
const wifiJson = () => ({
  enabled: wifi.enabled, current: wifi.current, saved: wifi.saved,
  nearby: wifi.nearby.map((n) => ({ ...n, saved: wifi.saved.some((s) => s.ssid === n.ssid) })),
});

let server = { url: 'https://music.example.com', token: EMPTY ? null : 'secret' };

// ---------- pairing ----------
const sessions = new Set();
let code = '';
let wrong = 0;
function newCode() {
  code = String(crypto.randomInt(0, 1_000_000)).padStart(6, '0');
  wrong = 0;
  console.log(`\n  Pairing code: ${code}\n`);
}

// ---------- art (generated SVG, or 404) ----------
function hash(s) { let h = 2166136261; for (const c of s) h = Math.imul(h ^ c.codePointAt(0), 16777619); return h >>> 0; }
function artSvg(key, wide) {
  const h = hash(key);
  const hue = h % 360, hue2 = (hue + 40 + (h >> 9) % 80) % 360;
  const w = wide ? 160 : 100, H = wide ? 90 : 100;
  const shape = (h >> 4) % 3;
  const motif = shape === 0
    ? `<circle cx="${w * 0.62}" cy="${H * 0.42}" r="${H * 0.28}" fill="hsl(${hue2} 80% 70% / .85)"/>`
    : shape === 1
      ? `<path d="M0 ${H * 0.72} Q ${w * 0.3} ${H * 0.45} ${w * 0.55} ${H * 0.66} T ${w} ${H * 0.58} V ${H} H 0 Z" fill="hsl(${hue2} 70% 30% / .9)"/>`
      : `<rect x="${w * 0.18}" y="${H * 0.18}" width="${w * 0.4}" height="${H * 0.4}" rx="4" fill="hsl(${hue2} 75% 62% / .8)" transform="rotate(${h % 30} ${w / 2} ${H / 2})"/>`;
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${w} ${H}" width="${w * 3}" height="${H * 3}">` +
    `<defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="hsl(${hue} 65% 52%)"/><stop offset="1" stop-color="hsl(${(hue + 30) % 360} 60% 22%)"/></linearGradient></defs>` +
    `<rect width="${w}" height="${H}" fill="url(#g)"/>${motif}</svg>`;
}

// ---------- HTTP ----------
const MIME = { '.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.svg': 'image/svg+xml', '.ttf': 'font/ttf', '.json': 'application/json', '.png': 'image/png', '.ico': 'image/x-icon' };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const delay = () => sleep(SLOW ? 200 + Math.random() * 500 : 30 + Math.random() * 120);

function send(res, status, body, headers = {}) {
  const data = typeof body === 'string' || Buffer.isBuffer(body) ? body : JSON.stringify(body);
  res.writeHead(status, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', ...headers });
  res.end(data);
}
const err = (res, status, error) => send(res, status, { error });

async function readJson(req) {
  let raw = '';
  for await (const chunk of req) raw += chunk;
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

function authed(req) {
  const header = req.headers['x-rondo-session'];
  const cookie = /(?:^|;\s*)rondo_session=([^;]+)/.exec(req.headers.cookie || '')?.[1];
  return sessions.has(header) || sessions.has(cookie);
}

const insideCard = (p) => typeof p === 'string' && !p.includes('..') && FOLDERS.some((f) => p.startsWith(f + '/'));
const extOf = (p) => (p.split('.').pop() || '').toLowerCase();

function uniquePath(folder, name) {
  const taken = new Set([...songs, ...videos, ...pending].map((x) => x.path));
  const dot = name.lastIndexOf('.');
  const base = dot > 0 ? name.slice(0, dot) : name, ext = dot > 0 ? name.slice(dot) : '';
  let p = `${folder}/${name}`;
  for (let n = 2; taken.has(p); n++) p = `${folder}/${base} (${n})${ext}`;
  return p;
}

async function upload(req, res, url) {
  const name = url.searchParams.get('name');
  const folder = url.searchParams.get('folder') || MUSIC;
  const length = Number(req.headers['content-length']);
  if (!name || name.includes('/') || name.includes('..')) return err(res, 400, 'name');
  if (!FOLDERS.includes(folder) && !insideCard(folder)) return err(res, 400, 'folder');
  if (!ALLOWED.includes(extOf(name))) return err(res, 415, 'type');
  if (!Number.isFinite(length) || req.headers['transfer-encoding']) return err(res, 411, 'length');
  const free = 62_500_000_000 - [...songs, ...videos, ...pending].reduce((n, x) => n + x.size, 0);
  if (length > free) return err(res, 507, 'full');
  if (name.toLowerCase().includes('fail')) { req.resume(); return err(res, 500, 'write failed'); } // to see an error row

  // read the body at about the Y1's Wi-Fi speed, so the page shows real progress
  const started = Date.now();
  let got = 0;
  await new Promise((resolve, reject) => {
    req.on('data', (chunk) => {
      got += chunk.length;
      const due = started + (got / UPLOAD_BYTES_PER_SEC) * 1000;
      const wait = due - Date.now();
      if (wait > 5) { req.pause(); setTimeout(() => req.resume(), wait); }
    });
    req.on('end', resolve);
    req.on('error', reject);
    req.on('aborted', () => reject(new Error('aborted')));
  }).catch(() => null);
  if (got !== length) return; // client gave up
  const p = uniquePath(folder, name);
  const ext = extOf(name);
  const stem = name.replace(/\.[^.]+$/, '');
  if (AUDIO.includes(ext)) pending.push({ kind: 'song', path: p, title: stem, artist: '', album: '', track: 0, durationSec: int(150, 300), size: length, art: false, modified: Date.now() });
  else if (VIDEO.includes(ext)) pending.push({ kind: 'video', path: p, title: stem, durationSec: int(60, 900), size: length, modified: Date.now() });
  console.log(`  upload: ${p} (${(length / 1e6).toFixed(1)} MB)`);
  send(res, 200, { path: p });
}

async function api(req, res, url) {
  const { pathname } = url;
  const method = req.method;

  if (pathname === '/api/pair' && method === 'POST') {
    const body = await readJson(req);
    if (body?.code === code) {
      const token = crypto.randomBytes(18).toString('base64url');
      sessions.add(token);
      return send(res, 200, { session: token }, { 'Set-Cookie': `rondo_session=${token}; Path=/` });
    }
    wrong++;
    if (wrong >= 5) newCode();
    return err(res, 403, 'code');
  }
  if (!authed(req)) return err(res, 401, 'pair');

  if (pathname === '/api/art' || pathname === '/api/thumb') {
    const key = url.searchParams.get('path') || url.searchParams.get('u') || '';
    const song = songs.find((s) => s.path === key);
    const video = videos.find((v) => v.path === key);
    if (pathname === '/api/art' && !(song?.art || video)) return err(res, 404, 'none');
    if (pathname === '/api/thumb' && !key) return err(res, 400, 'u');
    return send(res, 200, artSvg(song ? song.album + song.artist : key, !!video), { 'Content-Type': 'image/svg+xml', 'Cache-Control': 'max-age=86400' });
  }

  if (pathname === '/api/status' && method === 'GET') {
    const used = [...songs, ...videos].reduce((n, x) => n + x.size, 0) + 3_100_000_000;
    return send(res, 200, {
      device: { name: 'Rondo', version: '1.0.0', model: 'Innioasis Y1' },
      battery: { level: Math.max(4, 87 - Math.floor((Date.now() - BOOT) / 120000)), charging: false },
      storage: { freeBytes: 62_500_000_000 - used, totalBytes: 62_500_000_000 },
      wifi: wifi.current ? { ssid: wifi.current.ssid, signal: wifi.current.signal, ip: wifi.current.ip } : null,
      player: playerJson(),
    });
  }

  if (pathname === '/api/player' && method === 'POST') {
    const body = await readJson(req);
    if (!body) return err(res, 400, 'json');
    if (!player.queue.length) return err(res, 409, 'nothing playing');
    const p = position();
    switch (body.action) {
      case 'toggle': setPosition(p); player.playing = !player.playing; break;
      case 'next': player.index = (player.index + 1) % player.queue.length; setPosition(0); break;
      case 'previous':
        if (p > 3000) setPosition(0);
        else { player.index = (player.index - 1 + player.queue.length) % player.queue.length; setPosition(0); }
        break;
      case 'seek': if (!Number.isFinite(body.positionMs)) return err(res, 400, 'positionMs'); setPosition(Math.max(0, Math.min(body.positionMs, player.queue[player.index].durationMs))); break;
      case 'volume': if (!Number.isFinite(body.volume)) return err(res, 400, 'volume'); player.volume = Math.max(0, Math.min(player.maxVolume, Math.round(body.volume))); break;
      default: return err(res, 400, 'action');
    }
    return send(res, 200, playerJson());
  }
  if (pathname === '/api/player/play' && method === 'POST') {
    const body = await readJson(req);
    const i = songs.findIndex((s) => s.path === body?.path);
    const v = videos.find((x) => x.path === body?.path);
    if (i < 0 && !v) return err(res, 404, 'not found');
    if (v) player.queue = [{ title: v.title, subtitle: 'Library', art: `/api/art?path=${encodeURIComponent(v.path)}&size=300`, durationMs: v.durationSec * 1000, source: 'device' }];
    else player.queue = songs.map(songItem);
    player.index = v ? 0 : i; player.playing = true; setPosition(0);
    return send(res, 200, playerJson());
  }

  if (pathname === '/api/library' && method === 'GET') {
    return send(res, 200, { songs, videos, scanning, folders: FOLDERS });
  }
  if (pathname === '/api/library/upload' && method === 'PUT') return upload(req, res, url);
  if (pathname === '/api/library' && method === 'DELETE') {
    const p = url.searchParams.get('path');
    if (!insideCard(p) || !ALLOWED.includes(extOf(p))) return err(res, 403, 'path');
    const before = songs.length + videos.length;
    songs = songs.filter((s) => s.path !== p);
    videos = videos.filter((v) => v.path !== p);
    if (songs.length + videos.length === before) return err(res, 404, 'not found');
    return send(res, 200, { ok: true });
  }
  if (pathname === '/api/library/rescan' && method === 'POST') {
    if (!scanning) {
      scanning = true;
      setTimeout(() => {
        for (const f of pending) (f.kind === 'video' ? videos : songs).push(Object.fromEntries(Object.entries(f).filter(([k]) => k !== 'kind')));
        pending = [];
        scanning = false;
      }, 3500);
    }
    return send(res, 200, { ok: true });
  }

  if (pathname === '/api/settings') {
    if (method === 'GET') return send(res, 200, settings);
    if (method === 'PUT') {
      const b = await readJson(req);
      if (!b) return err(res, 400, 'json');
      const next = { ...settings };
      if ('videoQuality' in b) { if (![null, 360, 480, 720].includes(b.videoQuality)) return err(res, 400, 'videoQuality'); next.videoQuality = b.videoQuality; }
      if ('brightness' in b) { if (!(b.brightness >= 10 && b.brightness <= 100)) return err(res, 400, 'brightness'); next.brightness = Math.round(b.brightness); }
      if ('screenTimeoutSec' in b) { if (![15, 30, 60, 120, 300, 600].includes(b.screenTimeoutSec)) return err(res, 400, 'screenTimeoutSec'); next.screenTimeoutSec = b.screenTimeoutSec; }
      if ('lyricsOnOpen' in b) next.lyricsOnOpen = !!b.lyricsOnOpen;
      if ('wheelAcceleration' in b) { if (!['off', 'normal', 'fast'].includes(b.wheelAcceleration)) return err(res, 400, 'wheelAcceleration'); next.wheelAcceleration = b.wheelAcceleration; }
      if ('wheelVibration' in b) { if (!['off', 'light', 'normal', 'strong'].includes(b.wheelVibration)) return err(res, 400, 'wheelVibration'); next.wheelVibration = b.wheelVibration; }
      if ('clickSound' in b) next.clickSound = !!b.clickSound;
      settings = next;
      return send(res, 200, settings);
    }
  }

  if (pathname === '/api/theme') {
    if (method === 'GET') return send(res, 200, themeJson());
    if (method === 'PUT') {
      const b = await readJson(req);
      if (!b) return err(res, 400, 'json');
      if ('accent' in b) { if (!/^#[0-9a-f]{6}$/i.test(b.accent)) return err(res, 400, 'accent'); theme.accent = b.accent.toLowerCase(); }
      if ('appearance' in b) theme.appearance = b.appearance === 'dark' ? 'dark' : 'light';
      if ('textScale' in b) theme.textScale = [1.25, 1.4, 1.55].reduce((best, s) => (Math.abs(s - b.textScale) < Math.abs(best - b.textScale) ? s : best), 1.4);
      if (Array.isArray(b.menu)) {
        const seen = b.menu.filter((m) => LABELS[m?.id]).filter((m, i, a) => a.findIndex((x) => x.id === m.id) === i);
        const menu = [...seen.map((m) => ({ id: m.id, visible: m.visible !== false })), ...Object.keys(LABELS).filter((id) => !seen.some((m) => m.id === id)).map((id) => ({ id, visible: true }))];
        theme.menu = menu.map((m) => (m.id === 'settings' ? { ...m, visible: true } : m));
      }
      console.log(`  theme: ${theme.accent} ${theme.appearance} ${theme.textScale} [${theme.menu.map((m) => (m.visible ? m.id : `(${m.id})`)).join(' ')}]`);
      return send(res, 200, themeJson());
    }
  }

  if (pathname === '/api/radio') {
    if (method === 'GET') return send(res, 200, radioJson());
    if (method === 'PUT') {
      const b = await readJson(req);
      if (!b) return err(res, 400, 'json');
      if ('on' in b) radio.on = !!b.on;
      if ('band' in b) { radio.band = b.band === 'japan' ? 'japan' : 'world'; const { min, max } = radioRange(); radio.frequency = Math.min(max, Math.max(min, radio.frequency)); }
      if ('frequency' in b) { const { min, max } = radioRange(); radio.frequency = Math.min(max, Math.max(min, Math.round(b.frequency))); }
      if ('muted' in b) radio.muted = !!b.muted;
      if ('volume' in b) radio.volume = Math.min(13, Math.max(0, Math.round(b.volume)));
      if ('speaker' in b) radio.speaker = !!b.speaker;
      return send(res, 200, radioJson());
    }
  }
  if (pathname === '/api/radio/seek' && method === 'POST') {
    const b = await readJson(req);
    const list = Object.keys(STATIONS).map(Number).sort((x, y) => x - y);
    const next = b?.up ? list.find((f) => f > radio.frequency) ?? list[0] : [...list].reverse().find((f) => f < radio.frequency) ?? list.at(-1);
    radio.seeking = true;
    setTimeout(() => { radio.frequency = next; radio.seeking = false; }, 1200);
    return send(res, 200, radioJson());
  }
  if (pathname === '/api/radio/scan' && method === 'POST') {
    radio.scanning = true;
    setTimeout(() => { radio.found = Object.keys(STATIONS).map(Number).concat([945]).sort((x, y) => x - y); radio.scanning = false; }, 6000);
    return send(res, 200, radioJson());
  }
  if (pathname === '/api/radio/presets' && method === 'PUT') {
    const b = await readJson(req);
    if (!Array.isArray(b?.presets)) return err(res, 400, 'presets');
    radio.presets = b.presets.map((p) => ({ frequency: Math.round(p.frequency), name: String(p.name ?? '').slice(0, 40) }));
    return send(res, 200, radioJson());
  }

  if (pathname === '/api/bluetooth') {
    if (method === 'GET') return send(res, 200, btJson());
    if (method === 'PUT') {
      const b = await readJson(req);
      bt.state = b?.enabled ? 'turning_on' : 'turning_off';
      setTimeout(() => { bt.state = b?.enabled ? 'on' : 'off'; }, 1500);
      return send(res, 200, btJson());
    }
  }
  if (pathname === '/api/bluetooth/search' && method === 'POST') {
    bt.searching = true;
    setTimeout(() => { bt.searching = false; }, 8000);
    return send(res, 200, btJson());
  }
  const btAction = pathname.match(/^\/api\/bluetooth\/(connect|disconnect|forget)$/);
  if (btAction && method === 'POST') {
    const b = await readJson(req);
    const mine = bt.mine.find((d) => d.address === b?.address);
    const other = bt.others.find((d) => d.address === b?.address);
    if (btAction[1] === 'forget' && mine) bt.mine = bt.mine.filter((d) => d !== mine);
    if (btAction[1] === 'disconnect' && mine) mine.status = 'disconnected';
    if (btAction[1] === 'connect') {
      const d = mine ?? (other && { ...other, status: 'connecting' });
      if (!d) return err(res, 404, 'not found');
      if (other) { bt.others = bt.others.filter((x) => x !== other); delete d.pairing; bt.mine.push(d); }
      d.status = 'connecting';
      setTimeout(() => { bt.mine.forEach((x) => { if (x !== d && x.status === 'connected') x.status = 'disconnected'; }); d.status = 'connected'; }, 2000);
    }
    return send(res, 200, btJson());
  }

  if (pathname === '/api/wifi' && method === 'GET') return send(res, 200, wifiJson());
  if (pathname === '/api/wifi/join' && method === 'POST') {
    const b = await readJson(req);
    const net = wifi.nearby.find((n) => n.ssid === b?.ssid);
    if (!net) return err(res, 404, 'not found');
    if (net.security === 'eap') return err(res, 400, 'eap not supported');
    const saved = wifi.saved.some((s) => s.ssid === net.ssid);
    const ok = net.security === 'open' || saved || (typeof b.password === 'string' && b.password.length >= 8);
    setTimeout(() => {
      if (!ok) return; // wrong password: stays where it was
      wifi.current = { ssid: net.ssid, signal: net.signal, ip: '192.168.1.' + int(20, 200) };
      if (!saved) wifi.saved.push({ id: nextWifiId++, ssid: net.ssid });
    }, 3000);
    return send(res, 200, { ok: true });
  }
  if (pathname === '/api/wifi/saved' && method === 'DELETE') {
    const id = Number(url.searchParams.get('id'));
    const net = wifi.saved.find((s) => s.id === id);
    if (!net) return err(res, 404, 'not found');
    wifi.saved = wifi.saved.filter((s) => s.id !== id);
    if (wifi.current?.ssid === net.ssid) wifi.current = null;
    return send(res, 200, { ok: true });
  }

  if (pathname === '/api/server') {
    if (method === 'GET') return send(res, 200, { url: server.url, tokenSet: !!server.token });
    if (method === 'PUT') {
      const b = await readJson(req);
      if (!b) return err(res, 400, 'json');
      if ('url' in b) { if (typeof b.url !== 'string' || !/^https?:\/\/[^\s/]+/.test(b.url)) return err(res, 400, 'url'); server.url = b.url.replace(/\/+$/, ''); }
      if ('token' in b) server.token = b.token || null;
      return send(res, 200, { url: server.url, tokenSet: !!server.token });
    }
  }
  if (pathname === '/api/server/test' && method === 'POST') {
    await sleep(500 + Math.random() * 900);
    if (/bad|invalid|localhost/.test(server.url)) return send(res, 200, { ok: false, error: 'unreachable' });
    if (!server.token) return send(res, 200, { ok: false, error: 'unauthorized' });
    return send(res, 200, { ok: true, latencyMs: 120 + Math.floor(Math.random() * 140), ytdlp: '2026.10.29.233517', cookies: true });
  }

  return err(res, 404, 'not found');
}

const BOOT = Date.now();
http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');
  try {
    if (url.pathname.startsWith('/api/')) {
      await delay();
      return await api(req, res, url);
    }
    if (url.pathname.startsWith('/fonts/')) return serveFile(res, path.join(FONTS, path.basename(url.pathname)));
    const rel = url.pathname === '/' ? 'index.html' : path.normalize(url.pathname).replace(/^[/\\]+/, '');
    if (rel.includes('..')) return send(res, 400, 'Bad path', { 'Content-Type': 'text/plain' });
    return serveFile(res, path.join(WEB, rel));
  } catch (e) {
    console.error(e);
    if (!res.headersSent) err(res, 500, 'mock error');
  }
}).listen(PORT, () => {
  console.log(`Rondo dashboard mock on http://localhost:${PORT}${EMPTY ? ' (empty)' : ''}${SLOW ? ' (slow)' : ''}`);
  console.log(`  ${songs.length} songs, ${videos.length} videos. Upload a file with "fail" in its name to see an error.`);
  newCode();
});
