// YouTube Music's podcasts: the shows an account saved, a show's episodes (newest first), an episode
// with its chapters, Episodes for Later and New Episodes, podcast search, and what the account does with
// them (save a show, queue an episode for later, mark one played). Their episodes are YouTube videos:
// youtubei.js lists them here, yt-dlp resolves them (the /s/ routes), as for songs.
//
// These reduce YouTube Music's raw answers (parse: false), as native/direct/music.cpp does in direct
// mode, so both modes read the same fields. Nothing here reads YouTube's words to decide anything: an
// episode is known by its musicVideoType (MUSIC_VIDEO_TYPE_PODCAST_EPISODE), a show by its page type,
// a date by where it sits; YouTube's own words (a date, "3 hr 21 min") are passed on to be shown.

import { thumbLink, pickThumb } from "./browse.js";
import type { Chunk } from "./paging.js";

type Node = Record<string, any>;

// what a Rondo lists: a show or an episode (an Item of browse.ts, with an episode's own fields)
export interface Show {
  kind: "podcast";
  id: string; // MPSP… (the show's browse id)
  title: string;
  subtitle: string; // who makes it
  thumb?: string;
}

export interface Episode {
  kind: "episode";
  id: string; // the video id
  title: string;
  subtitle: string; // what a row shows under its title (YouTube's words: the date, the length)
  thumb?: string;
  durationSec?: number; // when YouTube gives the length as a clock
  show?: string;
  showId?: string;
  date?: string; // YouTube's words ("6d ago", "Dec 13, 2025")
  length?: string; // YouTube's words ("3 hr 21 min") when there is no clock
  progress?: number; // 0..100: how much of it YouTube says the account has heard (signed in)
  played?: boolean; // YouTube's Played mark (signed in)
}

export interface ShowPage {
  title: string;
  subtitle: string;
  thumb?: string;
  description: string;
  saved?: boolean; // in the account's library (only when signed in)
  items: Episode[];
}

export interface Chapter {
  ms: number;
  title: string;
}

export interface EpisodeInfo {
  id: string;
  title: string;
  show: string;
  showId: string;
  thumb?: string;
  description: string;
  durationSec?: number;
  date?: string;
  chapters: Chapter[];
  progress?: number;
  played?: boolean;
  later?: boolean; // in Episodes for Later (only when signed in)
}

export const PODCAST_PAGE = "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE";
export const EPISODE_PAGE = "MUSIC_PAGE_TYPE_NON_MUSIC_AUDIO_TRACK_PAGE";
export const EPISODE_TYPE = "MUSIC_VIDEO_TYPE_PODCAST_EPISODE";
const SEPARATOR = /^\s*[•·]\s*$/;
const VIDEO_ID = /^[A-Za-z0-9_-]{11}$/;
// a show's browse id: MPSP and its playlist (PL…, OL…)
export const SHOW_ID = /^MPSP[A-Za-z0-9_-]{4,80}$/;
export const showId = (id: string): string => (id.startsWith("MPSP") ? id : `MPSP${id}`);

// YouTube Music's search filters (direct mode's are the same): its Podcasts and Episodes chips
export const SEARCH_PARAMS: Record<string, string> = { podcast: "EgWKAQJQAQ%3D%3D", episode: "EgWKAQJIAQ%3D%3D" };

const runs = (t: any): Node[] => (Array.isArray(t?.runs) ? t.runs : []);
export const runsText = (t: any): string => (typeof t?.simpleText === "string" ? t.simpleText : runs(t).map((r) => r?.text ?? "").join(""));
const parts = (t: any): Node[] => runs(t).filter((r) => typeof r?.text === "string" && r.text.trim() && !SEPARATOR.test(r.text));
const join = (xs: Array<string | undefined>): string => xs.filter((x): x is string => !!x).join(" · ");
const pageType = (ep: any): string | undefined => ep?.browseEndpoint?.browseEndpointContextSupportedConfigs?.browseEndpointContextMusicConfig?.pageType;
const videoType = (ep: any): string | undefined => ep?.watchEndpoint?.watchEndpointMusicSupportedConfigs?.watchEndpointMusicConfig?.musicVideoType;
const thumbOf = (t: any): string | undefined => thumbLink(pickThumb(t?.musicThumbnailRenderer?.thumbnail?.thumbnails ?? t?.thumbnails));

