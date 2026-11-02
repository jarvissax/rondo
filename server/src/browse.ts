// Everything the user browses: youtubei.js only (stream URLs come from yt-dlp, never from here).
// Every item is reduced to the fields the app renders; thumbnails go through /thumb so the Y1
// never contacts Google.

import { readFile, stat } from "node:fs/promises";
import type { FastifyInstance, FastifyReply } from "fastify";
import sharp from "sharp";
import { request } from "undici";
import { ClientType, Innertube } from "youtubei.js";
import type { Config } from "./config.js";
import type { LyricLine } from "./lyrics.js";
import { type Chunk, type Page, Pager } from "./paging.js";
import { DEFAULT_THUMB_PX, isThumbHost, sizedThumbUrl, thumbPx } from "./thumb.js";
import { TtlCache } from "./ttl.js";
import { type TvAccount, type TvItem, tvChunk, tvItems } from "./tv.js";
import { dispatcher } from "./upstream.js";

export interface Item {
  kind: "song" | "video" | "album" | "artist" | "playlist";
  id: string;
  title: string;
  subtitle: string;
  durationSec?: number;
  thumb?: string;
}

export interface Section {
  title: string;
  items: Item[];
}

// a playlist or album page
export interface Collection {
  title: string;
  subtitle: string;
  thumb?: string;
  items: Item[];
}

const MUSIC_TYPES = new Set(["song", "video", "album", "artist", "playlist"]);

// a home page gathers YouTube Music pages (~3 shelves each) until it has this many sections
const HOME_SECTIONS = 6;
const SECTION_ITEMS = 20;
// a list page gathers YouTube pages until it has this many items (a page of shorts reduces to few)
const PAGE_ITEMS = 10;
// lists that are not paged (album, up next)
const LIST_ITEMS = 100;
const CACHE_TTL_MS = 120_000;
// /thumb asks Google for twice the output size and downscales it (lanczos3): sharper than Google's
// own resize, and full-resolution color for the 4:4:4 JPEG
const THUMB_OVERSAMPLE = 2;
// YouTube Music's Android app gets lyrics with line timings. youtubei.js 18.3 ships ANDROID_MUSIC
// 5.34.51, which YouTube answers with 400 (and 6.x with no lyrics), so the lyrics session claims a
// newer app version (checked 2026-10-31: 7.21.50 and 8.40.54 both work).
const ANDROID_MUSIC_VERSION = "7.21.50";
// a timed line ending this long before the next one starts is followed by a pause
const LYRIC_GAP_MS = 1000;

const VIDEO_ID = /^[A-Za-z0-9_-]{11}$/;
const PLAYLIST_ID = /^[A-Za-z0-9_-]{2,80}$/;
const ALBUM_ID = /^MPRE[A-Za-z0-9_-]{4,60}$/;
const ARTIST_ID = /^UC[A-Za-z0-9_-]{22}$/;

export const isVideoId = (id: unknown): id is string => typeof id === "string" && VIDEO_ID.test(id);

// browse ids of playlists carry a "VL" prefix; /music/playlist/:id takes the plain playlist id
export const playlistId = (id: string): string => (id.startsWith("VL") && id.length > 3 ? id.slice(2) : id);

// automatic radios ("RDAMVM…", "RD<videoId>") only exist as a watch queue; the curated
// "RDCLAK5uy_…" playlists are browseable like any other playlist
const isMix = (id: string): boolean => id.startsWith("RD") && !id.startsWith("RDCLAK");

// Netscape cookies.txt (as exported from a browser) -> Cookie header for youtube.com
export function cookiesToHeader(text: string, nowSec = Date.now() / 1000): string {
  const pairs: string[] = [];
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.startsWith("#HttpOnly_") ? raw.slice("#HttpOnly_".length) : raw;
    if (!line || line.startsWith("#")) continue;
    const f = line.split("\t");
    if (f.length < 7) continue;
    const [domain, , , , expiry, name, value] = f as [string, string, string, string, string, string, string];
    if (!/(^|\.)youtube\.com$/.test(domain)) continue;
    const exp = Number(expiry);
    if (exp > 0 && exp < nowSec) continue;
    pairs.push(`${name}=${value}`);
  }
  return pairs.join("; ");
}

// only links /thumb will serve, so the app never gets a thumbnail that answers 400
export function thumbLink(url: string | undefined): string | undefined {
  if (!url) return undefined;
  const abs = url.startsWith("//") ? `https:${url}` : url;
  try {
    if (!isThumbHost(new URL(abs))) return undefined;
  } catch {
    return undefined;
  }
  return `/thumb?u=${encodeURIComponent(abs)}`;
}

// youtubei.js node shapes, only the parts read here (they vary by renderer type)
type Node = Record<string, any>;

// youtubei.js Text renders an empty value as "N/A"; read .text so empty stays empty
const text = (v: any): string => (typeof v === "string" ? (v === "N/A" ? "" : v) : typeof v?.text === "string" ? v.text : "");

// "Album • 2013" -> "Album · 2013", the separator the app uses everywhere
const bullets = (s: string): string => s.replace(/\s*•\s*/g, " · ").trim();

const join = (parts: unknown[]): string => parts.filter((p) => typeof p === "string" && p).join(" · ");

// the largest thumbnail; /thumb sizes it for the screen (and asks Google's image server for more
// pixels when the URL carries a size token)
export function pickThumb(thumbs: any): string | undefined {
  const list: Array<{ url: string; width?: number }> = Array.isArray(thumbs) ? thumbs : (thumbs?.contents ?? []);
  let best: { url: string; width?: number } | undefined;
  for (const t of list) if (typeof t?.url === "string" && (!best || (t.width ?? 0) > (best.width ?? 0))) best = t;
  return best?.url;
}

// list items carry a MusicThumbnail (.contents), cards a bare array, older nodes .thumbnails
const nodeThumb = (n: Node | undefined): string | undefined =>
  thumbLink(pickThumb(n?.thumbnail?.contents ?? n?.thumbnails ?? n?.thumbnail));

const seconds = (d: any): number | undefined => {
  const s = typeof d === "number" ? d : d?.seconds;
  return typeof s === "number" && s > 0 ? s : undefined;
};

