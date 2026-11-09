// /resolve, /mpd and the /s range proxy: everything between a video ID and bytes on the device.

import { Readable } from "node:stream";
import type { FastifyInstance, FastifyReply, FastifyRequest } from "fastify";
import type { Config } from "./config.js";
import type { Health } from "./health.js";
import { renderMpd } from "./mpd.js";
import {
  AUDIO_ITAG,
  OPUS_ITAG,
  DASH_VIDEO_ITAGS,
  FALLBACK_ITAG,
  InvalidVideoId,
  isVideoId,
  type ResolvedVideo,
  type Resolver,
} from "./resolver.js";
import { SidxCache, type SegmentRanges } from "./sidx.js";
import { fetchRange, type UpstreamResponse } from "./upstream.js";

const STREAM_ITAGS = new Set<number>([...DASH_VIDEO_ITAGS, AUDIO_ITAG, OPUS_ITAG, FALLBACK_ITAG]);
// googlevideo throttles single requests above ~10 MB to playback speed (a 13.4 MB range took
// 106 s, a 9.1 MB one 0.2 s), so bigger ranges are fetched as consecutive 8 MiB pieces.
export const UPSTREAM_CHUNK = 8 * 1024 * 1024;
const sleep = (ms: number) => new Promise((ok) => setTimeout(ok, ms));

export class HttpError extends Error {
  constructor(
    readonly statusCode: number,
    readonly code: string,
  ) {
    super(code);
  }
}

type Range = { start: number; end?: number };

// Single ranges only: "bytes=a-b", "bytes=a-", "bytes=-n".
export function parseRange(header: string | undefined, total: number | undefined): Range | null | "invalid" {
  if (!header) return null;
  const m = /^bytes=(\d*)-(\d*)$/.exec(header.trim());
  if (!m || (m[1] === "" && m[2] === "")) return "invalid";
  if (m[1] === "") {
    if (total === undefined) return "invalid";
    return { start: Math.max(0, total - Number(m[2])), end: total - 1 };
  }
  const start = Number(m[1]);
  let end = m[2] === "" ? undefined : Number(m[2]);
  if (end !== undefined && end < start) return "invalid";
  if (total !== undefined) end = Math.min(end ?? total - 1, total - 1);
  return { start, end };
}

function headerValue(h: string | string[] | undefined): string | undefined {
  return Array.isArray(h) ? h[0] : h;
}

export interface UpstreamTiming {
  warmupMs: number; // minimum URL age before the first upstream request
  retryDelaysMs: number[]; // waits before each retry on the same URL
}

export const DEFAULT_TIMING: UpstreamTiming = { warmupMs: 800, retryDelaysMs: [300, 600, 1200] };

