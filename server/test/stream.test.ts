// End to end through Fastify against a fake googlevideo: range translation, the 206
// headers the device relies on, and one re-resolve when upstream says 403.

import { createServer, type Server } from "node:http";
import type { AddressInfo } from "node:net";
import { readFileSync } from "node:fs";
import { afterAll, beforeAll, beforeEach, describe, expect, it } from "vitest";
import type { FastifyInstance } from "fastify";
import { buildApp } from "../src/app.js";
import { loadConfig } from "../src/config.js";
import { parseRange, UPSTREAM_CHUNK, type UpstreamTiming } from "../src/stream.js";
import { dashFile } from "./mp4.js";

const TOKEN = "t".repeat(64);
const ID = "dQw4w9WgXcQ";
const FILE = dashFile(2000, 50_000); // same bytes for every itag
const FILE_REF = { current: FILE }; // what the fake upstream serves; swapped by the big-range test
const fixture = JSON.parse(readFileSync(new URL("./fixtures/ytdlp-dQw4w9WgXcQ.json", import.meta.url), "utf8"));

let upstream: Server;
let base = "";
const seen: Array<{ range: string | null; header: string | undefined; ua: string | undefined; sig: string | null }> = [];
let refuse: (sig: string | null) => boolean = () => false;
let resolves = 0;

beforeAll(async () => {
  upstream = createServer((req, res) => {
    const url = new URL(req.url!, "http://x");
    const range = url.searchParams.get("range");
    const sig = url.searchParams.get("sig");
    seen.push({ range, header: req.headers.range, ua: req.headers["user-agent"], sig });
    if (refuse(sig)) {
      res.writeHead(403).end();
      return;
    }
    const file = FILE_REF.current;
    const [a, b] = (range ?? `0-${file.length - 1}`).split("-").map(Number);
    const body = file.subarray(a!, Math.min(b!, file.length - 1) + 1);
    // like googlevideo: the range= slice is the whole resource, so its Content-Range total is the slice length
    res
      .writeHead(200, {
        "content-type": "video/mp4",
        "content-length": body.length,
        "content-range": `bytes 0-${body.length - 1}/${body.length}`,
      })
      .end(body);
  });
  await new Promise<void>((ok) => upstream.listen(0, "127.0.0.1", ok));
  base = `http://127.0.0.1:${(upstream.address() as AddressInfo).port}/videoplayback`;
});

afterAll(() => upstream.close());

// test-speed version of the real 800 ms warm-up and 300/600/1200 ms retries
const FAST: UpstreamTiming = { warmupMs: 0, retryDelaysMs: [5, 5] };
const issuedAt = new Map<string, number>(); // sig -> when the fake yt-dlp handed it out

async function makeApp(timing: UpstreamTiming = FAST): Promise<FastifyInstance> {
  const config = loadConfig({ API_TOKEN: TOKEN, COOKIES_FILE: "/nonexistent", LOG_LEVEL: "silent" });
  const { app } = await buildApp(config, {
    upstreamTiming: timing,
    ytdlpRunner: async () => {
      resolves++;
      issuedAt.set(`s${resolves}`, Date.now());
      const expire = Math.floor(Date.now() / 1000) + 3600;
      return JSON.stringify(fixture)
        .replace(/https:\/\/rr1---sn-test\.googlevideo\.com\/videoplayback/g, base)
        .replace(/clen=\d+/g, `clen=${FILE_REF.current.length}`)
        .replace(/expire=\d+/g, `expire=${expire}`)
        .replace(/sig=FAKE/g, `sig=s${resolves}`);
    },
  });
  await app.ready();
  return app;
}

const auth = { authorization: `Bearer ${TOKEN}` };

