// RSS podcasts for a Rondo (phase 3). A Rondo reads feeds and plays episodes straight from the
// podcasts' hosts itself, in both modes; the server only helps, with two routes of its own:
//   GET /podcasts/feed?url=&offset=&limit=&since=   a feed read (rssFeed.ts) and answered a page at a time
//   GET /podcasts/index/search?q=&max=               shows from Podcast Index (podcastIndex.ts)
// Feeds are kept in memory for 10 minutes, then asked again with ETag / If-Modified-Since. Metadata
// only: no feed or episode is written to disk.

import type { FastifyBaseLogger, FastifyInstance, FastifyReply } from "fastify";
import { type Conditional, feedAddress, FeedError, type FeedSource } from "./feedFetch.js";
import { IndexError, type PodcastIndex } from "./podcastIndex.js";
import { type FeedEpisode, type ParsedFeed, parseFeed } from "./rssFeed.js";

export const FEED_TTL_MS = 10 * 60_000;
// what all the feeds kept may weigh together (their text, roughly), and how many there may be
export const FEED_CACHE_BYTES = 64 * 1024 * 1024;
export const FEED_CACHE_ENTRIES = 200;
export const PAGE_DEFAULT = 100;
export const PAGE_MAX = 500;

// the compact feed, with the address it was read from in the end
export interface CompactFeed extends ParsedFeed {
  feed: string;
}

interface Entry {
  feed: CompactFeed;
  etag?: string;
  lastModified?: string;
  at: number;
  size: number;
}

// about how much memory a feed takes: its texts' lengths
function weight(f: CompactFeed): number {
  let n = 500 + f.title.length + f.description.length + f.art.length + f.link.length + f.author.length + f.feed.length;
  for (const e of f.episodes) n += 200 + e.guid.length + e.title.length + e.url.length + e.image.length + e.chapters.length + e.notes.length;
  return n;
}

// The feeds read lately (least recently used goes first), and the reads under way (two Rondo screens
// asking for the same feed share one).
export class FeedCache {
  private readonly entries = new Map<string, Entry>();
  private readonly loading = new Map<string, Promise<CompactFeed>>();
  private total = 0;

  constructor(
    private readonly source: FeedSource,
    private readonly opts: { ttlMs?: number; maxBytes?: number; maxEntries?: number; now?: () => number } = {},
  ) {}

  private now(): number {
    return (this.opts.now ?? Date.now)();
  }

  get size(): { entries: number; bytes: number } {
    return { entries: this.entries.size, bytes: this.total };
  }

  get(address: string): Promise<CompactFeed> {
    const hit = this.entries.get(address);
    if (hit && this.now() - hit.at < (this.opts.ttlMs ?? FEED_TTL_MS)) {
      this.entries.delete(address); // most recently used goes last
      this.entries.set(address, hit);
      return Promise.resolve(hit.feed);
    }
    const pending = this.loading.get(address);
    if (pending) return pending;
    const load = this.load(address, hit).finally(() => this.loading.delete(address));
    this.loading.set(address, load);
    return load;
  }

  private async load(address: string, old: Entry | undefined): Promise<CompactFeed> {
    const cond: Conditional = old ? { etag: old.etag, lastModified: old.lastModified } : {};
    const res = await this.source(address, cond);
    if (res.notModified) {
      if (!old) throw new FeedError("upstream", 304);
      old.at = this.now();
      old.feed.feed = res.feedUrl;
      this.put(address, old);
      return old.feed;
    }
    const parsed = parseFeed(res.text);
    if (!parsed) throw new FeedError("not_feed");
    const feed: CompactFeed = { feed: res.feedUrl, ...parsed };
    this.put(address, { feed, etag: res.etag, lastModified: res.lastModified, at: this.now(), size: weight(feed) });
    return feed;
  }

