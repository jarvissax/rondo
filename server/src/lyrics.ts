// Lyrics for the Now Playing screen. A request with a video id is a YouTube Music song: its own
// lyrics tab comes first, through youtubei.js (timed lines when YouTube has them, else plain text).
// LRCLIB (synced LRC when it has it) answers when YouTube Music has none or no id was sent. A
// failing source is one warning line and a fall-through to the next, never a 5xx. Lyrics text is
// never logged.

import type { FastifyInstance } from "fastify";
import { request } from "undici";
import { isVideoId, type MusicLyrics } from "./browse.js";
import { TtlCache } from "./ttl.js";
import { dispatcher } from "./upstream.js";

export interface LyricLine {
  t: number; // ms from the start of the song
  text: string; // "" is a pause
}

export interface Lyrics {
  synced: LyricLine[] | null;
  plain: string | null;
  instrumental: boolean;
  source: "lrclib" | "youtube" | null;
}

export interface LyricsRequest {
  title: string;
  artist: string;
  album?: string;
  durationSec?: number;
  id?: string;
}

// what LRCLIB is asked for: a track title and (maybe) an artist
export interface Query {
  title: string;
  artist: string;
}

const none = (): Lyrics => ({ synced: null, plain: null, instrumental: false, source: null });

const HIT_TTL_MS = 24 * 3600_000;
const MISS_TTL_MS = 3600_000;
// an answer reached past a failing source: ask again soon rather than keep a miss for an hour
const DEGRADED_TTL_MS = 5 * 60_000;
const CACHE_ENTRIES = 500;

export const LRCLIB_BASE = "https://lrclib.net";
// LRCLIB asks every client to name itself; an identified client gets ~90 requests/min per IP
const USER_AGENT = "Rondo server (https://github.com/jarvissax/rondo)";
const LRCLIB_TIMEOUT_MS = 6000;
// LRCLIB matches /api/get within ±2 s; search results are judged with a little more slack
const DURATION_SLACK_S = 3;
const MAX_FIELD = 200;

// LRC parsing