// artists on songs, authors on videos, a single author on album/playlist results
function people(n: Node): string {
  const list: Node[] = n.artists?.length ? n.artists : n.authors?.length ? n.authors : n.author ? [n.author] : [];
  return list.map((a) => text(a?.name ?? a)).filter(Boolean).join(", ");
}

function uniqueById(items: Array<Item | undefined>): Item[] {
  const seen = new Set<string>();
  return items.filter((i): i is Item => !!i && !seen.has(i.id) && !!seen.add(i.id));
}

export function musicItem(n: Node): Item | undefined {
  const kind = n.item_type === "non_music_track" ? "song" : n.item_type;
  const rawId = n.id ?? n.endpoint?.payload?.videoId ?? n.endpoint?.payload?.browseId;
  if (!MUSIC_TYPES.has(kind) || typeof rawId !== "string" || !rawId) return undefined;
  const id = kind === "playlist" ? playlistId(rawId) : rawId;
  const by = people(n);
  const sub = bullets(text(n.subtitle));
  const parts =
    kind === "song" || kind === "video" ? [by || sub, n.album?.name] : kind === "album" && by ? [by, n.year] : [by || sub];
  return {
    kind,
    id,
    title: text(n.title) || text(n.name),
    subtitle: join(parts),
    durationSec: seconds(n.duration),
    thumb: nodeThumb(n),
  };
}

// a row of the YouTube Music queue (up next, radio, mix)
export function panelItem(raw: Node): Item | undefined {
  const n: Node | undefined = raw.primary ?? raw; // PlaylistPanelVideoWrapper wraps the real row
  const id = n?.video_id ?? n?.endpoint?.payload?.videoId;
  if (!n || !isVideoId(id)) return undefined;
  return {
    kind: "song",
    id,
    title: text(n.title),
    subtitle: join([people({ artists: n.artists }) || text(n.author), n.album?.name]),
    durationSec: seconds(n.duration),
    thumb: nodeThumb(n),
  };
}

// a playable row of a playlist/album: always a song to the app, whatever YouTube calls it
export function trackItem(n: Node, fallbackThumb?: string): Item | undefined {
  const item = n.type === "PlaylistPanelVideo" || n.primary ? panelItem(n) : musicItem(n);
  if (!item || (item.kind !== "song" && item.kind !== "video") || !isVideoId(item.id)) return undefined;
  return { ...item, kind: "song", thumb: item.thumb ?? fallbackThumb };
}

export function videoItem(n: Node): Item | undefined {
  const id = n.video_id ?? n.id;
  if (typeof id !== "string") return undefined;
  return {
    kind: "video",
    id,
    title: text(n.title),
    subtitle: join([n.author?.name, text(n.short_view_count) || text(n.view_count)]),
    durationSec: seconds(n.duration),
    thumb: thumbLink(pickThumb(n.thumbnails)),
  };
}

// LockupView: the view model YouTube now uses for home, subscriptions, history and related
export function lockupItem(n: Node): Item | undefined {
  if (n.content_type !== "VIDEO" || !isVideoId(n.content_id)) return undefined;
  const durationSec = seconds(n.duration);
  if (!durationSec) return undefined; // live streams, premieres and promos carry no duration badge
  const parts: string[] = (n.metadata?.metadata?.metadata_rows ?? [])
    .flatMap((row: Node) => row?.metadata_parts ?? [])
    .map((p: Node) => text(p?.text))
    .filter(Boolean);
  const image = n.content_image?.image ?? n.content_image?.primary_thumbnail?.image;
  return {
    kind: "video",
    id: n.content_id,
    title: text(n.metadata?.title),
    // channel, views; related videos give a bare "1.6B"
    subtitle: join(parts.slice(0, 2).map((p, i) => (i === 1 && /^\d[\d.,]*[KMB]?$/.test(p) ? `${p} views` : p))),
    durationSec,
    thumb: thumbLink(pickThumb(image)),
  };
}

// a playable regular video from a YouTube feed; shorts, ads, live and upcoming are dropped
export function feedVideo(n: Node): Item | undefined {
  if (n?.type === "LockupView") return lockupItem(n);
  if (n?.type !== "Video" && n?.type !== "CompactVideo" && n?.type !== "GridVideo") return undefined;
  if (n.is_live || n.is_upcoming) return undefined;
  const item = videoItem(n);
  return item && isVideoId(item.id) && item.durationSec ? item : undefined;
}

export function shelfSection(s: Node, maxItems = SECTION_ITEMS): Section | undefined {
  const items = uniqueById((s.contents ?? []).map(musicItem)).slice(0, maxItems);
  if (!items.length) return undefined;
  return { title: text(s.header?.title) || text(s.title), items };
}

// MusicResponsiveHeader (album, playlist), MusicDetailHeader (older), MusicImmersiveHeader /
// MusicVisualHeader (artist); own playlists wrap theirs in MusicEditablePlaylistDetailHeader
// who made an album or playlist, from its page header
function headerBy(h: Node | undefined): string {
  const hh: Node | undefined = h?.type === "MusicEditablePlaylistDetailHeader" ? h.header : h;
  return text(hh?.strapline_text_one) || text(hh?.author?.name);
}

export function headerInfo(h: Node | undefined): { title: string; subtitle: string; thumb?: string } {
  const hh: Node | undefined = h?.type === "MusicEditablePlaylistDetailHeader" ? h.header : h;
  const by = headerBy(h);
  const count = text(hh?.second_subtitle)
    .split("•")
    .map((s) => s.trim())
    .find((s) => /\b(songs?|tracks?|episodes?|videos?)\b/.test(s));
  return {
    title: text(hh?.title),
    subtitle: join([by, bullets(text(hh?.subtitle)), count]),
    thumb: nodeThumb(hh),
  };
}

// An album's rows leave the artist out (it's the album's) and the album too, so its songs had no
// subtitle on Now Playing and in Up next; they get "Artist · Album" like songs anywhere else. Rows
// that name their own artists (features) keep them.
export function fillAlbumRows(items: Item[], artist: string, album: string): Item[] {
  const subtitle = join([artist, album]);
  return subtitle ? items.map((i) => (i.subtitle ? i : { ...i, subtitle })) : items;
}