// "3:21:14" -> 12074; anything else (words) -> undefined
export function clockSeconds(s: string | undefined): number | undefined {
  if (!s || !/^\d+(?::[0-5]\d)+$/.test(s.trim())) return undefined;
  return s
    .trim()
    .split(":")
    .reduce((n, p) => n * 60 + Number(p), 0);
}

// the run naming a show (a link to its page) among a row's runs
function showRun(...texts: any[]): Node | undefined {
  for (const t of texts) for (const r of runs(t)) if (pageType(r?.navigationEndpoint) === PODCAST_PAGE) return r;
  return undefined;
}

// the date of a row: the part right before the show's name, else the last part (YouTube's words)
function dateOf(t: any, show?: Node): string | undefined {
  const ps = parts(t);
  const at = show ? ps.indexOf(show) : -1;
  const p = at > 0 ? ps[at - 1] : at < 0 ? ps[ps.length - 1] : undefined;
  return p?.text?.trim() || undefined;
}

function progressOf(p: Node | undefined): { progress?: number; played?: boolean; length?: string } {
  const r = p?.musicPlaybackProgressRenderer;
  if (!r) return {};
  const pct = typeof r.playbackProgressPercentage === "number" ? Math.max(0, Math.min(100, r.playbackProgressPercentage)) : undefined;
  const length = parts(r.durationText).map((x) => x.text.trim()).pop();
  return { ...(pct !== undefined ? { progress: pct, played: pct >= 100 } : {}), ...(length ? { length } : {}) };
}

function episode(e: Omit<Episode, "kind" | "subtitle">, subtitle: string): Episode | undefined {
  if (!VIDEO_ID.test(e.id) || !e.title) return undefined;
  const out: Episode = { kind: "episode", id: e.id, title: e.title, subtitle };
  for (const k of ["thumb", "durationSec", "show", "showId", "date", "length", "progress", "played"] as const)
    if (e[k] !== undefined && e[k] !== "") (out as any)[k] = e[k];
  return out;
}

// An episode's row: musicMultiRowListItem (a show's page, New Episodes), musicResponsiveListItem (search,
// Episodes for Later) or musicTwoRowItem (a home shelf). undefined when it isn't an episode.
// onShow: the show the page is (a show's own rows don't name it); a row there leaves its name out.
export function episodeItem(entry: Node, onShow?: { title: string; id: string }): Episode | undefined {
  if (entry?.musicMultiRowListItemRenderer) {
    const r = entry.musicMultiRowListItemRenderer;
    const watch = r.onTap ?? r.overlay?.musicItemThumbnailOverlayRenderer?.content?.musicPlayButtonRenderer?.playNavigationEndpoint;
    const id = watch?.watchEndpoint?.videoId;
    const titled = pageType(runs(r.title)[0]?.navigationEndpoint) === EPISODE_PAGE;
    if (videoType(watch) !== EPISODE_TYPE && !titled) return undefined;
    const s = showRun(r.secondTitle);
    const p = progressOf(r.playbackProgress);
    const date = dateOf(r.subtitle);
    const show = s ? s.text : onShow?.title;
    return episode(
      {
        id,
        title: runsText(r.title),
        thumb: thumbOf(r.thumbnail),
        show,
        showId: s?.navigationEndpoint?.browseEndpoint?.browseId ?? onShow?.id,
        date,
        ...p,
      },
      join([onShow ? undefined : show, date, p.length]),
    );
  }
  if (entry?.musicResponsiveListItemRenderer) {
    const r = entry.musicResponsiveListItemRenderer;
    const cols = (r.flexColumns ?? []).map((c: Node) => c?.musicResponsiveListItemFlexColumnRenderer?.text);
    const play = r.overlay?.musicItemThumbnailOverlayRenderer?.content?.musicPlayButtonRenderer?.playNavigationEndpoint;
    const titled = pageType(runs(cols[0])[0]?.navigationEndpoint) === EPISODE_PAGE;
    if (videoType(play) !== EPISODE_TYPE && !titled) return undefined;
    const id = r.playlistItemData?.videoId ?? play?.watchEndpoint?.videoId;
    const s = showRun(cols[1], cols[2]);
    const fixed = runsText(r.fixedColumns?.[0]?.musicResponsiveListItemFixedColumnRenderer?.text).trim();
    const durationSec = clockSeconds(fixed);
    const date = dateOf(cols[1], s);
    return episode(
      { id, title: runsText(cols[0]), thumb: thumbOf(r.thumbnail), durationSec, show: s?.text, showId: s?.navigationEndpoint?.browseEndpoint?.browseId, date },
      join([s?.text, date]),
    );
  }
  if (entry?.musicTwoRowItemRenderer) {
    const r = entry.musicTwoRowItemRenderer;
    const ep = r.navigationEndpoint;
    if (videoType(ep) !== EPISODE_TYPE) return undefined;
    const s = showRun(r.subtitle);
    const date = dateOf(r.subtitle, s);
    return episode(
      { id: ep.watchEndpoint.videoId, title: runsText(r.title), thumb: thumbOf(r.thumbnailRenderer), show: s?.text, showId: s?.navigationEndpoint?.browseEndpoint?.browseId, date },
      join([s?.text, date]),
    );
  }
  return undefined;
}