// [mm:ss], [mm:ss.x], [mm:ss.xx], [mm:ss.xxx]; some files put ':' before the fraction
const STAMP = /^\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]/;
// [ar:…] [ti:…] [al:…] [by:…] [length:…] [offset:…] [#:…]
const TAG = /^\[([a-z#]+):([^\]]*)\]$/i;
// enhanced LRC word timings inside a line: <00:12.34>
const WORD_STAMP = /<\d{1,3}:\d{1,2}(?:[.:]\d{1,3})?>/g;

// LRC -> lines in time order. Metadata tags are dropped and [offset:] applied (positive = lyrics
// sooner); a line with several stamps appears at each of them.
export function parseLrc(lrc: string): LyricLine[] {
  const lines: LyricLine[] = [];
  let offset = 0;
  for (const raw of lrc.split(/\r?\n/)) {
    let rest = raw.trim();
    const stamps: number[] = [];
    for (let m = STAMP.exec(rest); m; m = STAMP.exec(rest)) {
      const frac = m[3] ?? "";
      stamps.push((Number(m[1]) * 60 + Number(m[2])) * 1000 + (frac ? Number(frac.padEnd(3, "0")) : 0));
      rest = rest.slice(m[0].length).trimStart();
    }
    if (stamps.length) {
      const text = rest.replace(WORD_STAMP, "").replace(/\s+/g, " ").trim();
      for (const t of stamps) lines.push({ t, text });
      continue;
    }
    const tag = TAG.exec(rest);
    if (tag?.[1]?.toLowerCase() === "offset") {
      const ms = Number((tag[2] ?? "").trim());
      if (Number.isFinite(ms)) offset = ms;
    }
  }
  // Array.sort is stable: lines sharing a stamp keep their file order
  return lines.map((l) => ({ t: Math.max(0, Math.round(l.t - offset)), text: l.text })).sort((a, b) => a.t - b.t);
}

// Query cleanup: YouTube titles carry upload noise that LRCLIB track names do not

// words that describe the upload, never the song: "(Official Music Video)", "[Lyrics]", "(Remastered 2011)"
const NOISE_WORDS = new Set([
  "official", "music", "video", "videoclip", "audio", "lyric", "lyrics", "visualizer", "visualiser",
  "remaster", "remastered", "hd", "hq", "4k", "8k", "uhd", "1080p", "720p", "mv", "m/v", "clip",
  "explicit", "resmi", "klip", "oficial", "officiel", "letra", "paroles", "şarkı", "sözleri",
]);
// brackets that credit people rather than name the song: "(feat. X)", "(with X)", "(prod. X)"
const CREDIT = /^(?:feat\.?|ft\.?|featuring|with|prod\.?|produced by)\s/i;
// what follows " - " when the dash belongs to the title: "Song - Live", "Song - Acoustic Version"
const VERSION = /\b(?:live|acoustic|remix|mix|edit|version|instrumental|demo|session|unplugged|cover)\b/i;
const BRACKETS = /\s*(?:\(([^()]*)\)|\[([^[\]]*)\]|【([^】]*)】)/g;

const isNoise = (inner: string): boolean => {
  const words = inner.toLowerCase().split(/[\s,.:;!_-]+/).filter(Boolean);
  return words.length > 0 && words.every((w) => NOISE_WORDS.has(w) || /^(?:19|20)\d{2}$/.test(w) || w === "&");
};

// accent-, case- and punctuation-blind form for comparing names (Turkish dotless ı included)
export const fold = (s: string): string =>
  s.normalize("NFKD").replace(/\p{M}/gu, "").toLowerCase().replace(/ı/g, "i").replace(/[^\p{L}\p{N}]/gu, "");

const unquote = (s: string): string => s.replace(/^["“”«»]+|["“”«»]+$/g, "").trim();

// title without noise brackets, featured artists, " | …" tails, HD/4K and " - Remastered" suffixes
export function cleanTitle(raw: string): string {
  let t = raw.replace(/\s+[–—]\s+/g, " - ").replace(/\s+/g, " ").trim();
  t = t.split(" | ")[0] ?? t;
  t = t.replace(BRACKETS, (whole, a?: string, b?: string, c?: string) => {
    const inner = (a ?? b ?? c ?? "").trim();
    return isNoise(inner) || CREDIT.test(`${inner} `) ? "" : whole;
  });
  t = t.replace(/\s+(?:feat\.?|ft\.?|featuring)\s+.*?(?=\s+-\s+|\s*[([【]|$)/gi, "");
  for (let prev = ""; prev !== t; ) {
    prev = t;
    t = t
      .replace(/\s+(?:hd|hq|4k|8k|uhd|1080p|720p)$/i, "")
      .replace(/\s+official\s+(?:music\s+|lyric\s+)?(?:video|audio)$/i, "")
      .replace(/\s+-\s+(?:(?:\d{4}\s+)?remaster(?:ed)?(?:\s+\d{4})?(?:\s+version)?|radio edit|single version|album version|mono|stereo)$/i, "")
      .trim();
  }
  return t.replace(/\s+/g, " ").trim() || raw.trim();
}

// the first of several artists, without YouTube's channel suffixes ("X - Topic", "XVEVO")
export function mainArtist(raw: string): string {
  const a = raw.replace(/\s+-\s+topic$/i, "").replace(/vevo$/i, "").trim();
  return (a.split(/\s*(?:,|&|•|·)\s*|\s+x\s+|\s+(?:feat\.?|ft\.?|featuring)\s+/)[0] ?? a).trim();
}

// "Daft Punk" ~ "DaftPunk" ~ "Daft Punk feat. X"; names under 3 letters must match exactly
const sameArtist = (a: string, b: string): boolean => {
  const x = fold(a);
  const y = fold(b);
  if (!x || !y) return false;
  return x === y || (Math.min(x.length, y.length) >= 3 && (x.includes(y) || y.includes(x)));
};

// Queries to try, best first: the cleaned title (split from "Artist - Title" where the artist is in
// the title), an alternative reading when that split is a guess, the title without any brackets,
// and the title and artist as given.
export function lyricsQueries(rawTitle: string, rawArtist = ""): Query[] {
  const title = cleanTitle(rawTitle);
  const artist = mainArtist(rawArtist);
  const out: Query[] = [];
  const dash = title.indexOf(" - ");
  if (dash > 0) {
    const left = title.slice(0, dash).trim();
    const right = unquote(title.slice(dash + 3));
    if (!artist || sameArtist(left, artist)) out.push({ title: right, artist: mainArtist(left) });
    else if (sameArtist(right, artist)) out.push({ title: unquote(left), artist }); // "Title - Artist"
    else if (VERSION.test(right.replace(BRACKETS, ""))) out.push({ title: unquote(title), artist });
    // a label or fan channel uploading "Artist - Title"; or a title that has a dash of its own
    else out.push({ title: right, artist: mainArtist(left) }, { title: unquote(title), artist });
  } else {
    out.push({ title: unquote(title), artist });
  }
  const first = out[0]!;
  const bare = first.title.replace(BRACKETS, "").trim();
  if (bare) out.push({ title: bare, artist: first.artist });
  out.push({ title: rawTitle.replace(/\s+/g, " ").trim(), artist: rawArtist.replace(/\s+/g, " ").trim() });
  const seen = new Set<string>();
  return out.filter((q) => {
    const key = `${fold(q.title)}|${fold(q.artist)}`;
    return !!fold(q.title) && !seen.has(key) && !!seen.add(key);
  });
}

// LRCLIB

interface LrclibEntry {
  duration?: unknown;
  instrumental?: unknown;
  plainLyrics?: unknown;
  syncedLyrics?: unknown;
}

const hasText = (s: unknown): s is string => typeof s === "string" && s.trim() !== "";
const tidy = (s: string): string => s.replace(/\r\n?/g, "\n").replace(/[ \t]+$/gm, "").trim();

// The search result to use: synced lyrics within ±3 s of the song, then any synced lyrics, then
// plain lyrics (each tier closest in duration first), then an instrumental record within ±3 s.
export function pickMatch(entries: unknown[], durationSec?: number): LrclibEntry | undefined {
  const list = entries.filter((e): e is LrclibEntry => !!e && typeof e === "object");
  const gap = (e: LrclibEntry): number =>
    durationSec === undefined ? 0 : typeof e.duration === "number" ? Math.abs(e.duration - durationSec) : Infinity;
  const near = (e: LrclibEntry): boolean => durationSec !== undefined && gap(e) <= DURATION_SLACK_S;
  const tiers: Array<(e: LrclibEntry) => boolean> = [
    (e) => hasText(e.syncedLyrics) && near(e),
    (e) => hasText(e.syncedLyrics),
    (e) => hasText(e.plainLyrics),
    (e) => e.instrumental === true && near(e),
  ];
  for (const tier of tiers) {
    let best: LrclibEntry | undefined;
    for (const e of list) if (tier(e) && (!best || gap(e) < gap(best))) best = e;
    if (best) return best;
  }
  return undefined;
}

// an LRCLIB record -> the app's answer; undefined when it holds nothing usable
export function fromLrclib(raw: unknown): Lyrics | undefined {
  if (!raw || typeof raw !== "object") return undefined;
  const e = raw as LrclibEntry;
  const parsed = hasText(e.syncedLyrics) ? parseLrc(e.syncedLyrics) : [];
  const synced = parsed.some((l) => l.text) ? parsed : null;
  const plain = hasText(e.plainLyrics) ? tidy(e.plainLyrics) : synced ? tidy(synced.map((l) => l.text).join("\n")) : null;
  if (synced || plain) return { synced, plain, instrumental: false, source: "lrclib" };
  if (e.instrumental === true) return { synced: null, plain: null, instrumental: true, source: "lrclib" };
  return undefined;
}

// one LRCLIB GET; any status comes back (404 is an answer), network errors and timeouts throw
export type JsonGet = (url: URL) => Promise<{ status: number; body?: unknown; retryAfterS?: number }>;

export class LrclibError extends Error {
  constructor(
    readonly status: number,
    readonly retryAfterS?: number,
  ) {
    super(`HTTP ${status}`);
  }
}

export const lrclibGet: JsonGet = async (url) => {
  const res = await request(url, {
    dispatcher,
    headers: { "user-agent": USER_AGENT, accept: "application/json" },
    signal: AbortSignal.timeout(LRCLIB_TIMEOUT_MS),
  });
  if (res.statusCode !== 200) {
    await res.body.dump();
    const retryAfterS = Number(res.headers["retry-after"]);
    return { status: res.statusCode, ...(retryAfterS > 0 ? { retryAfterS } : {}) };
  }
  return { status: 200, body: await res.body.json() };
};

function lrclibUrl(path: string, params: Record<string, string | undefined>): URL {
  const url = new URL(path, LRCLIB_BASE);
  for (const [k, v] of Object.entries(params)) if (v) url.searchParams.set(k, v);
  return url;
}

// /api/get with the first query (LRCLIB's exact match on title, artist, album and duration), then
// /api/search with each query until one has a usable record. Throws LrclibError on anything but
// 200/404, so a broken LRCLIB is not mistaken for "no lyrics".
export async function lrclibLookup(get: JsonGet, queries: Query[], album?: string, durationSec?: number): Promise<Lyrics | undefined> {
  const first = queries[0];
  if (first?.artist) {
    const duration = durationSec !== undefined ? Math.round(durationSec) : undefined;
    const res = await get(
      lrclibUrl("/api/get", {
        track_name: first.title,
        artist_name: first.artist,
        album_name: album,
        duration: duration !== undefined && duration >= 1 && duration <= 3600 ? String(duration) : undefined,
      }),
    );
    if (res.status === 200) {
      const found = fromLrclib(res.body);
      if (found) return found;
    } else if (res.status !== 404) {
      throw new LrclibError(res.status, res.retryAfterS);
    }
  }
  for (const q of queries) {
    const res = await get(lrclibUrl("/api/search", { track_name: q.title, artist_name: q.artist }));
    if (res.status !== 200) throw new LrclibError(res.status, res.retryAfterS);
    const match = pickMatch(Array.isArray(res.body) ? res.body : [], durationSec);
    const found = match && fromLrclib(match);
    if (found) return found;
  }
  return undefined;
}

// The service

// a song's lyrics from its YouTube Music lyrics tab (Browse.musicLyrics); undefined when it has none
export type YoutubeLyrics = (videoId: string) => Promise<MusicLyrics | undefined>;

export interface LyricsTiming {
  youtubeTimeoutMs: number;
  retryDelayMs: number;
}

const DEFAULT_TIMING: LyricsTiming = { youtubeTimeoutMs: 8000, retryDelayMs: 300 };

export interface LyricsDeps {
  lrclib: JsonGet;
  youtube: YoutubeLyrics;
  timing?: LyricsTiming;
  log?: { warn: (obj: object, msg: string) => void };
  now?: () => number;
}

interface Found {
  lyrics: Lyrics;
  degraded: boolean; // a source failed on the way to this answer
}

const sleep = (ms: number) => new Promise((ok) => setTimeout(ok, ms));

// LRCLIB sheds load with 503 (and rate-limits with 429) plus Retry-After, usually 1 s (seen all
// through its recovery on 2026-10-31). Wait that long, plus up to 30% jitter as its docs ask, and try
// once more; a longer Retry-After pauses LRCLIB instead (LyricsService).
const RETRY_AFTER_MAX_S = 2;
export function honouringRetryAfter(get: JsonGet): JsonGet {
  return async (url) => {
    const res = await get(url);
    const wait = res.retryAfterS;
    if ((res.status === 503 || res.status === 429) && wait && wait <= RETRY_AFTER_MAX_S) {
      await sleep(wait * 1000 * (1 + Math.random() * 0.3));
      return get(url);
    }
    return res;
  };
}

function withTimeout<T>(p: Promise<T>, ms: number): Promise<T> {
  return new Promise((ok, fail) => {
    const timer = setTimeout(() => fail(new Error(`timed out after ${ms} ms`)), ms);
    p.then(
      (v) => {
        clearTimeout(timer);
        ok(v);
      },
      (e) => {
        clearTimeout(timer);
        fail(e);
      },
    );
  });
}

const errText = (err: unknown): string => String((err as Error)?.message ?? err).slice(0, 200);

const normKey = (s: string | undefined): string => (s ?? "").normalize("NFC").toLowerCase().replace(/\s+/g, " ").trim();

export class LyricsService {
  private readonly cache: TtlCache<Found>;
  private readonly timing: LyricsTiming;
  private readonly now: () => number;
  // LRCLIB answered 429/503 with Retry-After: leave it alone until then
  private lrclibPausedUntil = 0;

  constructor(private readonly deps: LyricsDeps) {
    this.timing = deps.timing ?? DEFAULT_TIMING;
    this.now = deps.now ?? Date.now;
    const ttl = (f: Found) => (f.degraded ? DEGRADED_TTL_MS : f.lyrics.source ? HIT_TTL_MS : MISS_TTL_MS);
    this.cache = new TtlCache<Found>(ttl, CACHE_ENTRIES, this.now);
  }

  async find(req: LyricsRequest): Promise<Lyrics> {
    const duration = req.durationSec !== undefined ? String(Math.round(req.durationSec)) : "";
    const key = [normKey(req.title), normKey(req.artist), normKey(req.album), duration, req.id ?? ""].join("\u0000");
    return (await this.cache.get(key, () => this.load(req))).lyrics;
  }

  // never rejects: every failure is logged and becomes the next source or the empty answer
  private async load(req: LyricsRequest): Promise<Found> {
    let degraded = false;
    if (req.id) {
      const id = req.id;
      const ask = () => withTimeout(this.deps.youtube(id), this.timing.youtubeTimeoutMs);
      try {
        // one retry, as for browse: a lone "fetch failed" between the VPS and YouTube is common
        const found = await ask().catch(async () => {
          await sleep(this.timing.retryDelayMs);
          return ask();
        });
        if (found && hasText(found.plain)) {
          const synced = found.synced?.length ? found.synced : null;
          // partial: the timed version failed, so ask again soon rather than keep plain text for a day
          return { lyrics: { synced, plain: tidy(found.plain), instrumental: false, source: "youtube" }, degraded: !!found.partial };
        }
      } catch (err) {
        degraded = true;
        this.deps.log?.warn({ source: "youtube", err: errText(err) }, "lyrics source failed");
      }
    }
    if (this.now() >= this.lrclibPausedUntil) {
      try {
        const get = honouringRetryAfter(this.deps.lrclib);
        const found = await lrclibLookup(get, lyricsQueries(req.title, req.artist), req.album, req.durationSec);
        if (found) return { lyrics: found, degraded };
      } catch (err) {
        degraded = true;
        if (err instanceof LrclibError && err.retryAfterS) this.lrclibPausedUntil = this.now() + err.retryAfterS * 1000;
        this.deps.log?.warn({ source: "lrclib", err: errText(err) }, "lyrics source failed");
      }
    } else {
      degraded = true;
    }
    return { lyrics: none(), degraded };
  }
}

const field = (v: unknown): string => (typeof v === "string" ? v.replace(/\s+/g, " ").trim().slice(0, MAX_FIELD) : "");

export function registerLyricsRoutes(app: FastifyInstance, deps: { lyrics: LyricsService }): void {
  // GET /lyrics?title=&artist=&album=&duration=<s>&id=<video id>: always 200 with the Lyrics shape
  app.get<{ Querystring: Record<string, unknown> }>("/lyrics", async (req, reply) => {
    const q = req.query;
    const title = field(q.title);
    if (!title) return reply.code(400).send({ error: "title" });
    const id = field(q.id);
    if (id && !isVideoId(id)) return reply.code(400).send({ error: "id" });
    const duration = Number(field(q.duration) || NaN);
    return deps.lyrics.find({
      title,
      artist: field(q.artist),
      album: field(q.album) || undefined,
      durationSec: duration > 0 && duration <= 24 * 3600 ? duration : undefined,
      id: id || undefined,
    });
  });
}