// headerless pages (album playlists "OLAK5uy_…", mixes) get a header from their first rows
function derivedHeader(nodes: Node[], items: Item[], fallbackTitle: string): { title: string; subtitle: string; thumb?: string } {
  const albums = new Set(nodes.map((n) => n.album?.name).filter(Boolean));
  const first = nodes[0];
  const names = nodes.slice(0, 10).flatMap((n) => people(n.primary ?? n).split(", "));
  const artists = [...new Set(names.filter(Boolean))].slice(0, 3);
  return {
    title: albums.size === 1 ? String([...albums][0]) : fallbackTitle,
    subtitle: join([artists.join(", "), items.length ? `${items.length} songs` : undefined]),
    thumb: first ? nodeThumb(first.primary ?? first) : undefined,
  };
}

// Pages of a list. Each youtubei.js response is reduced to a Chunk; Pager turns chunks into pages
// with `next` tokens. Continuation pages go through the same reducers as first pages.

// Feed (home, subscriptions, history, search) and the YouTube Music HomeFeed, Search and Playlist
// all page with has_continuation / getContinuation()
function continued<T>(src: any, chunk: (page: any) => Chunk<T>): (() => Promise<Chunk<T>>) | undefined {
  return src?.has_continuation ? async () => chunk(await src.getContinuation()) : undefined;
}

// Feed.videos lists every video node of a response, first page or continuation
export const feedChunk = (feed: any): Chunk<Item> => ({ items: (feed.videos ?? []).map(feedVideo), more: continued(feed, feedChunk) });

// history comes in day sections; the shorts sit in ReelShelf rows, which feedVideo skips
export const historyChunk = (feed: any): Chunk<Item> => ({
  items: (feed.sections ?? []).flatMap((s: Node) => s.contents ?? []).map(feedVideo),
  more: continued(feed, historyChunk),
});

// a first search page holds shelves (MusicShelf, ItemSection); a continuation one MusicShelfContinuation
export const musicSearchChunk = (res: any): Chunk<Item> => {
  const shelves: Node[] = Array.isArray(res?.contents) ? res.contents : res?.contents ? [res.contents] : [];
  return { items: shelves.flatMap((s) => s?.contents ?? []).map(musicItem), more: continued(res, musicSearchChunk) };
};

const searchVideo = (n: Node): Item | undefined => {
  const item = videoItem(n);
  return item && isVideoId(item.id) ? item : undefined;
};

export const videoSearchChunk = (res: any): Chunk<Item> => ({ items: (res.videos ?? []).map(searchVideo), more: continued(res, videoSearchChunk) });

// A short from a search or home shelf (ShortsLockupView, or an older ReelItem), or from the Shorts
// "watch next" chain, which names only the video and its first frame: no title until /video/short/:id.
export function shortItem(n: Node): Item | undefined {
  let item: Item | undefined;
  if (n?.type === "ShortsLockupView") {
    const id = n.on_tap_endpoint?.payload?.videoId;
    item = { kind: "video", id, title: text(n.overlay_metadata?.primary_text), subtitle: text(n.overlay_metadata?.secondary_text), thumb: thumbLink(pickThumb(n.thumbnail)) };
  } else if (n?.type === "ReelItem") {
    item = { kind: "video", id: n.id, title: text(n.title), subtitle: text(n.views), thumb: thumbLink(pickThumb(n.thumbnails)) };
  } else if (n?.payload?.videoId) {
    item = { kind: "video", id: n.payload.videoId, title: "", subtitle: "", thumb: thumbLink(pickThumb(n.payload.thumbnail?.thumbnails)) };
  }
  return item && isVideoId(item.id) ? item : undefined;
}

// the shorts anywhere in a parsed response (search puts them in shelves inside sections)
export function findShorts(n: unknown, depth = 0, out: Node[] = []): Node[] {
  if (!n || typeof n !== "object" || depth > 8) return out;
  if (Array.isArray(n)) {
    for (const x of n) findShorts(x, depth + 1, out);
    return out;
  }
  const node = n as Node;
  if (node.type === "ShortsLockupView" || node.type === "ReelItem") out.push(node);
  else for (const key of ["contents", "items", "content", "results"]) findShorts(node[key], depth + 1, out);
  return out;
}

// the Shorts chain: each response names the next few shorts and how to get more
export const sequenceChunk = (info: any): Chunk<Item> => ({
  items: (info.watch_next_feed ?? []).map(shortItem),
  more: info.wn_has_continuation ? async () => sequenceChunk(await info.getWatchNextContinuation()) : undefined,
});

const compact = (n: number): string =>
  n >= 1e9 ? `${(n / 1e9).toFixed(1).replace(/\.0$/, "")}B` : n >= 1e6 ? `${(n / 1e6).toFixed(1).replace(/\.0$/, "")}M` : n >= 1e3 ? `${Math.round(n / 1e3)}K` : String(n);

export const homeChunk = (page: any): Chunk<Section> => ({
  items: (page.sections ?? []).map((s: Node) => shelfSection(s)),
  more: continued(page, homeChunk),
});

const playlistRows = (page: any): Node[] => (page.items ?? page.contents ?? []).filter((n: Node) => n?.type === "MusicResponsiveListItem");

export const playlistChunk = (page: any): Chunk<Item> => ({ items: playlistRows(page).map((n) => trackItem(n)), more: continued(page, playlistChunk) });

// TV pages (tv.ts) in the app's shape; songs when they come from a YouTube Music list
export const tvItem = (t: TvItem, kind: Item["kind"] = t.kind): Item => ({
  kind,
  id: t.id,
  title: t.title,
  subtitle: t.subtitle,
  ...(t.durationSec ? { durationSec: t.durationSec } : {}),
  ...(thumbLink(t.thumbUrl) ? { thumb: thumbLink(t.thumbUrl) } : {}),
});

const asItems = (c: Chunk<TvItem>, kind?: Item["kind"]): Chunk<Item> => ({
  items: c.items.map((t) => (t ? tvItem(t, kind) : undefined)),
  more: c.more ? async () => asItems(await c.more!(), kind) : undefined,
});

// YouTube without an account: its home is empty when signed out and Trending is gone, so the
// signed-out home mixes the TV app's topic pages (about 100 videos together, no paging)
const TOPICS = ["FEtopics_music", "FEtopics_gaming", "FEtopics_news", "FEtopics_sports"];

