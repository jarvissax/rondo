// What is newer than what this server runs: the latest Rondo release (the server and the device
// firmware share a version) and yt-dlp's latest nightly build, the channel the container's yt-dlp
// follows (infra/entrypoint.sh). Asked of GitHub at most every 6 hours, in the background (it
// allows 60 unauthenticated requests an hour); the admin page shows the last answer.

import { request } from "undici";
import { dispatcher } from "./upstream.js";

export interface Release {
  version: string; // "1.0.1", "2026.11.01.233542"
  url: string; // the release page
  publishedAt: number | null;
}

export interface UpdateState {
  rondo: Release | null;
  ytdlp: Release | null;
  checkedAt: number | null;
}

export const RONDO_RELEASES = "https://api.github.com/repos/jarvissax/rondo/releases/latest";
export const YTDLP_RELEASES = "https://api.github.com/repos/yt-dlp/yt-dlp-nightly-builds/releases/latest";
const EVERY_MS = 6 * 60 * 60 * 1000;

export type FetchJson = (url: string) => Promise<unknown>;

export async function githubJson(url: string): Promise<unknown> {
  const res = await request(url, {
    dispatcher,
    headers: { "user-agent": "rondo-server", accept: "application/vnd.github+json" },
    headersTimeout: 10_000,
    bodyTimeout: 10_000,
  });
  if (res.statusCode !== 200) {
    await res.body.dump();
    throw new Error(`GitHub answered ${res.statusCode}`);
  }
  return res.body.json();
}

function toRelease(j: unknown): Release | null {
  const r = j as { tag_name?: unknown; html_url?: unknown; published_at?: unknown };
  if (typeof r?.tag_name !== "string" || typeof r.html_url !== "string") return null;
  return { version: r.tag_name.replace(/^v/, ""), url: r.html_url, publishedAt: Date.parse(String(r.published_at)) || null };
}

// "1.0.10" is newer than "1.0.9", part by part; yt-dlp's "2026.11.01.233542" compares the same way
export function isNewer(latest: string | null | undefined, current: string | null | undefined): boolean {
  if (!latest || !current) return false;
  const a = latest.split(/[.-]/);
  const b = current.split(/[.-]/);
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const x = Number(a[i] ?? 0);
    const y = Number(b[i] ?? 0);
    if (Number.isNaN(x) || Number.isNaN(y)) return latest !== current;
    if (x !== y) return x > y;
  }
  return false;
}

export class Updates {
  private state: UpdateState = { rondo: null, ytdlp: null, checkedAt: null };
  private running: Promise<void> | null = null;

  // null: never ask (tests, and servers started with UPDATE_CHECK=0)
  constructor(private readonly fetchJson: FetchJson | null = githubJson) {}

  // the last answer; an old one starts a new check for next time
  get(): UpdateState {
    const stale = !this.state.checkedAt || Date.now() - this.state.checkedAt > EVERY_MS;
    if (this.fetchJson && stale && !this.running) this.running = this.check().finally(() => (this.running = null));
    return this.state;
  }

  async check(): Promise<void> {
    const fetchJson = this.fetchJson;
    if (!fetchJson) return;
    const one = (url: string) => fetchJson(url).then(toRelease, () => null);
    const [rondo, ytdlp] = await Promise.all([one(RONDO_RELEASES), one(YTDLP_RELEASES)]);
    // a failed check keeps what the last one found
    this.state = { rondo: rondo ?? this.state.rondo, ytdlp: ytdlp ?? this.state.ytdlp, checkedAt: Date.now() };
  }
}
