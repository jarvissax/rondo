// What is newer than what this server runs: the latest Rondo release (the server and the device
// firmware share a version) and yt-dlp's latest nightly build, the channel the container's yt-dlp
// follows (infra/entrypoint.sh). Asked at most every 6 hours, in the background (GitHub allows 60
// unauthenticated requests an hour); the admin page shows the last answer. Rondo's comes from the
// update page's latest.json, the same answer the dashboard and the update page get, which appears
// only once a release's rom.zip is out (.github/workflows/release.yml).

import type { FastifyInstance } from "fastify";
import { request } from "undici";
import { dispatcher } from "./upstream.js";

export interface Release {
  version: string; // "1.0.1", "2026.11.01.233542"
  url: string; // the release page
  publishedAt: number | null;
  apk?: { url: string; size: number }; // a Rondo release's APK: the update the Y1 installs
  firmware?: number; // Rondo: the firmware the release needs (firmwareLevel in app/app/build.gradle.kts)
}

export interface UpdateState {
  rondo: Release | null;
  ytdlp: Release | null;
  checkedAt: number | null;
}

export const RONDO_LATEST = "https://jarvissax.github.io/rondo/latest.json";
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

type GithubAsset = { name?: unknown; browser_download_url?: unknown; size?: unknown };

function toRelease(j: unknown): Release | null {
  const r = j as { tag_name?: unknown; html_url?: unknown; published_at?: unknown; assets?: GithubAsset[] };
  if (typeof r?.tag_name !== "string" || typeof r.html_url !== "string") return null;
  const release: Release = { version: r.tag_name.replace(/^v/, ""), url: r.html_url, publishedAt: Date.parse(String(r.published_at)) || null };
  const apk = (r.assets ?? []).find((a) => typeof a?.name === "string" && /^rondo-[\w.-]+\.apk$/.test(a.name));
  if (apk && typeof apk.browser_download_url === "string" && typeof apk.size === "number") {
    release.apk = { url: apk.browser_download_url, size: apk.size };
  }
  return release;
}

// latest.json: {"version", "file" (next to it), "size", "firmware", "notes" (the release page), "published"}
function toRondo(j: unknown): Release | null {
  const r = j as { version?: unknown; file?: unknown; size?: unknown; firmware?: unknown; notes?: unknown; published?: unknown };
  if (typeof r?.version !== "string" || !r.version) return null;
  const release: Release = {
    version: r.version,
    url: typeof r.notes === "string" ? r.notes : `https://github.com/jarvissax/rondo/releases/tag/v${r.version}`,
    publishedAt: Date.parse(String(r.published)) || null,
    firmware: Number(r.firmware) || 1,
  };
  if (typeof r.file === "string" && typeof r.size === "number") release.apk = { url: new URL(r.file, RONDO_LATEST).href, size: r.size };
  return release;
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

  // the latest Rondo release, after a first check if none has been made yet
  async latest(): Promise<Release | null> {
    this.get();
    if (!this.state.checkedAt && this.running) await this.running;
    return this.state.rondo;
  }

  // the last answer; an old one starts a new check for next time
  get(): UpdateState {
    const stale = !this.state.checkedAt || Date.now() - this.state.checkedAt > EVERY_MS;
    if (this.fetchJson && stale && !this.running) this.running = this.check().finally(() => (this.running = null));
    return this.state;
  }

  async check(): Promise<void> {
    const fetchJson = this.fetchJson;
    if (!fetchJson) return;
    const [rondo, ytdlp] = await Promise.all([
      fetchJson(RONDO_LATEST).then(toRondo, () => null),
      fetchJson(YTDLP_RELEASES).then(toRelease, () => null),
    ]);
    // a failed check keeps what the last one found
    this.state = { rondo: rondo ?? this.state.rondo, ytdlp: ytdlp ?? this.state.ytdlp, checkedAt: Date.now() };
  }
}

// Settings › Software update on the Rondo (device token, like every route after the auth hook).
// GET /update names the newest Rondo that has an update APK; GET /update/rondo.apk passes that APK
// through as it downloads, so the Y1 talks only to its server. Android installs it only when it's
// signed with the same key as the firmware's copy. ?firmware=N is the Rondo's firmware: a release
// that needs a newer one comes with its firmware number, and the Rondo points to Innioasis Updater
// CE instead of installing it. A Rondo that doesn't say (1.0.0) hears only of first-firmware ones.
export function registerUpdateRoutes(app: FastifyInstance, updates: Updates | undefined): void {
  app.get<{ Querystring: { firmware?: string } }>("/update", async (req) => {
    const r = await updates?.latest();
    if (!r?.apk) return { version: null };
    const firmware = r.firmware ?? 1;
    if (!Number(req.query.firmware) && firmware > 1) return { version: null };
    return { version: r.version, size: r.apk.size, notes: r.url, firmware };
  });

  app.get("/update/rondo.apk", async (_req, reply) => {
    const r = await updates?.latest();
    if (!r?.apk) return reply.code(404).send({ error: "none" });
    const res = await request(r.apk.url, {
      dispatcher, // follows a redirect, if the site sends one
      headers: { "user-agent": "rondo-server" },
      headersTimeout: 15_000,
      bodyTimeout: 60_000,
    });
    if (res.statusCode !== 200) {
      await res.body.dump();
      return reply.code(502).send({ error: "github" });
    }
    return reply
      .type("application/vnd.android.package-archive")
      .header("content-length", String(res.headers["content-length"] ?? r.apk.size))
      .header("cache-control", "no-store")
      .send(res.body);
  });
}