export function interleave<T>(lists: T[][]): T[] {
  const out: T[] = [];
  for (let i = 0; lists.some((l) => i < l.length); i++) for (const l of lists) if (i < l.length) out.push(l[i]!);
  return out;
}

// the related column ends in a ContinuationItem; its /next response appends more videos and the
// next ContinuationItem
export function relatedChunk(actions: unknown, nodes: Node[]): Chunk<Item> {
  const cont = nodes.find((n) => n?.type === "ContinuationItem");
  return {
    items: nodes.map(feedVideo),
    more: cont?.endpoint
      ? async () => {
          const res: any = await cont.endpoint.call(actions, { parse: true });
          return relatedChunk(actions, (res.on_response_received_endpoints ?? []).flatMap((a: Node) => a?.contents ?? []));
        }
      : undefined,
  };
}

// a radio's queue: PlaylistPanel and PlaylistPanelContinuation both carry { contents, continuation }.
// A short radio (a Replay Mix of 2 songs) answers empty continuations forever: no rows ends it.
type Execute = { execute: (endpoint: string, args: Record<string, unknown>) => Promise<unknown> };
export function mixChunk(actions: Execute, id: string, panel: Node | undefined): Chunk<Item> {
  const token: unknown = panel?.continuation;
  const rows: Node[] = panel?.contents ?? [];
  return {
    items: rows.map((n: Node) => trackItem(n)),
    more:
      typeof token === "string" && token && rows.length
        ? async () => {
            const res: any = await actions.execute("/next", { playlistId: id, continuation: token, client: "YTMUSIC", parse: true });
            return mixChunk(actions, id, res?.continuation_contents);
          }
        : undefined,
  };
}

const byId = { key: (i: Item) => i.id, min: PAGE_ITEMS };

const withNext = (p: Page<unknown>): { next?: string } => (p.next ? { next: p.next } : {});

// continuation pages of the routes whose first page is not a bare { items, next }
export const homePage = (p: Page<unknown>) => ({ sections: p.items as Section[], ...withNext(p) });
export const playlistPage = (p: Page<unknown>) => ({ title: "", subtitle: "", items: p.items as Item[], ...withNext(p) });

// lyrics as YouTube Music has them; partial: the timed version failed and this is the plain one
export interface MusicLyrics {
  synced: LyricLine[] | null;
  plain: string;
  partial?: boolean;
}

// the first object under `key`, depth-first (the lyrics model sits ~8 levels down)
function findKey(o: unknown, key: string, depth = 0): Node | undefined {
  if (!o || typeof o !== "object" || depth > 16) return undefined;
  const n = o as Node;
  if (n[key] && typeof n[key] === "object") return n[key];
  for (const v of Object.values(n)) {
    const hit = findKey(v, key, depth + 1);
    if (hit) return hit;
  }
  return undefined;
}

// The Android app's lyrics page (a ~650 KB response) reduced to its lines. Songs carry a cue range
// per line; music videos get the same lines without timings ("staticLayout"), which become plain
// text. "♪" lines mark instrumental stretches and become pauses (""), as do gaps between lines
// and the end of the last line.
export function timedLyrics(data: unknown): MusicLyrics | undefined {
  const rows: Node[] = findKey(data, "timedLyricsModel")?.lyricsData?.timedLyricsData ?? [];
  const lines = rows.map((r) => {
    const words = typeof r?.lyricLine === "string" ? r.lyricLine.replace(/\s+/g, " ").trim() : "";
    const ms = (v: unknown): number => (typeof v === "string" || typeof v === "number") && v !== "" ? Number(v) : NaN;
    return { text: /^[♪♫\s]*$/.test(words) ? "" : words, start: ms(r?.cueRange?.startTimeMilliseconds), end: ms(r?.cueRange?.endTimeMilliseconds) };
  });
  const plain = lines.map((l) => l.text).join("\n").replace(/\n{3,}/g, "\n\n").trim();
  if (!plain) return undefined;
  if (!lines.every((l) => Number.isFinite(l.start) && l.start >= 0)) return { synced: null, plain };
  const synced: LyricLine[] = [];
  lines.forEach((l, i) => {
    synced.push({ t: Math.round(l.start), text: l.text });
    const next = lines[i + 1];
    const pause = l.text && Number.isFinite(l.end) && l.end > l.start && (!next || next.start - l.end >= LYRIC_GAP_MS);
    if (pause) synced.push({ t: Math.round(l.end), text: "" });
  });
  return { synced: synced.sort((a, b) => a.t - b.t), plain };
}

const isLyricsTab = (t: Node): boolean =>
  t?.endpoint?.payload?.browseEndpointContextSupportedConfigs?.browseEndpointContextMusicConfig?.pageType === "MUSIC_PAGE_TYPE_TRACK_LYRICS";

function tvTitle(data: unknown): string | undefined {
  let found: string | undefined;
  const walk = (o: unknown): void => {
    if (found || !o || typeof o !== "object") return;
    const r = o as Node;
    if (r.tileRenderer || r.lockupViewModel) return;
    const t = r.entityMetadataRenderer?.title ?? r.playlistHeaderRenderer?.title;
    const value = typeof t?.simpleText === "string" ? t.simpleText : Array.isArray(t?.runs) ? t.runs.map((x: Node) => x?.text ?? "").join("") : undefined;
    if (value?.trim()) {
      found = value.trim();
      return;
    }
    for (const v of Object.values(r)) walk(v);
  };
  walk(data);
  return found;
}

export class Browse {
  private client: Promise<Innertube> | undefined;
  private android: Promise<Innertube> | undefined;
  private anonTv: Promise<Innertube> | undefined; // the TV topic pages, never signed in
  private cookieMtime = -1;
  private readonly cache = new TtlCache<unknown>(CACHE_TTL_MS);
  private readonly pager = new Pager();

  constructor(
    private readonly config: Config,
    private readonly log?: { warn: (obj: object, msg: string) => void },
    private readonly tv?: TvAccount,
  ) {}

  // a code sign-in came or went: feeds and continuations of the old state are dropped
  forget(): void {
    this.cache.clear();
    this.pager.clear();
  }