export function registerStreamRoutes(
  app: FastifyInstance,
  deps: { config: Config; resolver: Resolver; health: Health; timing?: UpstreamTiming },
): void {
  const { config, resolver, health } = deps;
  const timing = deps.timing ?? DEFAULT_TIMING;
  const sidx = new SidxCache();

  async function resolve(id: string): Promise<ResolvedVideo> {
    if (!isVideoId(id)) throw new HttpError(400, "bad_id");
    try {
      const video = await resolver.resolve(id);
      health.recordResolve(true);
      return video;
    } catch (err) {
      if (err instanceof InvalidVideoId) throw new HttpError(400, "bad_id");
      health.recordResolve(false);
      app.log.warn({ id, err: (err as Error).message.slice(0, 300) }, "resolve failed");
      throw new HttpError(502, "resolve");
    }
  }

  // Opens an upstream range. A freshly resolved googlevideo URL answers 403 for its first
  // ~0.6–0.8 s and then serves normally (TV-client URLs, measured 2026-10-30), so the first request waits until the URLs are warmupMs old, and a
  // 403/410 or network error is retried on the same URL with backoff before resolving once
  // more; a second round of failures gives up with 502.
  async function openUpstream(
    id: string,
    itag: number,
    start: number,
    end: number | undefined,
    signal?: AbortSignal,
  ): Promise<UpstreamResponse> {
    for (let round = 0; round < 2; round++) {
      if (round > 0) resolver.invalidate(id);
      const video = await resolve(id);
      const format = video.formats.get(itag);
      if (!format) throw new HttpError(404, "no_format");
      const young = video.resolvedAt + timing.warmupMs - Date.now();
      if (young > 0) await sleep(young);
      for (let attempt = 0; attempt <= timing.retryDelaysMs.length; attempt++) {
        if (attempt > 0) await sleep(timing.retryDelaysMs[attempt - 1]!);
        let res: UpstreamResponse;
        try {
          res = await fetchRange(format, start, end, signal);
        } catch (err) {
          if (signal?.aborted) throw err;
          app.log.debug({ id, itag, err: (err as Error).message, round, attempt }, "upstream request failed");
          continue;
        }
        if (res.statusCode < 300) return res;
        await res.body.dump();
        app.log.debug({ id, itag, status: res.statusCode, round, attempt }, "upstream refused range");
        if (res.statusCode !== 403 && res.statusCode !== 410) throw new HttpError(502, "upstream");
      }
      app.log.warn({ id, itag, round }, "upstream kept refusing the range");
    }
    throw new HttpError(502, "upstream");
  }

  function rangesFor(id: string, itag: number): Promise<SegmentRanges> {
    return sidx.get(`${id}:${itag}`, async (start, end) => {
      const res = await openUpstream(id, itag, start, end);
      return new Uint8Array(await res.body.arrayBuffer());
    });
  }

  app.setErrorHandler((err, _req, reply) => {
    if (err instanceof HttpError) return reply.code(err.statusCode).send({ error: err.code });
    if ((err as { statusCode?: number }).statusCode === 429) return reply.send(err);
    app.log.error({ err }, "unhandled error");
    return reply.code(500).send({ error: "internal" });
  });

  app.get<{ Params: { id: string } }>("/resolve/:id", async (req) => {
    const video = await resolve(req.params.id);
    const formats: Record<string, unknown> = {};
    for (const [itag, f] of video.formats) {
      formats[itag] = {
        bitrate: f.bitrate,
        w: f.width,
        h: f.height,
        codecs: f.codecs,
        contentLength: f.contentLength,
        expiresAt: f.expiresAt,
      };
    }
    return { duration: video.duration, formats };
  });

  app.get<{ Params: { id: string }; Querystring: { token?: string } }>("/mpd/:id", async (req, reply) => {
    const { id } = req.params;
    const video = await resolve(id);
    const itags = [...DASH_VIDEO_ITAGS, AUDIO_ITAG].filter((i) => video.formats.has(i));
    const ranges = new Map<number, SegmentRanges>();
    await Promise.all(
      itags.map(async (itag) => {
        try {
          ranges.set(itag, await rangesFor(id, itag));
        } catch (err) {
          app.log.warn({ id, itag, err: (err as Error).message }, "sidx probe failed");
        }
      }),
    );
    let xml: string;
    try {
      // a desktop player that authenticated with ?token= needs it on the segment URLs too
      const token = req.headers.authorization ? undefined : req.query.token;
      xml = renderMpd({ video, ranges, publicBase: config.publicBase, token });
    } catch {
      throw new HttpError(404, "no_dash");
    }
    return reply.type("application/dash+xml").header("cache-control", "no-store").send(xml);
  });

  app.get<{ Params: { id: string; itag: string } }>("/s/:id/:itag", async (req: FastifyRequest<{ Params: { id: string; itag: string } }>, reply: FastifyReply) => {
    const { id } = req.params;
    const itag = Number(req.params.itag);
    if (!STREAM_ITAGS.has(itag)) throw new HttpError(404, "no_format");
    const format = (await resolve(id)).formats.get(itag);
    if (!format) throw new HttpError(404, "no_format");
    const total = format.contentLength;

    const range = parseRange(req.headers.range, total);
    if (range === "invalid" || (range && total !== undefined && range.start >= total)) {
      return reply.code(416).header("content-range", `bytes */${total ?? "*"}`).send({ error: "range" });
    }
    // ExoPlayer's first progressive request (music) carries no Range: serve the whole file
    const start = range?.start ?? 0;
    const end = range ? range.end : total !== undefined ? total - 1 : undefined;

    const abort = new AbortController();
    reply.raw.on("close", () => abort.abort());

    if (end !== undefined && end - start + 1 > UPSTREAM_CHUNK) {
      // the first piece is opened before answering, so an upstream failure is still a clean 502
      const first = await openUpstream(id, itag, start, Math.min(end, start + UPSTREAM_CHUNK - 1), abort.signal);
      async function* pieces() {
        yield* first.body;
        for (let a = start + UPSTREAM_CHUNK; a <= end!; a += UPSTREAM_CHUNK) {
          const res = await openUpstream(id, itag, a, Math.min(end!, a + UPSTREAM_CHUNK - 1), abort.signal);
          yield* res.body;
        }
      }
      reply
        .code(range ? 206 : 200)
        .header("content-type", format.mimeType)
        .header("accept-ranges", "bytes")
        .header("cache-control", "no-store")
        .header("content-length", end - start + 1);
      if (range) reply.header("content-range", `bytes ${start}-${end}/${total ?? "*"}`);
      return reply.send(Readable.from(pieces(), { objectMode: false }));
    }

    const res = await openUpstream(id, itag, start, end, abort.signal);

    const length = Number(headerValue(res.headers["content-length"]));
    reply
      .header("content-type", headerValue(res.headers["content-type"]) ?? format.mimeType)
      .header("accept-ranges", "bytes")
      .header("cache-control", "no-store");
    if (Number.isFinite(length)) reply.header("content-length", length);
    if (range) {
      // upstream only ever sees the slice, so its own Content-Range (if any) has the wrong total
      const last = Number.isFinite(length) ? start + length - 1 : end;
      reply.code(206).header("content-range", `bytes ${start}-${last ?? ""}/${total ?? "*"}`);
    }
    return reply.send(res.body);
  });
}
