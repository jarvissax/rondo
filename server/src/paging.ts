// Infinite scrolling. A list route answers one page plus an opaque `next` token; the token maps to
// a closure over the youtubei.js object (Feed, Search, Playlist, raw continuation string…) that can
// fetch the following page. Tokens live in memory: 30 minutes, at most 500, gone on restart or when
// cookies.txt changes (the app then reloads page one after a 410).

import { randomBytes } from "node:crypto";

// What a route reduces one YouTube response to: its items (undefined = dropped by a reducer) and,
// when YouTube has more, how to fetch the next response.
export interface Chunk<T> {
  items: Array<T | undefined>;
  more?: () => Promise<Chunk<T>>;
}

export interface Page<T> {
  items: T[];
  next?: string;
}

interface Continuation {
  scope: string;
  load: () => Promise<Page<unknown>>;
}

export const TOKEN_TTL_MS = 30 * 60_000;
export const MAX_TOKENS = 500;

export class TokenStore<V> {
  private readonly entries = new Map<string, { expires: number; value: V }>();

  constructor(
    private readonly ttlMs = TOKEN_TTL_MS,
    private readonly maxEntries = MAX_TOKENS,
    private readonly now: () => number = Date.now,
  ) {}

  put(value: V): string {
    const token = randomBytes(16).toString("base64url");
    this.entries.set(token, { expires: this.now() + this.ttlMs, value });
    for (const [key, entry] of this.entries) {
      // Map order is insertion order, so expired and over-cap entries are at the front
      if (this.entries.size <= this.maxEntries && entry.expires > this.now()) break;
      this.entries.delete(key);
    }
    return token;
  }

  // reusable until it expires, so the app can retry a page that failed on the way to it
  get(token: string): V | undefined {
    const hit = this.entries.get(token);
    if (!hit) return undefined;
    if (hit.expires > this.now()) return hit.value;
    this.entries.delete(token);
    return undefined;
  }

  clear(): void {
    this.entries.clear();
  }

  get size(): number {
    return this.entries.size;
  }
}

export interface PageOptions<T> {
  // dedupe key across all pages of one list (ids); omit to keep everything
  key?: (item: T) => string;
  // keep fetching while a page holds fewer items than this (pages of shorts/promos reduce to
  // nothing), up to `maxHops` extra YouTube requests
  min: number;
  maxHops?: number;
}

export class Pager {
  constructor(private readonly tokens = new TokenStore<Continuation>()) {}

  // One page of a list, starting with the chunk `load` fetches. Its `next` resumes after it.
  async page<T>(scope: string, load: () => Promise<Chunk<T>>, opts: PageOptions<T>, seen: ReadonlySet<string> = new Set()): Promise<Page<T>> {
    const ids = new Set(seen);
    const items: T[] = [];
    let chunk = await load();
    let more = chunk.more;
    for (let hop = 0; ; hop++) {
      for (const item of chunk.items) {
        if (item === undefined) continue;
        const id = opts.key?.(item);
        if (id !== undefined && ids.has(id)) continue;
        if (id !== undefined) ids.add(id);
        items.push(item);
      }
      more = chunk.more;
      if (!more || items.length >= opts.min || hop >= (opts.maxHops ?? 3)) break;
      try {
        chunk = await more();
      } catch (err) {
        if (!items.length) throw err;
        break; // what we have is a page; `next` retries the request that failed
      }
    }
    if (!more) return { items };
    const resume = more;
    // the seen ids are a snapshot, so fetching the same token twice gives the same page
    const next = this.tokens.put({ scope, load: () => this.page(scope, resume, opts, ids) });
    return { items, next };
  }

  // the loader of the page a token points to; undefined when unknown, expired or from another list
  resume(scope: string, token: string): (() => Promise<Page<unknown>>) | undefined {
    const hit = this.tokens.get(token);
    return hit && hit.scope === scope ? hit.load : undefined;
  }

  clear(): void {
    this.tokens.clear();
  }
}