  // the account feeds (library, subscriptions, history) need cookies.txt or the code sign-in
  async hasAccount(): Promise<boolean> {
    return !!this.tv?.signedIn || (await this.hasCookies());
  }

  private tvBrowse = (args: Record<string, unknown>) => this.tv!.browse(args);

  private async tvFeed(args: Record<string, unknown>, kind?: Item["kind"]): Promise<Chunk<Item>> {
    return asItems(tvChunk(this.tvBrowse, await this.tvBrowse(args)), kind);
  }

  private async popularChunk(): Promise<Chunk<Item>> {
    if (!this.anonTv) {
      const created = Innertube.create({ retrieve_player: false, generate_session_locally: true, lang: "en" });
      this.anonTv = created;
      created.catch(() => {
        if (this.anonTv === created) this.anonTv = undefined;
      });
    }
    const yt = await this.anonTv;
    const pages = await Promise.all(
      TOPICS.map((browseId) =>
        yt.actions.execute("/browse", { browseId, client: "TV" }).then(
          (r) => tvItems(r.data).filter((i) => i.kind === "video"),
          () => [] as TvItem[],
        ),
      ),
    );
    return { items: interleave(pages).map((t) => tvItem(t)) };
  }

  // the account's playlists from the TV app, Liked Music first (YouTube Music without cookies)
  private async tvLibrary(): Promise<{ playlists: Item[] }> {
    const data = await this.tvBrowse({ browseId: "FEplaylist_aggregation" });
    const own = tvItems(data)
      .filter((t) => t.kind === "playlist" && !["LM", "LL", "WL"].includes(t.id))
      .map((t) => tvItem(t));
    return { playlists: uniqueById([{ kind: "playlist", id: "LM", title: "Liked Music", subtitle: "Auto playlist" }, ...own]) };
  }

  // a playlist through the TV app (Liked Music, or a private one without cookies); every row a song
  private async tvPlaylist(id: string, title?: string): Promise<Collection & { next?: string }> {
    const first = await this.tvBrowse({ browseId: `VL${id}` });
    const page = await this.pager.page(`music/playlist/${id}`, async () => asItems(tvChunk(this.tvBrowse, first), "song"), byId);
    const named = title ?? tvTitle(first) ?? "Playlist";
    return { title: named, subtitle: page.items.length ? `${page.items.length}${page.next ? "+" : ""} songs` : "", items: page.items, ...withNext(page) };
  }

  // YouTube answers a signed-out session with sign-in promos instead of an error
  private emptyAccountFeed(feed: string): void {
    this.log?.warn({ feed }, "account feed is empty: cookies.txt may be signed out (export a new one; docs/server.md)");
  }

  async hasCookies(): Promise<boolean> {
    return stat(this.config.cookiesFile).then((s) => s.size > 0, () => false);
  }

  // who cookies.txt signs in as (the admin page); null when signed out or no cookies. youtubei.js
  // throws when YouTube treats the session as signed out.
  async accountInfo(): Promise<{ name: string; handle: string | null } | null> {
    if (!(await this.hasCookies())) return null;
    const info = await (await this.yt()).account.getInfo();
    const item = info.contents?.contents.find((c) => "account_name" in c) as { account_name?: { toString(): string }; channel_handle?: { toString(): string } } | undefined;
    const name = item?.account_name?.toString().trim();
    if (!name) return null;
    return { name, handle: item?.channel_handle?.toString().trim() || null };
  }

  // one Innertube session, rebuilt when cookies.txt is replaced
  private async yt(): Promise<Innertube> {
    return (await this.session()).client;
  }

  // the current session (not awaited); a replaced cookies.txt starts a new one and drops what the
  // old one produced
  private async session(): Promise<{ client: Promise<Innertube> }> {
    const mtime = await stat(this.config.cookiesFile).then((s) => s.mtimeMs, () => 0);
    if (!this.client || mtime !== this.cookieMtime) {
      // assigned before any await, so concurrent first requests share one session
      this.cookieMtime = mtime;
      this.cache.clear(); // feeds of the previous cookies
      this.pager.clear(); // and their continuations
      const client = this.createClient(mtime);
      this.client = client;
      client.catch(() => {
        if (this.client === client) this.client = undefined;
      });
    }
    return { client: this.client };
  }

  private async createClient(mtime: number): Promise<Innertube> {
    const cookie = mtime ? cookiesToHeader(await readFile(this.config.cookiesFile, "utf8")) : undefined;
    return Innertube.create({
      cookie: cookie || undefined,
      retrieve_player: false,
      generate_session_locally: true,
      lang: "en",
    });
  }

  // Check cookies.txt first, so a session rebuild clears the cache before this entry goes in.
  // A cached first page keeps its `next` token: tokens live 30 min, the cache 2 min, so a page
  // served from here always carries a token with at least 28 min left.
  async cached<T>(key: string, load: () => Promise<T>): Promise<T> {
    await this.yt();
    return (await this.cache.get(key, load)) as T;
  }

  // the loader of a later page; undefined when the token is unknown, expired or from another list
  async more(scope: string, token: string): Promise<(() => Promise<Page<unknown>>) | undefined> {
    await this.session(); // replaced cookies drop the old session's tokens
    return this.pager.resume(scope, token);
  }

  async musicSearch(q: string, type: string): Promise<Page<Item>> {
    const yt = await this.yt();
    return this.pager.page("music/search", async () => musicSearchChunk(await yt.music.search(q, { type: type as any })), byId);
  }

  async videoSearch(q: string): Promise<Page<Item>> {
    const yt = await this.yt();
    return this.pager.page("video/search", async () => videoSearchChunk(await yt.search(q, { type: "video" })), byId);
  }

  // the first page holds ~3 shelves; each continuation (~0.2–0.6 s) adds ~3 more
  async musicHome(): Promise<{ sections: Section[]; next?: string }> {
    const yt = await this.yt();
    return homePage(await this.pager.page("music/home", async () => homeChunk(await yt.music.getHomeFeed()), { min: HOME_SECTIONS }));
  }