  private put(address: string, entry: Entry): void {
    const old = this.entries.get(address);
    if (old) {
      this.total -= old.size;
      this.entries.delete(address);
    }
    const maxBytes = this.opts.maxBytes ?? FEED_CACHE_BYTES;
    if (entry.size > maxBytes) return; // bigger than the whole cache: answered, not kept
    this.entries.set(address, entry);
    this.total += entry.size;
    while (this.total > maxBytes || this.entries.size > (this.opts.maxEntries ?? FEED_CACHE_ENTRIES)) {
      const [oldest, e] = this.entries.entries().next().value as [string, Entry];
      this.entries.delete(oldest);
      this.total -= e.size;
    }
  }
}

// a whole number query value, or `fallback` when it isn't one; clamped to min..max
function whole(v: unknown, fallback: number, min: number, max: number): number {
  const n = typeof v === "string" && v.trim() ? Number(v) : NaN;
  return Number.isFinite(n) ? Math.min(Math.max(Math.floor(n), min), max) : fallback;
}

// A page of a feed: the channel, `count` (every episode), and `limit` episodes from `offset` (with
// `since`, of the episodes dated after it only), `next` the next page's offset or null on the last.
export function feedPage(feed: CompactFeed, q: { offset?: unknown; limit?: unknown; since?: unknown }) {
  const offset = whole(q.offset, 0, 0, Number.MAX_SAFE_INTEGER);
  const limit = whole(q.limit, PAGE_DEFAULT, 1, PAGE_MAX);
  const since = whole(q.since, -1, 0, Number.MAX_SAFE_INTEGER);
  const list: FeedEpisode[] = since < 0 ? feed.episodes : feed.episodes.filter((e) => e.date > since);
  const { episodes: _all, ...channel } = feed;
  return { ...channel, offset, next: offset + limit < list.length ? offset + limit : null, episodes: list.slice(offset, offset + limit) };
}

function feedFailed(reply: FastifyReply, err: unknown) {
  const e = err instanceof FeedError ? err : new FeedError("upstream");
  if (e.code === "url") return reply.code(400).send({ error: "url" });
  if (e.code === "timeout") return reply.code(504).send({ error: "timeout" });
  if (e.code === "too_big") return reply.code(413).send({ error: "too_big" });
  if (e.code === "not_feed") return reply.code(422).send({ error: "not_feed" });
  return reply.code(502).send({ error: "upstream", status: e.status });
}

export function registerRssRoutes(app: FastifyInstance, deps: { feeds: FeedCache; index: PodcastIndex; log?: FastifyBaseLogger }): void {
  const log = deps.log ?? app.log;

  app.get<{ Querystring: { url?: unknown; offset?: unknown; limit?: unknown; since?: unknown } }>("/podcasts/feed", async (req, reply) => {
    const url = feedAddress(req.query.url);
    if (!url) return reply.code(400).send({ error: "url" });
    try {
      return feedPage(await deps.feeds.get(url.toString()), req.query);
    } catch (err) {
      // a podcast's host failing is the host's business, not this server's: a line in the log, not a warning
      if (!(err instanceof FeedError) || err.code !== "url") log.info({ host: url.hostname, why: String((err as Error)?.message ?? err).slice(0, 120) }, "a podcast feed couldn't be read");
      return feedFailed(reply, err);
    }
  });

  app.get<{ Querystring: { q?: unknown; max?: unknown } }>("/podcasts/index/search", async (req, reply) => {
    const q = typeof req.query.q === "string" ? req.query.q.trim().slice(0, 200) : "";
    if (!q) return reply.code(400).send({ error: "query" });
    if (!deps.index.hasKey()) return reply.code(503).send({ error: "no_key" });
    try {
      return await deps.index.search(q, whole(req.query.max, 20, 1, 100));
    } catch (err) {
      if (err instanceof IndexError && err.code === "no_key") return reply.code(503).send({ error: "no_key" });
      // the owner's to fix (a key Podcast Index refuses) or Podcast Index down: the admin page's Activity
      log.warn(`Podcast Index search failed: ${err instanceof IndexError ? err.message : "no answer"}`);
      return reply.code(502).send({ error: "upstream" });
    }
  });
}