describe("range proxy", () => {
  let app: FastifyInstance;
  beforeEach(async () => {
    seen.length = 0;
    refuse = () => false;
    resolves = 0;
    app = await makeApp();
  });

  it("translates Range into range= only, and answers 206 with the real file size", async () => {
    const res = await app.inject({ url: `/s/${ID}/140`, headers: { ...auth, range: "bytes=0-1023" } });
    expect(res.statusCode).toBe(206);
    expect(res.headers["content-range"]).toBe(`bytes 0-1023/${FILE.length}`);
    expect(res.headers["content-length"]).toBe("1024");
    expect(res.headers["accept-ranges"]).toBe("bytes");
    expect(res.rawPayload.length).toBe(1024);
    expect(seen.at(-1)).toMatchObject({ range: "0-1023", header: undefined });
    expect(seen.at(-1)!.ua).toMatch(/Mozilla/); // yt-dlp's header, not the device's
  });

  it("serves the Opus stream (251) the Y1 plays songs as, and 404s an itag it doesn't serve", async () => {
    const opus = await app.inject({ url: `/s/${ID}/251`, headers: { ...auth, range: "bytes=0-1023" } });
    expect(opus.statusCode).toBe(206);
    expect(opus.rawPayload.length).toBe(1024);
    const other = await app.inject({ url: `/s/${ID}/250`, headers: { ...auth, range: "bytes=0-1023" } });
    expect(other.statusCode).toBe(404);
  });

  it("closes open-ended ranges at the end of the file", async () => {
    const res = await app.inject({ url: `/s/${ID}/140`, headers: { ...auth, range: "bytes=50000-" } });
    expect(res.statusCode).toBe(206);
    expect(res.headers["content-range"]).toBe(`bytes 50000-${FILE.length - 1}/${FILE.length}`);
    expect(seen.at(-1)!.range).toBe(`50000-${FILE.length - 1}`);
  });

  it("returns the exact bytes of a mid-file range", async () => {
    const res = await app.inject({ url: `/s/${ID}/136`, headers: { ...auth, range: "bytes=2024-2131" } });
    expect(res.statusCode).toBe(206);
    expect(Buffer.from(res.rawPayload).equals(Buffer.from(FILE.subarray(2024, 2132)))).toBe(true);
    expect(res.rawPayload.subarray(4, 8).toString()).toBe("sidx");
  });

  it("splits ranges above 8 MiB into consecutive upstream pieces with exact bytes", async () => {
    const big = dashFile(2000, 20 * 1024 * 1024);
    FILE_REF.current = big;
    try {
      const bigApp = await makeApp(); // its resolver reports the big file's length
      const res = await bigApp.inject({ url: `/s/${ID}/135`, headers: { ...auth, range: "bytes=1000-" } });
      expect(res.statusCode).toBe(206);
      expect(res.headers["content-length"]).toBe(String(big.length - 1000));
      expect(res.headers["content-range"]).toBe(`bytes 1000-${big.length - 1}/${big.length}`);
      expect(Buffer.from(res.rawPayload).equals(Buffer.from(big.subarray(1000)))).toBe(true);
      const pieces = seen.map((s) => s.range);
      expect(pieces.length).toBe(3);
      expect(pieces[0]).toBe(`1000-${1000 + UPSTREAM_CHUNK - 1}`);
    } finally {
      FILE_REF.current = FILE;
    }
  });

  it("serves the whole file with 200 when there is no Range header", async () => {
    const res = await app.inject({ url: `/s/${ID}/140`, headers: auth });
    expect(res.statusCode).toBe(200);
    expect(res.rawPayload.length).toBe(FILE.length);
  });

  it("answers 416 past the end", async () => {
    const res = await app.inject({ url: `/s/${ID}/140`, headers: { ...auth, range: `bytes=${FILE.length}-` } });
    expect(res.statusCode).toBe(416);
  });

  it("waits out the warm-up window of a fresh URL instead of failing", async () => {
    // like googlevideo: a URL refuses everything for its first moments
    refuse = (sig) => Date.now() - (issuedAt.get(sig ?? "") ?? 0) < 120;
    const warm = await makeApp({ warmupMs: 200, retryDelaysMs: [] });
    const res = await warm.inject({ url: `/s/${ID}/140`, headers: { ...auth, range: "bytes=0-99" } });
    expect(res.statusCode).toBe(206);
    expect(seen.length).toBe(1);
    expect(resolves).toBe(1);
  });

  it("rides out the first two 403s of a fresh URL without re-resolving", async () => {
    const hits = new Map<string | null, number>();
    refuse = (sig) => {
      hits.set(sig, (hits.get(sig) ?? 0) + 1);
      return hits.get(sig)! <= 2;
    };
    const res = await app.inject({ url: `/s/${ID}/136`, headers: { ...auth, range: "bytes=0-99" } });
    expect(res.statusCode).toBe(206);
    expect(resolves).toBe(1);
    expect(seen.map((s) => s.sig)).toEqual(["s1", "s1", "s1"]);
  });

  it("re-resolves after three refusals of the same URL", async () => {
    refuse = (sig) => sig === "s1";
    const res = await app.inject({ url: `/s/${ID}/136`, headers: { ...auth, range: "bytes=0-99" } });
    expect(res.statusCode).toBe(206);
    expect(resolves).toBe(2);
    expect(seen.map((s) => s.sig)).toEqual(["s1", "s1", "s1", "s2"]);
  });

  it("returns 502 with {error:'upstream'} when the fresh URLs keep refusing", async () => {
    refuse = () => true;
    const res = await app.inject({ url: `/s/${ID}/136`, headers: { ...auth, range: "bytes=0-99" } });
    expect(res.statusCode).toBe(502);
    expect(res.json()).toEqual({ error: "upstream" });
    expect(resolves).toBe(2);
    expect(seen.length).toBe(6);
  });

  it("returns 502 when googlevideo cannot be reached", async () => {
    const config = loadConfig({ API_TOKEN: TOKEN, COOKIES_FILE: "/nonexistent", LOG_LEVEL: "silent" });
    const expire = Math.floor(Date.now() / 1000) + 3600;
    const { app: dead } = await buildApp(config, {
      upstreamTiming: FAST,
      ytdlpRunner: async () =>
        JSON.stringify(fixture)
          .replace(/https:\/\/rr1---sn-test\.googlevideo\.com/g, "http://127.0.0.1:1")
          .replace(/expire=\d+/g, `expire=${expire}`),
    });
    const res = await dead.inject({ url: `/s/${ID}/140`, headers: { ...auth, range: "bytes=0-9" } });
    expect(res.statusCode).toBe(502);
    expect(res.json()).toEqual({ error: "upstream" });
  });

  it("rejects unknown itags and bad ids", async () => {
    expect((await app.inject({ url: `/s/${ID}/999`, headers: { ...auth, range: "bytes=0-1" } })).statusCode).toBe(404);
    expect((await app.inject({ url: `/s/short/140`, headers: { ...auth, range: "bytes=0-1" } })).statusCode).toBe(400);
  });
});