  // yt.music.getLibrary() throws on the current library page (ItemSection where it expects a
  // Grid), so read the playlists grid directly.
  async musicLibrary(): Promise<{ playlists: Item[] }> {
    if (this.tv?.signedIn && !(await this.hasCookies())) return this.tvLibrary();
    const yt = await this.yt();
    let res: any = await yt.actions.execute("/browse", { browseId: "FEmusic_liked_playlists", client: "YTMUSIC", parse: true });
    const cards: Node[] = [...(res.contents_memo?.get("MusicTwoRowItem") ?? [])];
    let continuation: unknown = res.contents_memo?.get("Grid")?.[0]?.continuation;
    for (let i = 0; i < 3 && typeof continuation === "string"; i++) {
      try {
        res = await yt.actions.execute("/browse", { continuation, client: "YTMUSIC", parse: true });
      } catch {
        break;
      }
      cards.push(...(res.continuation_contents_memo?.get("MusicTwoRowItem") ?? []));
      continuation = res.continuation_contents?.continuation;
    }
    if (!cards.length && this.tv?.signedIn) return this.tvLibrary(); // cookies signed out
    if (!cards.length) this.emptyAccountFeed("music/library"); // signed in, the grid always has Liked Music
    const liked = cards.map(musicItem).find((i) => i?.id === "LM");
    const own = cards
      .map(musicItem)
      // "New playlist" is a create button (item_type "endpoint", dropped by musicItem);
      // "Episodes for Later" (SE) holds podcast episodes, which the app cannot list
      .filter((i): i is Item => i?.kind === "playlist" && i.id !== "LM" && i.id !== "SE");
    return {
      playlists: uniqueById([
        { kind: "playlist", id: "LM", title: "Liked Music", subtitle: "Auto playlist", ...(liked?.thumb ? { thumb: liked.thumb } : {}) },
        ...own,
      ]),
    };
  }

  // YouTube's own page size (100 rows); continuation pages carry rows only, no header
  async musicPlaylist(id: string): Promise<Collection & { next?: string }> {
    if (isMix(id)) return this.musicMix(id);
    const viaTv = !!this.tv?.signedIn && !(await this.hasCookies());
    if (viaTv && id === "LM") return this.tvPlaylist(id, "Liked Music");
    const first: any = await (await this.yt()).music.getPlaylist(id).catch((err) => {
      if (viaTv) return null; // private: only the signed-in TV app can list it
      throw err;
    });
    if (!first) return this.tvPlaylist(id);
    const page = await this.pager.page(`music/playlist/${id}`, async () => playlistChunk(first), byId);
    const head = headerInfo(first.header);
    const info = head.title ? head : derivedHeader(playlistRows(first), page.items, "Playlist");
    return { title: info.title, subtitle: info.subtitle, ...(info.thumb ? { thumb: info.thumb } : {}), items: page.items, ...withNext(page) };
  }

  // a radio's queue (~50 rows a page) continues for as long as the app keeps scrolling
  private async musicMix(id: string): Promise<Collection & { next?: string }> {
    const yt = await this.yt();
    const res: any = await yt.actions.execute("/next", { playlistId: id, client: "YTMUSIC", parse: true });
    const panel: Node | undefined = res.contents_memo?.get("PlaylistPanel")?.[0];
    const nodes: Node[] = panel?.contents ?? [];
    const page = await this.pager.page(`music/playlist/${id}`, async () => mixChunk(yt.actions, id, panel), byId);
    const info = derivedHeader(nodes, page.items, text(panel?.title) || "Mix");
    return { title: text(panel?.title) || "Mix", subtitle: info.subtitle, ...(info.thumb ? { thumb: info.thumb } : {}), items: page.items, ...withNext(page) };
  }

  async musicAlbum(id: string): Promise<Collection> {
    const album: any = await (await this.yt()).music.getAlbum(id);
    const info = headerInfo(album.header);
    // album rows have no artwork of their own; give them the cover
    const rows = uniqueById((album.contents ?? []).map((n: Node) => trackItem(n, info.thumb))).slice(0, LIST_ITEMS);
    const items = fillAlbumRows(rows, headerBy(album.header), info.title);
    return { title: info.title, subtitle: info.subtitle, ...(info.thumb ? { thumb: info.thumb } : {}), items };
  }

  async musicArtist(id: string): Promise<{ title: string; thumb?: string; sections: Section[] }> {
    const artist: any = await (await this.yt()).music.getArtist(id);
    const info = headerInfo(artist.header);
    const sections = (artist.sections ?? []).map((s: Node) => shelfSection(s)).filter((s: Section | undefined): s is Section => !!s);
    return { title: info.title, ...(info.thumb ? { thumb: info.thumb } : {}), sections };
  }

  async musicNext(videoId: string): Promise<{ items: Item[] }> {
    const panel: any = await (await this.yt()).music.getUpNext(videoId, true);
    const items = uniqueById((panel.contents ?? []).map((n: Node) => trackItem(n)));
    return { items: items.filter((i) => i.id !== videoId).slice(0, LIST_ITEMS) };
  }

  // An anonymous session that talks as the YouTube Music Android app, for timed lyrics only: the
  // context is youtubei.js's own ANDROID_MUSIC one with a newer app version (see
  // ANDROID_MUSIC_VERSION). Built locally, no request.
  private androidMusic(): Promise<Innertube> {
    if (!this.android) {
      const client = Innertube.create({
        client_type: ClientType.ANDROID_MUSIC,
        user_agent: `com.google.android.apps.youtube.music/${ANDROID_MUSIC_VERSION} (Linux; U; Android 14) gzip`,
        retrieve_player: false,
        generate_session_locally: true,
        enable_session_cache: false,
        lang: "en",
      }).then((yt) => {
        Object.assign(yt.session.context.client, {
          clientVersion: ANDROID_MUSIC_VERSION,
          androidSdkVersion: 34,
          osName: "Android",
          osVersion: "14",
          platform: "MOBILE",
          clientFormFactor: "SMALL_FORM_FACTOR",
        });
        return yt;
      });
      this.android = client;
      client.catch(() => {
        if (this.android === client) this.android = undefined;
      });
    }
    return this.android;
  }