// A show's card or row (the library's grid, a home shelf, search). Its subtitle: who makes it (the last
// part of YouTube's line, which is "Podcast • <author>" in search)
export function showItem(entry: Node): Show | undefined {
  const r = entry?.musicTwoRowItemRenderer ?? entry?.musicResponsiveListItemRenderer;
  if (!r) return undefined;
  const ep = r.navigationEndpoint;
  if (pageType(ep) !== PODCAST_PAGE) return undefined;
  const id: unknown = ep.browseEndpoint.browseId;
  if (typeof id !== "string" || !SHOW_ID.test(id)) return undefined;
  const two = !!entry.musicTwoRowItemRenderer;
  const cols = two ? [] : (r.flexColumns ?? []).map((c: Node) => c?.musicResponsiveListItemFlexColumnRenderer?.text);
  const title = two ? runsText(r.title) : runsText(cols[0]);
  const by = parts(two ? r.subtitle : cols[1]).pop()?.text?.trim() ?? "";
  if (!title) return undefined;
  const thumb = thumbOf(two ? r.thumbnailRenderer : r.thumbnail);
  return { kind: "podcast", id, title, subtitle: by, ...(thumb ? { thumb } : {}) };
}

// the episodes of a list's rows and the token of its next page (both layouts YouTube uses)
function rowsAndToken(contents: Node[] | undefined, onShow?: { title: string; id: string }): { items: Array<Episode | undefined>; token?: string } {
  let token: string | undefined;
  const items: Array<Episode | undefined> = [];
  for (const e of contents ?? []) {
    const t = e?.continuationItemRenderer?.continuationEndpoint?.continuationCommand?.token;
    if (typeof t === "string") token = t;
    else items.push(episodeItem(e, onShow));
  }
  return { items, ...(token ? { token } : {}) };
}

const nextToken = (shelf: Node | undefined): string | undefined => {
  const t = shelf?.continuations?.[0]?.nextContinuationData?.continuation;
  return typeof t === "string" && t ? t : undefined;
};