describe("/mpd", () => {
  it("probes sidx through the proxy path and renders all renditions", async () => {
    const app = await makeApp();
    const res = await app.inject({ url: `/mpd/${ID}?token=${TOKEN}` });
    expect(res.statusCode).toBe(200);
    expect(res.headers["content-type"]).toMatch(/application\/dash\+xml/);
    expect(res.headers["cache-control"]).toBe("no-store");
    expect(res.body).toContain('indexRange="2024-2123"');
    expect(res.body).toContain(`/s/${ID}/136?token=${TOKEN}`);
    expect([...res.body.matchAll(/Representation id="(\d+)"/g)].map((m) => m[1])).toEqual(["136", "135", "134", "140"]);
  });

  it("omits the token from BaseURLs when the header was used", async () => {
    const app = await makeApp();
    const res = await app.inject({ url: `/mpd/${ID}`, headers: auth });
    expect(res.body).not.toContain("token=");
  });
});

describe("CORS for desktop players", () => {
  it("answers preflights on /s without a token and adds headers to responses", async () => {
    const app = await makeApp();
    const pre = await app.inject({ method: "OPTIONS", url: `/s/${ID}/140` });
    expect(pre.statusCode).toBe(204);
    expect(pre.headers["access-control-allow-headers"]).toMatch(/Range/);
    const res = await app.inject({ url: `/s/${ID}/140?token=${TOKEN}`, headers: { range: "bytes=0-9" } });
    expect(res.headers["access-control-allow-origin"]).toBe("*");
    expect(res.headers["access-control-expose-headers"]).toMatch(/Content-Range/);
  });

  it("leaves other paths without CORS headers", async () => {
    const app = await makeApp();
    const res = await app.inject({ url: `/resolve/${ID}`, headers: auth });
    expect(res.headers["access-control-allow-origin"]).toBeUndefined();
  });
});

describe("parseRange", () => {
  it("handles the single-range forms", () => {
    expect(parseRange("bytes=0-99", 1000)).toEqual({ start: 0, end: 99 });
    expect(parseRange("bytes=900-", 1000)).toEqual({ start: 900, end: 999 });
    expect(parseRange("bytes=-100", 1000)).toEqual({ start: 900, end: 999 });
    expect(parseRange("bytes=0-5000", 1000)).toEqual({ start: 0, end: 999 });
    expect(parseRange(undefined, 1000)).toBeNull();
    expect(parseRange("bytes=0-1,5-6", 1000)).toBe("invalid");
    expect(parseRange("bytes=9-3", 1000)).toBe("invalid");
  });
});