  // The song's lyrics tab: timed lines from the Android app's page when it has them, else the web
  // page's plain text (what yt.music.getLyrics reads). undefined when the song has no lyrics.
  async musicLyrics(videoId: string): Promise<MusicLyrics | undefined> {
    const yt = await this.yt();
    const next: any = await yt.actions.execute("/next", { videoId, client: "YTMUSIC", parse: true });
    const tab: Node | undefined = next.contents_memo?.get("Tab")?.find(isLyricsTab);
    const browseId: unknown = tab?.endpoint?.payload?.browseId;
    if (!tab || typeof browseId !== "string") return undefined; // not a song, or no lyrics tab
    let partial = false;
    try {
      const res: any = await (await this.androidMusic()).actions.execute("/browse", { browseId, parse: false });
      const found = timedLyrics(res?.data); // the raw response is dropped here
      if (found) return found;
    } catch (err) {
      partial = true;
      this.log?.warn({ err: String((err as Error)?.message ?? err).slice(0, 200) }, "timed lyrics request failed, trying plain lyrics");
    }
    // "Lyrics not available" comes back as a Message page with no shelf
    const page: any = await tab.endpoint.call(yt.actions, { client: "YTMUSIC", parse: true });
    const plain = text(page?.contents_memo?.get("MusicDescriptionShelf")?.[0]?.description).trim();
    return plain ? { synced: null, plain, ...(partial ? { partial } : {}) } : undefined;
  }

  // the code sign-in's home, else the cookies' home, else (signed out, or an empty feed) the mix of
  // topic pages
  async videoHome(): Promise<Page<Item>> {
    if (this.tv?.signedIn) return this.pager.page("video/home", () => this.tvFeed({ browseId: "FEwhat_to_watch" }), byId);
    if (await this.hasCookies()) {
      const yt = await this.yt();
      const page = await this.pager.page("video/home", async () => feedChunk(await yt.getHomeFeed()), byId);
      if (page.items.length) return page;
    }
    return this.pager.page("video/home", () => this.popularChunk(), byId);
  }

  // Shorts, like the app's Shorts tab: popular shorts first (titled, from a Shorts search), then
  // YouTube's own "watch next" chain from one of them, which goes on for as long as anyone scrolls.
  // With cookies.txt the chain follows the account's taste.
  async videoShorts(): Promise<Page<Item>> {
    const yt = await this.yt();
    return this.pager.page("video/shorts", async () => {
      const seeds = uniqueById(findShorts((await yt.search("shorts", { type: "shorts" })).results).map(shortItem));
      const start = seeds[Math.floor(Math.random() * Math.min(5, seeds.length))];
      return { items: seeds, more: start ? async () => sequenceChunk(await yt.getShortsVideoInfo(start.id)) : undefined };
    }, byId);
  }

  // what a short's page shows: title, channel, views
  async shortInfo(id: string): Promise<{ id: string; title: string; channel: string; subtitle: string; durationSec?: number }> {
    const yt = await this.yt();
    const b = (await yt.getShortsVideoInfo(id)).basic_info;
    const channel = b.author ?? b.channel?.name ?? "";
    const views = typeof b.view_count === "number" ? `${compact(b.view_count)} views` : "";
    return { id, title: b.title ?? "", channel, subtitle: join([channel, views]), durationSec: seconds(b.duration) };
  }

  async videoSubscriptions(): Promise<Page<Item>> {
    if (this.tv?.signedIn) return this.pager.page("video/subscriptions", () => this.tvFeed({ browseId: "FEsubscriptions" }), byId);
    const yt = await this.yt();
    const page = await this.pager.page("video/subscriptions", async () => feedChunk(await yt.getSubscriptionsFeed()), byId);
    if (!page.items.length) this.emptyAccountFeed("video/subscriptions");
    return page;
  }

  async videoHistory(): Promise<Page<Item>> {
    if (this.tv?.signedIn) return this.pager.page("video/history", () => this.tvFeed({ browseId: "FEhistory" }), byId);
    const yt = await this.yt();
    const page = await this.pager.page("video/history", async () => historyChunk(await yt.getHistory()), byId);
    if (!page.items.length) this.emptyAccountFeed("video/history");
    return page;
  }

  // the watch page's "up next" column, without the /player request getInfo() would add
  async videoRelated(videoId: string): Promise<Page<Item>> {
    const yt = await this.yt();
    const load = async (): Promise<Chunk<Item>> => {
      const res: any = await yt.actions.execute("/next", { videoId, parse: true });
      const secondary: Node[] = res.contents?.item?.()?.secondary_results ?? [];
      const nodes = secondary.flatMap((n) => (n?.type === "ItemSection" ? (n.contents ?? []) : [n]));
      if (nodes.some((n) => n?.type === "LockupView" || n?.type === "CompactVideo")) return relatedChunk(yt.actions, nodes);
      // another layout: any video of the response, without paging
      return { items: [...(res.contents_memo?.get("LockupView") ?? []), ...(res.contents_memo?.get("CompactVideo") ?? [])].map(feedVideo) };
    };
    return this.pager.page(`video/related/${videoId}`, load, byId, new Set([videoId]));
  }
}