// A show's page: its header and its first episodes, newest first (YouTube's own "Latest" order)
export function showPage(data: Node, id: string): ShowPage & { token?: string } {
  const two = data?.contents?.twoColumnBrowseResultsRenderer;
  const h = two?.tabs?.[0]?.tabRenderer?.content?.sectionListRenderer?.contents?.[0]?.musicResponsiveHeaderRenderer;
  const title = runsText(h?.title);
  const by = runsText(h?.straplineTextOne);
  const description = runsText(h?.description?.musicDescriptionShelfRenderer?.description).trim();
  const thumb = thumbOf(h?.thumbnail);
  // Save to library: a toggle that likes the show's playlist (signed out it opens a sign-in prompt)
  let saved: boolean | undefined;
  for (const b of h?.buttons ?? []) {
    const t = b?.toggleButtonRenderer;
    if (t?.defaultServiceEndpoint?.likeEndpoint) saved = !!t.isToggled;
  }
  const shelf = two?.secondaryContents?.sectionListRenderer?.contents?.[0]?.musicShelfRenderer;
  const { items, token } = rowsAndToken(shelf?.contents, { title, id });
  return {
    title,
    subtitle: by,
    ...(thumb ? { thumb } : {}),
    description,
    ...(saved !== undefined ? { saved } : {}),
    items: items.filter((i): i is Episode => !!i),
    ...(nextToken(shelf) ?? token ? { token: nextToken(shelf) ?? token } : {}),
  };
}

// a show's later page (a musicShelfContinuation, or appended items)
export function showMore(data: Node, onShow: { title: string; id: string }): { items: Array<Episode | undefined>; token?: string } {
  const c = data?.continuationContents?.musicShelfContinuation;
  if (c) {
    const r = rowsAndToken(c.contents, onShow);
    return { items: r.items, ...(nextToken(c) ?? r.token ? { token: nextToken(c) ?? r.token } : {}) };
  }
  const appended = (data?.onResponseReceivedActions ?? []).flatMap((a: Node) => a?.appendContinuationItemsAction?.continuationItems ?? []);
  return rowsAndToken(appended, onShow);
}

// Episodes for Later (VLSE) and New Episodes (VLRDPN): a playlist page of episodes
export function episodeList(data: Node): { title: string; items: Array<Episode | undefined>; token?: string } {
  const two = data?.contents?.twoColumnBrowseResultsRenderer;
  const head = two?.tabs?.[0]?.tabRenderer?.content?.sectionListRenderer?.contents?.[0]?.musicResponsiveHeaderRenderer;
  const first = two?.secondaryContents?.sectionListRenderer?.contents?.[0];
  const shelf = first?.musicPlaylistShelfRenderer ?? first?.musicShelfRenderer;
  const r = rowsAndToken(shelf?.contents);
  const token = nextToken(shelf) ?? r.token;
  return { title: runsText(head?.title), items: r.items, ...(token ? { token } : {}) };
}

// a later page of one (either layout)
export function episodeListMore(data: Node): { items: Array<Episode | undefined>; token?: string } {
  const c = data?.continuationContents?.musicPlaylistShelfContinuation ?? data?.continuationContents?.musicShelfContinuation;
  if (c) {
    const r = rowsAndToken(c.contents);
    return { items: r.items, ...(nextToken(c) ?? r.token ? { token: nextToken(c) ?? r.token } : {}) };
  }
  const appended = (data?.onResponseReceivedActions ?? []).flatMap((a: Node) => a?.appendContinuationItemsAction?.continuationItems ?? []);
  return rowsAndToken(appended);
}

// The account's podcasts page (FEmusic_library_non_music_audio_list): its saved shows; New Episodes and
// Episodes for Later are cards in the same grid, said apart by their ids (the subtitle, YouTube's words,
// says how many episodes each has)
export function libraryPodcasts(data: Node): { shows: Show[]; later?: string; latest?: string } {
  const cards: Node[] = [];
  const walk = (o: unknown, depth = 0): void => {
    if (!o || typeof o !== "object" || depth > 30) return;
    if (Array.isArray(o)) return o.forEach((x) => walk(x, depth + 1));
    const n = o as Node;
    if (n.musicTwoRowItemRenderer) return void cards.push(n);
    for (const v of Object.values(n)) walk(v, depth + 1);
  };
  walk(data?.contents ?? data?.continuationContents);
  const shows: Show[] = [];
  let later: string | undefined, latest: string | undefined;
  const seen = new Set<string>();
  for (const c of cards) {
    const id = c.musicTwoRowItemRenderer.navigationEndpoint?.browseEndpoint?.browseId;
    if (id === "VLSE") later = runsText(c.musicTwoRowItemRenderer.subtitle).trim();
    else if (id === "VLRDPN") latest = runsText(c.musicTwoRowItemRenderer.subtitle).trim();
    const s = showItem(c);
    if (s && !seen.has(s.id)) seen.add(s.id), shows.push(s);
  }
  return { shows, ...(later !== undefined ? { later } : {}), ...(latest !== undefined ? { latest } : {}) };
}

