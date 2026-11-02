// Video ID -> stream URLs via yt-dlp. The only place yt-dlp is used; browsing goes
// through youtubei.js. Results live in memory until 10 minutes before the URLs expire.

import { execFile } from "node:child_process";
import { access } from "node:fs/promises";
import { promisify } from "node:util";
import type { Config } from "./config.js";

const run = promisify(execFile);

export const DASH_VIDEO_ITAGS = [136, 135, 134] as const;
export const AUDIO_ITAG = 140;
export const FALLBACK_ITAG = 18;
const WANTED = new Set<number>([...DASH_VIDEO_ITAGS, AUDIO_ITAG]);
const VIDEO_ID = /^[A-Za-z0-9_-]{11}$/;
const SAFETY_MS = 10 * 60 * 1000;

export interface StreamFormat {
  itag: number;
  url: string;
  mimeType: "video/mp4" | "audio/mp4";
  codecs: string;
  bitrate: number; // bits per second
  width?: number;
  height?: number;
  audioSamplingRate?: number;
  contentLength?: number;
  expiresAt: number; // epoch ms
  httpHeaders: Record<string, string>;
}

export interface ResolvedVideo {
  id: string;
  duration: number; // seconds
  formats: Map<number, StreamFormat>;
  expiresAt: number; // earliest expiry among formats
  resolvedAt: number; // epoch ms; googlevideo refuses the URLs for the first ~0.8 s
}

export class InvalidVideoId extends Error {}

export function isVideoId(id: string): boolean {
  return VIDEO_ID.test(id);
}

interface YtdlpFormat {
  format_id?: string;
  protocol?: string;
  url?: string;
  vcodec?: string;
  acodec?: string;
  width?: number | null;
  height?: number | null;
  asr?: number | null;
  tbr?: number | null;
  filesize?: number | null;
  language_preference?: number | null;
  http_headers?: Record<string, string>;
}

function toFormat(f: YtdlpFormat, itag: number): StreamFormat | undefined {
  if (!f.url) return undefined;
  const url = new URL(f.url);
  const expire = Number(url.searchParams.get("expire"));
  const clen = Number(url.searchParams.get("clen") ?? f.filesize ?? NaN);
  const hasVideo = !!f.vcodec && f.vcodec !== "none";
  const hasAudio = !!f.acodec && f.acodec !== "none";
  const codecs = [hasVideo ? f.vcodec : undefined, hasAudio ? f.acodec : undefined].filter(Boolean).join(",");
  return {
    itag,
    url: f.url,
    mimeType: hasVideo ? "video/mp4" : "audio/mp4",
    codecs,
    bitrate: Math.round((f.tbr ?? 0) * 1000),
    width: f.width ?? undefined,
    height: f.height ?? undefined,
    audioSamplingRate: f.asr ?? undefined,
    contentLength: Number.isFinite(clen) && clen > 0 ? clen : undefined,
    expiresAt: Number.isFinite(expire) && expire > 0 ? expire * 1000 : Date.now() + 60 * 60 * 1000,
    httpHeaders: f.http_headers ?? {},
  };
}

export function parseYtdlp(id: string, json: { duration?: number; formats?: YtdlpFormat[] }): ResolvedVideo {
  const formats = new Map<number, StreamFormat>();
  // A video YouTube has dubbed has one audio track per language under the same itag ("140-0" …
  // "140-9"): the original is the one with the highest language_preference. "140-drc" is a
  // dynamic-range-compressed copy, taken only when nothing else is there. Plain https only: it
  // gives byte ranges (HLS variants are skipped).
  const rank = (f: YtdlpFormat): number => (f.language_preference ?? 0) * 2 - (/-drc$/.test(f.format_id ?? "") ? 1 : 0);
  const pick = (wanted: Set<number>) => {
    const best = new Map<number, YtdlpFormat>();
    for (const f of json.formats ?? []) {
      const m = /^(\d+)(?:-\w+)?$/.exec(f.format_id ?? "");
      const itag = Number(m?.[1]);
      if (!m || !wanted.has(itag) || f.protocol !== "https" || formats.has(itag)) continue;
      const held = best.get(itag);
      if (!held || rank(f) > rank(held)) best.set(itag, f);
    }
    for (const [itag, f] of best) {
      const format = toFormat(f, itag);
      if (format) formats.set(itag, format);
    }
  };
  pick(WANTED);
  // 18 (360p Baseline H.264 + AAC, progressive) is kept as well: it is the fallback when no
  // DASH formats exist, and the format the Y1's decoder is being compared against
  pick(new Set([FALLBACK_ITAG]));
  if (formats.size === 0) throw new Error(`no usable formats for ${id}`);
  const expiresAt = Math.min(...[...formats.values()].map((f) => f.expiresAt));
  return { id, duration: json.duration ?? 0, formats, expiresAt, resolvedAt: Date.now() };
}

export type YtdlpRunner = (args: string[]) => Promise<string>;

export function execRunner(config: Config): YtdlpRunner {
  return async (args) => {
    const { stdout } = await run(config.ytdlpBin, args, { timeout: 20_000, maxBuffer: 32 * 1024 * 1024 });
    return stdout;
  };
}

export class Resolver {
  private readonly cache = new Map<string, ResolvedVideo>();
  private readonly inflight = new Map<string, Promise<ResolvedVideo>>();

  constructor(
    private readonly config: Config,
    private readonly runYtdlp: YtdlpRunner = execRunner(config),
    // DEBUG_TTL_S shortens cache validity to exercise mid-playback re-resolves
    private readonly debugTtlMs: number | undefined = config.debugTtlS ? config.debugTtlS * 1000 : undefined,
  ) {}

  private fresh(v: ResolvedVideo): boolean {
    const now = Date.now();
    if (this.debugTtlMs !== undefined && now - v.resolvedAt > this.debugTtlMs) return false;
    return now < v.expiresAt - SAFETY_MS;
  }

  async resolve(id: string, opts: { force?: boolean } = {}): Promise<ResolvedVideo> {
    if (!isVideoId(id)) throw new InvalidVideoId(id);
    const hit = this.cache.get(id);
    if (!opts.force && hit && this.fresh(hit)) return hit;
    const pending = this.inflight.get(id);
    if (pending) return pending;
    const job = this.runAndStore(id).finally(() => this.inflight.delete(id));
    this.inflight.set(id, job);
    return job;
  }

  invalidate(id: string): void {
    this.cache.delete(id);
  }

  private async runAndStore(id: string): Promise<ResolvedVideo> {
    // HLS and DASH manifests are never used (plain https formats only): skipping them saves ~1.5 s
    const args = ["-J", "--no-playlist", "--no-warnings", "--extractor-args", "youtube:skip=hls,dash,translated_subs"];
    if (this.config.ytdlpUseCookies && (await access(this.config.cookiesFile).then(() => true, () => false))) {
      args.push("--cookies", this.config.cookiesFile);
    }
    args.push("--", `https://www.youtube.com/watch?v=${id}`);
    const video = parseYtdlp(id, JSON.parse(await this.runYtdlp(args)));
    this.cache.set(id, video);
    if (this.cache.size > 500) {
      const oldest = this.cache.keys().next().value as string;
      this.invalidate(oldest);
    }
    return video;
  }
}