// returns the Browse instance so /lyrics can share its Innertube session
export function registerBrowseRoutes(app: FastifyInstance, deps: { config: Config; tv?: TvAccount }): Browse {
  const browse = new Browse(deps.config, app.log, deps.tv);

  // one retry: a lone "fetch failed" between the VPS and YouTube is common (seen 2026-10-30)
  const run = async (reply: FastifyReply, load: () => Promise<unknown>) => {
    try {
      return await load().catch(async () => {
        await new Promise((ok) => setTimeout(ok, 300));
        return load();
      });
    } catch (err) {
      app.log.warn({ err: String((err as Error)?.message ?? err).slice(0, 300) }, "youtubei.js request failed");
      return reply.code(502).send({ error: "browse" });
    }
  };

  // feeds that only exist for a signed-in account
  const account = async (reply: FastifyReply, key: string, load: () => Promise<unknown>) => {
    if (!(await browse.hasAccount())) return reply.code(503).send({ error: "no_cookies" });
    return run(reply, () => browse.cached(key, load));
  };

  // a later page of a list: the token stands for the youtubei.js object that continues it
  const later = async (reply: FastifyReply, scope: string, token: string, shape: (p: Page<unknown>) => unknown = (p) => p) => {
    const load = await browse.more(scope, token);
    if (!load) return reply.code(410).send({ error: "expired" });
    return run(reply, async () => shape(await load()));
  };

  const nextOf = (q: { next?: unknown }): string | undefined => (typeof q.next === "string" && q.next ? q.next : undefined);

  // a list route: page one (cached for 2 minutes with `cache`, signed-in only with `account`), or
  // the page a `next` token points to
  const list = (
    path: string,
    scope: string,
    first: () => Promise<unknown>,
    opts: { cache?: boolean; account?: boolean; shape?: (p: Page<unknown>) => unknown } = {},
  ) =>
    app.get<{ Querystring: { next?: string } }>(path, async (req, reply) => {
      if (opts.account && !(await browse.hasAccount())) return reply.code(503).send({ error: "no_cookies" });
      const next = nextOf(req.query);
      if (next) return later(reply, scope, next, opts.shape);
      return run(reply, () => (opts.cache ? browse.cached(scope, first) : first()));
    });

  app.get<{ Querystring: { q?: string; type?: string; next?: string } }>("/music/search", async (req, reply) => {
    const next = nextOf(req.query);
    if (next) return later(reply, "music/search", next);
    const q = (req.query.q ?? "").trim();
    const type = req.query.type ?? "song";
    if (!q || !MUSIC_TYPES.has(type)) return reply.code(400).send({ error: "query" });
    return run(reply, () => browse.musicSearch(q, type));
  });

  app.get<{ Querystring: { q?: string; next?: string } }>("/video/search", async (req, reply) => {
    const next = nextOf(req.query);
    if (next) return later(reply, "video/search", next);
    const q = (req.query.q ?? "").trim();
    if (!q) return reply.code(400).send({ error: "query" });
    return run(reply, () => browse.videoSearch(q));
  });

  list("/music/home", "music/home", () => browse.musicHome(), { cache: true, shape: homePage });

  app.get("/music/library", async (_req, reply) => account(reply, "music/library", () => browse.musicLibrary()));

  app.get<{ Params: { id: string }; Querystring: { next?: string } }>("/music/playlist/:id", async (req, reply) => {
    const id = playlistId(req.params.id);
    if (!PLAYLIST_ID.test(id)) return reply.code(400).send({ error: "id" });
    const next = nextOf(req.query);
    if (next) return later(reply, `music/playlist/${id}`, next, playlistPage);
    return run(reply, () => browse.cached(`music/playlist/${id}`, () => browse.musicPlaylist(id)));
  });

  app.get<{ Params: { id: string } }>("/music/album/:id", async (req, reply) => {
    const id = req.params.id;
    if (!ALBUM_ID.test(id)) return reply.code(400).send({ error: "id" });
    return run(reply, () => browse.cached(`music/album/${id}`, () => browse.musicAlbum(id)));
  });

  app.get<{ Params: { id: string } }>("/music/artist/:id", async (req, reply) => {
    const id = req.params.id;
    if (!ARTIST_ID.test(id)) return reply.code(400).send({ error: "id" });
    return run(reply, () => browse.cached(`music/artist/${id}`, () => browse.musicArtist(id)));
  });

  app.get<{ Params: { id: string } }>("/music/next/:id", async (req, reply) => {
    const id = req.params.id;
    if (!isVideoId(id)) return reply.code(400).send({ error: "id" });
    return run(reply, () => browse.cached(`music/next/${id}`, () => browse.musicNext(id)));
  });

  list("/video/home", "video/home", () => browse.videoHome(), { cache: true });
  list("/video/subscriptions", "video/subscriptions", () => browse.videoSubscriptions(), { cache: true, account: true });
  list("/video/history", "video/history", () => browse.videoHistory(), { cache: true, account: true });
  list("/video/shorts", "video/shorts", () => browse.videoShorts(), { cache: true });

  app.get<{ Params: { id: string } }>("/video/short/:id", async (req, reply) => {
    const id = req.params.id;
    if (!isVideoId(id)) return reply.code(400).send({ error: "id" });
    return run(reply, () => browse.cached(`video/short/${id}`, () => browse.shortInfo(id)));
  });

  app.get<{ Params: { id: string }; Querystring: { next?: string } }>("/video/related/:id", async (req, reply) => {
    const id = req.params.id;
    if (!isVideoId(id)) return reply.code(400).send({ error: "id" });
    const next = nextOf(req.query);
    if (next) return later(reply, `video/related/${id}`, next);
    return run(reply, () => browse.cached(`video/related/${id}`, () => browse.videoRelated(id)));
  });

  // GET /thumb?u=<Google image URL>&w=<px>[&h=<px>]: w wide (aspect kept), or w×h cropped to fill
  app.get<{ Querystring: { u?: string; w?: string; h?: string } }>("/thumb", async (req, reply) => {
    let url: URL;
    try {
      url = new URL(req.query.u ?? "");
    } catch {
      return reply.code(400).send({ error: "url" });
    }
    // only Google's image hosts: anything else would make this an open proxy
    if (!isThumbHost(url)) {
      return reply.code(400).send({ error: "host" });
    }
    const width = thumbPx(req.query.w) ?? DEFAULT_THUMB_PX;
    const height = thumbPx(req.query.h);
    const sized = sizedThumbUrl(url, Math.max(width, height ?? 0) * THUMB_OVERSAMPLE);
    const fetchImage = async (src: URL): Promise<Buffer | undefined> => {
      const res = await request(src, { dispatcher, headersTimeout: 10_000, bodyTimeout: 10_000 });
      if (res.statusCode === 200) return Buffer.from(await res.body.arrayBuffer());
      await res.body.dump();
      return undefined;
    };
    return run(reply, async () => {
      // a size the image server refuses falls back to the URL as YouTube gave it
      const image = (await fetchImage(sized)) ?? (sized !== url ? await fetchImage(url) : undefined);
      if (!image) return reply.code(502).send({ error: "upstream" });
      const jpeg = await sharp(image)
        .resize(height ? { width, height, fit: "cover", kernel: "lanczos3" } : { width, kernel: "lanczos3" })
        .jpeg({ quality: 88, chromaSubsampling: "4:4:4" })
        .toBuffer();
      return reply.type("image/jpeg").header("cache-control", "public, max-age=604800").send(jpeg);
    });
  });

  return browse;
}