// the library page's next page token (its grid's), when it has more shows
export function libraryToken(data: Node): string | undefined {
  let token: string | undefined;
  const walk = (o: unknown, depth = 0): void => {
    if (token || !o || typeof o !== "object" || depth > 30) return;
    if (Array.isArray(o)) return o.forEach((x) => walk(x, depth + 1));
    const n = o as Node;
    if (n.gridRenderer || n.gridContinuation) {
      token = nextToken(n.gridRenderer ?? n.gridContinuation);
      return;
    }
    for (const v of Object.values(n)) walk(v, depth + 1);
  };
  walk(data?.contents ?? data?.continuationContents);
  return token;
}

// search results: shows (type podcast) or episodes (type episode), the first page's shelves or a later one
export function podcastSearch(data: Node, type: "podcast" | "episode"): { items: Array<Show | Episode | undefined>; token?: string } {
  const list =
    data?.contents?.tabbedSearchResultsRenderer?.tabs?.[0]?.tabRenderer?.content?.sectionListRenderer?.contents ??
    (data?.continuationContents?.musicShelfContinuation ? [{ musicShelfRenderer: data.continuationContents.musicShelfContinuation }] : []);
  const items: Array<Show | Episode | undefined> = [];
  let token: string | undefined;
  for (const s of list) {
    const shelf = s?.musicShelfRenderer;
    if (!shelf) continue;
    for (const r of shelf.contents ?? []) items.push(type === "podcast" ? showItem(r) : episodeItem(r));
    token = nextToken(shelf) ?? token;
  }
  return { items, ...(token ? { token } : {}) };
}

// what the account can do with an episode, from its page (MPED<id>): mark it played or unplayed (a
// feedback token each, YouTube's Mark as played toggle) and whether it's in Episodes for Later
export function episodeActions(data: Node): { played?: string; unplayed?: string; isPlayed?: boolean; later?: boolean } {
  const h = data?.contents?.twoColumnBrowseResultsRenderer?.tabs?.[0]?.tabRenderer?.content?.sectionListRenderer?.contents?.[0]?.musicResponsiveHeaderRenderer;
  const out: { played?: string; unplayed?: string; isPlayed?: boolean; later?: boolean } = {};
  const token = h?.progress?.musicPlaybackProgressRenderer?.videoPlaybackPositionFeedbackToken;
  for (const b of h?.buttons ?? []) {
    const t = b?.toggleButtonRenderer;
    if (t?.toggledServiceEndpoint?.playlistEditEndpoint?.playlistId === "SE" && !t.defaultServiceEndpoint?.modalEndpoint) out.later = !!t.isToggled;
    for (const i of b?.menuRenderer?.items ?? []) {
      const m = i?.toggleMenuServiceItemRenderer;
      const on = m?.defaultServiceEndpoint?.feedbackEndpoint?.feedbackToken;
      const off = m?.toggledServiceEndpoint?.feedbackEndpoint?.feedbackToken;
      // the Mark as played toggle is the one whose token is the episode's playback-position token
      if (typeof on === "string" && on === token && typeof off === "string") {
        out.played = on;
        out.unplayed = off;
        out.isPlayed = !!m.isToggled;
      }
    }
  }
  return out;
}

