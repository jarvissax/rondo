// Podcast search in Podcast Index (https://podcastindex.org), with the server's own free API key
// (pasted on the admin page's Podcasts page, kept in /data/config.json). Every request is signed:
// X-Auth-Key, X-Auth-Date (unix seconds) and Authorization = SHA-1 of key + secret + date, in hex.
// The secret is never logged and never sent back to the admin page.

import { createHash } from "node:crypto";
import { request } from "undici";
import { TtlCache } from "./ttl.js";
import { dispatcher } from "./upstream.js";

export const INDEX_BASE = "https://api.podcastindex.org/api/1.0";
const USER_AGENT = "Rondo server (https://github.com/jarvissax/rondo)";
const TIMEOUT_MS = 10_000;
const CACHE_MS = 10 * 60_000;

export interface IndexKey {
  key: string;
  secret: string;
}

// a show as a Rondo lists it: its feed's address, which the Rondo then reads
export interface IndexShow {
  feed: string;
  title: string;
  author: string;
  art: string;
  id: number; // Podcast Index's feed id
  itunesId: number | null;
  count: number; // episodes
  language: string;
  explicit: boolean;
}

export type IndexGet = (url: string, headers: Record<string, string>) => Promise<{ status: number; body?: unknown }>;

export const indexGet: IndexGet = async (url, headers) => {
  const res = await request(url, { dispatcher, headers: { ...headers, Accept: "application/json" }, signal: AbortSignal.timeout(TIMEOUT_MS) });
  if (res.statusCode !== 200) {
    await res.body.dump();
    return { status: res.statusCode };
  }
  return { status: 200, body: await res.body.json() };
};

// the headers Podcast Index wants on every request
export function indexHeaders(k: IndexKey, nowSec: number): Record<string, string> {
  const date = String(Math.floor(nowSec));
  return {
    "User-Agent": USER_AGENT,
    "X-Auth-Key": k.key,
    "X-Auth-Date": date,
    Authorization: createHash("sha1").update(k.key + k.secret + date).digest("hex"),
  };
}

// no_key: none set; key: Podcast Index refused it; upstream: it failed or didn't answer
export class IndexError extends Error {
  constructor(
    readonly code: "no_key" | "key" | "upstream",
    readonly status = 0,
  ) {
    super(status ? `${code} (HTTP ${status})` : code);
  }
}

const str = (v: unknown): string => (typeof v === "string" ? v.trim() : "");
const num = (v: unknown): number => {
  const n = typeof v === "number" ? v : typeof v === "string" ? Number(v) : NaN;
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
};

export function indexShow(f: any): IndexShow | undefined {
  const feed = str(f?.url);
  if (!/^https?:\/\//i.test(feed) || f.dead === 1 || f.dead === true) return undefined;
  return {
    feed,
    title: str(f.title),
    author: str(f.author) || str(f.ownerName),
    art: str(f.artwork) || str(f.image),
    id: num(f.id),
    itunesId: num(f.itunesId) || null,
    count: num(f.episodeCount),
    language: str(f.language).toLowerCase(),
    explicit: f.explicit === true || f.explicit === 1 || f.explicit === "true",
  };
}

export class PodcastIndex {
  private readonly cache = new TtlCache<{ items: IndexShow[] }>(CACHE_MS, 200);

  constructor(
    private readonly key: () => IndexKey | undefined,
    private readonly get: IndexGet = indexGet,
    private readonly now: () => number = Date.now,
  ) {}

  hasKey(): boolean {
    return !!this.key();
  }

  // a new or removed key: answers found with the old one go
  forget(): void {
    this.cache.clear();
  }

  private async byTerm(k: IndexKey, q: string, max: number): Promise<{ items: IndexShow[] }> {
    const url = `${INDEX_BASE}/search/byterm?${new URLSearchParams({ q, max: String(max) })}`;
    let res: Awaited<ReturnType<IndexGet>>;
    try {
      res = await this.get(url, indexHeaders(k, this.now() / 1000));
    } catch {
      throw new IndexError("upstream");
    }
    if (res.status === 401 || res.status === 403) throw new IndexError("key", res.status);
    if (res.status !== 200) throw new IndexError("upstream", res.status);
    const feeds = (res.body as { feeds?: unknown })?.feeds;
    if (!Array.isArray(feeds)) throw new IndexError("upstream");
    return { items: feeds.map(indexShow).filter((s): s is IndexShow => !!s) };
  }

  // shows matching the words, as Podcast Index ranks them (kept 10 minutes)
  search(q: string, max: number): Promise<{ items: IndexShow[] }> {
    const k = this.key();
    if (!k) return Promise.reject(new IndexError("no_key"));
    return this.cache.get(`${max}|${q.toLowerCase()}`, () => this.byTerm(k, q, max));
  }

  // the admin page's Check: one search with the saved key
  async check(): Promise<{ ok: true; ms: number } | { ok: false; error: IndexError["code"]; status?: number }> {
    const k = this.key();
    if (!k) return { ok: false, error: "no_key" };
    const started = this.now();
    try {
      await this.byTerm(k, "music", 1);
      return { ok: true, ms: Math.max(0, Math.round(this.now() - started)) };
    } catch (err) {
      const e = err instanceof IndexError ? err : new IndexError("upstream");
      return { ok: false, error: e.code, ...(e.status ? { status: e.status } : {}) };
    }
  }
}