// An episode's page (MPED<id>): what it is; its length from the queue YouTube Music would play it in
// (WEB_REMIX /next, a clock); its chapters from YouTube's chapter markers (WEB /next)
export function episodeInfo(id: string, page: Node, next?: Node, web?: Node): EpisodeInfo | undefined {
  const two = page?.contents?.twoColumnBrowseResultsRenderer;
  const h = two?.tabs?.[0]?.tabRenderer?.content?.sectionListRenderer?.contents?.[0]?.musicResponsiveHeaderRenderer;
  const title = runsText(h?.title);
  if (!title) return undefined;
  const s = showRun(h?.straplineTextOne) ?? runs(h?.straplineTextOne)[0];
  const description = runsText(two?.secondaryContents?.sectionListRenderer?.contents?.[0]?.musicDescriptionShelfRenderer?.description).trim();
  const p = progressOf(h?.progress);
  const a = episodeActions(page);
  const thumb = thumbOf(h?.thumbnail);
  return {
    id,
    title,
    show: s?.text ?? "",
    showId: s?.navigationEndpoint?.browseEndpoint?.browseId ?? "",
    ...(thumb ? { thumb } : {}),
    description,
    ...(lengthFromNext(next, id) ? { durationSec: lengthFromNext(next, id) } : {}),
    ...(dateOf(h?.subtitle) ? { date: dateOf(h?.subtitle) } : {}),
    chapters: web ? chaptersOf(web) : [],
    ...(p.progress !== undefined ? { progress: p.progress, played: a.isPlayed ?? p.played } : {}),
    ...(a.later !== undefined ? { later: a.later } : {}),
  };
}

// the episode's own row in a /next queue: its length as a clock
export function lengthFromNext(next: Node | undefined, id: string): number | undefined {
  let found: number | undefined;
  const walk = (o: unknown, depth = 0): void => {
    if (found !== undefined || !o || typeof o !== "object" || depth > 40) return;
    if (Array.isArray(o)) return o.forEach((x) => walk(x, depth + 1));
    const n = o as Node;
    const r = n.playlistPanelVideoRenderer;
    if (r && r.videoId === id) {
      found = clockSeconds(runsText(r.lengthText));
      return;
    }
    for (const v of Object.values(n)) walk(v, depth + 1);
  };
  walk(next);
  return found;
}

// YouTube's chapter markers (the description's chapters, else the automatic ones): start and title,
// in order, from the first at 0
export function chaptersOf(web: Node): Chapter[] {
  let maps: Node[] | undefined;
  const walk = (o: unknown, depth = 0): void => {
    if (maps || !o || typeof o !== "object" || depth > 40) return;
    if (Array.isArray(o)) return o.forEach((x) => walk(x, depth + 1));
    const n = o as Node;
    if (Array.isArray(n.multiMarkersPlayerBarRenderer?.markersMap)) {
      maps = n.multiMarkersPlayerBarRenderer.markersMap;
      return;
    }
    for (const v of Object.values(n)) walk(v, depth + 1);
  };
  walk(web?.playerOverlays ?? web);
  const pick = (key: string) => maps?.find((m) => m?.key === key)?.value?.chapters;
  const list: Node[] = pick("DESCRIPTION_CHAPTERS") ?? pick("AUTO_CHAPTERS") ?? [];
  const out: Chapter[] = [];
  for (const c of list) {
    const r = c?.chapterRenderer;
    const ms = Number(r?.timeRangeStartMillis);
    if (!Number.isFinite(ms) || ms < 0) continue;
    if (out.length && ms <= out[out.length - 1]!.ms) continue;
    out.push({ ms, title: runsText(r.title).trim() });
  }
  return out.length >= 2 ? out : [];
}

// a later page as a Chunk for the Pager
export function chunkOf<T>(r: { items: Array<T | undefined>; token?: string }, more: (token: string) => Promise<{ items: Array<T | undefined>; token?: string }>): Chunk<T> {
  return { items: r.items, ...(r.token ? { more: async () => chunkOf(await more(r.token!), more) } : {}) };
}
