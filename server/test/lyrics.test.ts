import type { FastifyInstance } from "fastify";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { buildApp } from "../src/app.js";
import { timedLyrics } from "../src/browse.js";
import { loadConfig } from "../src/config.js";
import {
  cleanTitle,
  fromLrclib,
  type JsonGet,
  LrclibError,
  lrclibLookup,
  LyricsService,
  lyricsQueries,
  mainArtist,
  parseLrc,
  pickMatch,
} from "../src/lyrics.js";

describe("parseLrc", () => {
  it("reads [mm:ss], [mm:ss.x], [mm:ss.xx], [mm:ss.xxx] and [mm:ss:xx]", () => {
    const lrc = ["[00:01]one", "[00:02.5]two", "[00:03.25]three", "[00:04.125]four", "[01:05:50]five"].join("\n");
    expect(parseLrc(lrc)).toEqual([
      { t: 1000, text: "one" },
      { t: 2500, text: "two" },
      { t: 3250, text: "three" },
      { t: 4125, text: "four" },
      { t: 65500, text: "five" },
    ]);
  });

  it("puts a line with several stamps at each of them, in time order", () => {
    const lrc = "[00:12.00][01:30.50]chorus\n[00:20.00]verse\n[01:00.00]bridge";
    expect(parseLrc(lrc)).toEqual([
      { t: 12000, text: "chorus" },
      { t: 20000, text: "verse" },
      { t: 60000, text: "bridge" },
      { t: 90500, text: "chorus" },
    ]);
  });

  it("drops metadata tags and keeps empty lines as pauses", () => {
    const lrc = ["[ar: Someone]", "[ti:Song]", "[al:Album]", "[by:me]", "[length: 03:45]", "[#: comment]", "[00:05.00]hello", "[00:09.00]", "[00:10.00]  world  ", "untimed text"].join("\r\n");
    expect(parseLrc(lrc)).toEqual([
      { t: 5000, text: "hello" },
      { t: 9000, text: "" },
      { t: 10000, text: "world" },
    ]);
  });

  it("applies [offset:] wherever it appears (positive = sooner), never below 0", () => {
    expect(parseLrc("[00:01.00]a\n[00:10.00]b\n[offset:+1500]")).toEqual([
      { t: 0, text: "a" },
      { t: 8500, text: "b" },
    ]);
    expect(parseLrc("[offset:-250]\n[00:10.00]b")).toEqual([{ t: 10250, text: "b" }]);
    expect(parseLrc("[offset:abc]\n[00:10.00]b")).toEqual([{ t: 10000, text: "b" }]);
  });

  it("sorts out-of-order lines, keeping file order for equal stamps", () => {
    expect(parseLrc("[00:30.00]c\n[00:10.00]a\n[00:10.00]b").map((l) => l.text)).toEqual(["a", "b", "c"]);
  });

  it("strips enhanced-LRC word stamps", () => {
    expect(parseLrc("[00:01.00]<00:01.00> Hello <00:01.50> world")).toEqual([{ t: 1000, text: "Hello world" }]);
  });
});

describe("title cleanup", () => {
  it.each([
    ["Instant Crush (Official Video)", "Instant Crush"],
    ["Instant Crush (Official Music Video)", "Instant Crush"],
    ["Instant Crush [Lyrics]", "Instant Crush"],
    ["Instant Crush (Lyric Video)", "Instant Crush"],
    ["Instant Crush (Audio)", "Instant Crush"],
    ["Instant Crush (Visualizer)", "Instant Crush"],
    ["Here Comes The Sun (Remastered 2011)", "Here Comes The Sun"],
    ["Here Comes The Sun (2019 Remaster)", "Here Comes The Sun"],
    ["Here Comes The Sun - Remastered 2009", "Here Comes The Sun"],
    ["Instant Crush HD", "Instant Crush"],
    ["Instant Crush [4K] (Official Video)", "Instant Crush"],
    ["Instant Crush (feat. Julian Casablancas)", "Instant Crush"],
    ["Daft Punk - Instant Crush (Official Video) ft. Julian Casablancas", "Daft Punk - Instant Crush"],
    ["Daft Punk ft. Julian Casablancas - Instant Crush", "Daft Punk - Instant Crush"],
    ["Kuzey Rüzgârı - Gölge Şarkısı | Official Video", "Kuzey Rüzgârı - Gölge Şarkısı"],
    ["Daft Punk – Instant Crush", "Daft Punk - Instant Crush"],
    ["Instant Crush Official Video", "Instant Crush"],
  ])("%s -> %s", (raw, clean) => {
    expect(cleanTitle(raw)).toBe(clean);
  });

  it("keeps what names the song", () => {
    expect(cleanTitle("Video Games")).toBe("Video Games");
    expect(cleanTitle("Audio")).toBe("Audio");
    expect(cleanTitle("Bohemian Rhapsody (Live Aid 1985)")).toBe("Bohemian Rhapsody (Live Aid 1985)");
    expect(cleanTitle("Get Lucky (Radio Edit)")).toBe("Get Lucky (Radio Edit)");
    expect(cleanTitle("Dancing with Myself")).toBe("Dancing with Myself");
  });

  it("takes the first artist and drops channel suffixes", () => {
    expect(mainArtist("Daft Punk, Pharrell Williams")).toBe("Daft Punk");
    expect(mainArtist("Simon & Garfunkel")).toBe("Simon");
    expect(mainArtist("Ozuna x Bad Bunny")).toBe("Ozuna");
    expect(mainArtist("Kuzey Rüzgârı • Ayaz")).toBe("Kuzey Rüzgârı");
    expect(mainArtist("Daft Punk · Random Access Memories")).toBe("Daft Punk"); // the app's subtitle separator
    expect(mainArtist("Daft Punk - Topic")).toBe("Daft Punk");
    expect(mainArtist("DaftPunkVEVO")).toBe("DaftPunk");
    expect(mainArtist("Lil Nas X")).toBe("Lil Nas X");
    expect(mainArtist("AC/DC")).toBe("AC/DC");
  });

  it("asks for a clean title once when nothing needed cleaning", () => {
    expect(lyricsQueries("Instant Crush", "Daft Punk")).toEqual([{ title: "Instant Crush", artist: "Daft Punk" }]);
  });

  it("splits 'Artist - Title' when no artist is given, then retries the raw title", () => {
    expect(lyricsQueries("Daft Punk - Instant Crush (Official Video)")).toEqual([
      { title: "Instant Crush", artist: "Daft Punk" },
      { title: "Daft Punk - Instant Crush (Official Video)", artist: "" },
    ]);
  });

  it("splits when the given artist is the channel form of the title's artist", () => {
    expect(lyricsQueries("Daft Punk - Instant Crush", "DaftPunkVEVO")[0]).toEqual({ title: "Instant Crush", artist: "Daft Punk" });
    expect(lyricsQueries("Gölge Şarkısı - Kuzey Rüzgârı", "Kuzey Rüzgârı")[0]).toEqual({ title: "Gölge Şarkısı", artist: "Kuzey Rüzgârı" });
  });

  it("tries both readings when a label channel uploads 'Artist - Title'", () => {
    expect(lyricsQueries("Bülent Ersoy - Yüzünü Göremem (Official Video)", "MuzikPlay")).toEqual([
      { title: "Yüzünü Göremem", artist: "Bülent Ersoy" },
      { title: "Bülent Ersoy - Yüzünü Göremem", artist: "MuzikPlay" },
      { title: "Bülent Ersoy - Yüzünü Göremem (Official Video)", artist: "MuzikPlay" },
    ]);
  });

  it("does not split a dash that introduces a version", () => {
    expect(lyricsQueries("Hello - Live", "Adele")[0]).toEqual({ title: "Hello - Live", artist: "Adele" });
  });

  it("adds a bracket-free query and the first artist", () => {
    expect(lyricsQueries("Get Lucky (Radio Edit)", "Daft Punk, Pharrell Williams")).toEqual([
      { title: "Get Lucky (Radio Edit)", artist: "Daft Punk" },
      { title: "Get Lucky", artist: "Daft Punk" },
      { title: "Get Lucky (Radio Edit)", artist: "Daft Punk, Pharrell Williams" },
    ]);
  });
});

const entry = (duration: number, synced: string | null, plain: string | null = synced ? "plain" : null, instrumental = false) => ({
  id: duration,
  trackName: "t",
  artistName: "a",
  duration,
  instrumental,
  syncedLyrics: synced,
  plainLyrics: plain,
});

describe("pickMatch", () => {
  it("prefers synced lyrics within ±3 s, closest first", () => {
    const list = [entry(200, null, "p"), entry(300, "[00:01.00]far"), entry(242, "[00:01.00]near"), entry(239, "[00:01.00]nearer")];
    expect(pickMatch(list, 240)).toBe(list[3]);
  });

  it("then any synced lyrics, then plain lyrics", () => {
    const far = entry(300, "[00:01.00]x");
    const plain = entry(240, null, "words");
    expect(pickMatch([plain, far], 240)).toBe(far);
    expect(pickMatch([plain, entry(240, null, null)], 240)).toBe(plain);
  });

  it("takes LRCLIB's order when the duration is unknown", () => {
    const a = entry(100, "[00:01.00]a");
    const b = entry(200, "[00:01.00]b");
    expect(pickMatch([entry(150, null, "p"), a, b])).toBe(a);
  });

  it("accepts an instrumental record only within ±3 s, and nothing from junk", () => {
    const inst = entry(240, null, null, true);
    expect(pickMatch([inst], 241)).toBe(inst);
    expect(pickMatch([inst], 260)).toBeUndefined();
    expect(pickMatch([inst])).toBeUndefined();
    expect(pickMatch([null, "x", 3, { syncedLyrics: "  " }], 240)).toBeUndefined();
  });
});

describe("fromLrclib", () => {
  it("returns synced lines and the plain text", () => {
    expect(fromLrclib({ syncedLyrics: "[00:01.00]a\n[00:02.00]b\n", plainLyrics: "a\nb\n", instrumental: false })).toEqual({
      synced: [
        { t: 1000, text: "a" },
        { t: 2000, text: "b" },
      ],
      plain: "a\nb",
      instrumental: false,
      source: "lrclib",
    });
  });

  it("derives plain text from synced lines, and marks instrumental records", () => {
    expect(fromLrclib({ syncedLyrics: "[00:01.00]a\n[00:02.00]b", plainLyrics: null })!.plain).toBe("a\nb");
    expect(fromLrclib({ instrumental: true, syncedLyrics: null, plainLyrics: null })).toEqual({ synced: null, plain: null, instrumental: true, source: "lrclib" });
    expect(fromLrclib({ syncedLyrics: "[00:01.00]", plainLyrics: "" })).toBeUndefined();
  });
});

// a fake LRCLIB: answers by path, records every URL asked
function fakeLrclib(routes: { get?: (u: URL) => { status: number; body?: unknown }; search?: (u: URL) => { status: number; body?: unknown } }) {
  const urls: URL[] = [];
  const get: JsonGet = async (u) => {
    urls.push(u);
    const route = u.pathname === "/api/get" ? routes.get : routes.search;
    if (!route) throw new Error(`unexpected ${u.pathname}`);
    return route(u);
  };
  return { get, urls };
}

const SYNCED = "[00:01.00]first line\n[00:03.00]\n[00:04.50]second line";

describe("lrclibLookup", () => {
  it("uses /api/get when it matches, with whole seconds and no empty album", async () => {
    const lr = fakeLrclib({ get: () => ({ status: 200, body: entry(337, SYNCED) }) });
    const found = await lrclibLookup(lr.get, [{ title: "Instant Crush", artist: "Daft Punk" }], undefined, 337.4);
    expect(found?.synced).toHaveLength(3);
    expect(lr.urls.map((u) => u.toString())).toEqual(["https://lrclib.net/api/get?track_name=Instant+Crush&artist_name=Daft+Punk&duration=337"]);
  });

  it("searches after a 404 and picks the closest synced record", async () => {
    const lr = fakeLrclib({
      get: () => ({ status: 404, body: { name: "TrackNotFound" } }),
      search: () => ({ status: 200, body: [entry(200, "[00:01.00]other"), entry(336, SYNCED)] }),
    });
    const found = await lrclibLookup(lr.get, [{ title: "Instant Crush", artist: "Daft Punk" }], "Random Access Memories", 337);
    expect(found?.synced?.[0]).toEqual({ t: 1000, text: "first line" });
    expect(lr.urls[0]!.searchParams.get("album_name")).toBe("Random Access Memories");
    expect(lr.urls[1]!.pathname).toBe("/api/search");
    expect(lr.urls[1]!.searchParams.get("track_name")).toBe("Instant Crush");
    expect(lr.urls[1]!.searchParams.has("album_name")).toBe(false);
  });

  it("searches each query in turn and skips /api/get without an artist", async () => {
    const lr = fakeLrclib({ search: (u) => ({ status: 200, body: u.searchParams.get("track_name") === "raw title" ? [entry(100, null, "words")] : [] }) });
    const found = await lrclibLookup(lr.get, [{ title: "clean", artist: "" }, { title: "raw title", artist: "" }]);
    expect(found).toEqual({ synced: null, plain: "words", instrumental: false, source: "lrclib" });
    expect(lr.urls.map((u) => u.pathname)).toEqual(["/api/search", "/api/search"]);
    expect(lr.urls[0]!.searchParams.has("artist_name")).toBe(false);
  });

  it("answers undefined when nothing matches and throws on a broken LRCLIB", async () => {
    const empty = fakeLrclib({ get: () => ({ status: 404 }), search: () => ({ status: 200, body: [] }) });
    expect(await lrclibLookup(empty.get, [{ title: "zzz", artist: "qqq" }])).toBeUndefined();
    const broken = fakeLrclib({ get: () => ({ status: 502 }) });
    await expect(lrclibLookup(broken.get, [{ title: "a", artist: "b" }])).rejects.toBeInstanceOf(LrclibError);
    const searchBroken = fakeLrclib({ search: () => ({ status: 500 }) });
    await expect(lrclibLookup(searchBroken.get, [{ title: "a", artist: "" }])).rejects.toThrow("HTTP 500");
  });

  it("leaves out a duration LRCLIB would reject", async () => {
    const lr = fakeLrclib({ get: () => ({ status: 404 }), search: () => ({ status: 200, body: [] }) });
    await lrclibLookup(lr.get, [{ title: "a", artist: "b" }], undefined, 7200);
    expect(lr.urls[0]!.searchParams.has("duration")).toBe(false);
  });
});

// the Android YouTube Music lyrics page as it came back on 2026-10-31 (structure only)
const androidPage = (rows: object[]) => ({
  responseContext: {},
  contents: { elementRenderer: { newElement: { type: { componentType: { model: { timedLyricsModel: { lyricsData: { timedLyricsData: rows, sourceMessage: "Source: Musixmatch" } } } } } } } },
});
const row = (text: string, start?: number, end?: number) => ({
  lyricLine: text,
  ...(start === undefined ? {} : { cueRange: { startTimeMilliseconds: String(start), endTimeMilliseconds: String(end), metadata: { id: "0" } } }),
});

describe("timedLyrics", () => {
  it("reads timed lines, turning ♪ lines, gaps and the end into pauses", () => {
    const page = androidPage([row("♪", 0, 20570), row("first", 20570, 23470), row("  second ", 23470, 25000), row("third", 30000, 32000)]);
    expect(timedLyrics(page)).toEqual({
      synced: [
        { t: 0, text: "" },
        { t: 20570, text: "first" },
        { t: 23470, text: "second" },
        { t: 25000, text: "" },
        { t: 30000, text: "third" },
        { t: 32000, text: "" },
      ],
      plain: "first\nsecond\nthird",
    });
  });

  it("gives plain text for the untimed lines of a music video", () => {
    expect(timedLyrics(androidPage([row("first"), row("second")]))).toEqual({ synced: null, plain: "first\nsecond" });
  });

  it("answers undefined without lyrics", () => {
    expect(timedLyrics({ contents: { messageRenderer: {} } })).toBeUndefined();
    expect(timedLyrics(androidPage([row("♪", 0, 1000)]))).toBeUndefined();
    expect(timedLyrics(undefined)).toBeUndefined();
  });
});

const FAST = { youtubeTimeoutMs: 200, retryDelayMs: 1 };

describe("LyricsService", () => {
  const logs: Array<{ obj: object; msg: string }> = [];
  const log = { warn: (obj: object, msg: string) => logs.push({ obj, msg }) };

  const TIMED = { synced: [{ t: 0, text: "" }, { t: 1000, text: "secret words" }], plain: "secret words" };
  const lrclibHit: JsonGet = async (u) => (u.pathname === "/api/get" ? { status: 200, body: entry(100, "[00:01.00]secret words") } : { status: 200, body: [] });

  it("answers from YouTube Music first when there is an id, without asking LRCLIB", async () => {
    let lrclibAsked = 0;
    const svc = new LyricsService({ lrclib: async (u) => (lrclibAsked++, lrclibHit(u)), youtube: async () => TIMED, timing: FAST });
    expect(await svc.find({ title: "t", artist: "a", id: "dQw4w9WgXcQ" })).toEqual({ ...TIMED, instrumental: false, source: "youtube" });
    expect(lrclibAsked).toBe(0);
    const plain = new LyricsService({ lrclib: lrclibHit, youtube: async () => ({ synced: null, plain: "a\r\nb\n" }), timing: FAST });
    expect(await plain.find({ title: "t", artist: "a", id: "dQw4w9WgXcQ" })).toEqual({ synced: null, plain: "a\nb", instrumental: false, source: "youtube" });
  });

  it("asks LRCLIB when YouTube Music has no lyrics, and YouTube only with an id", async () => {
    const ids: string[] = [];
    const svc = new LyricsService({ lrclib: lrclibHit, youtube: async (id) => (ids.push(id), undefined), timing: FAST });
    expect((await svc.find({ title: "t", artist: "a", id: "dQw4w9WgXcQ" })).source).toBe("lrclib");
    expect((await svc.find({ title: "u", artist: "a" })).source).toBe("lrclib");
    expect(ids).toEqual(["dQw4w9WgXcQ"]);
  });

  it("retries a failing YouTube once, then falls through to LRCLIB with one warning and no lyrics in the log", async () => {
    logs.length = 0;
    let calls = 0;
    const svc = new LyricsService({
      lrclib: lrclibHit,
      youtube: async () => {
        calls++;
        throw new Error("fetch failed");
      },
      timing: FAST,
      log,
    });
    expect((await svc.find({ title: "t", artist: "a", id: "dQw4w9WgXcQ" })).source).toBe("lrclib");
    expect(calls).toBe(2);
    expect(logs.map((l) => l.obj)).toEqual([{ source: "youtube", err: "fetch failed" }]);
    expect(JSON.stringify(logs)).not.toContain("secret");
  });

  it("answers empty with one warning when YouTube has nothing and LRCLIB fails", async () => {
    logs.length = 0;
    const svc = new LyricsService({
      lrclib: async () => {
        throw new Error("The operation was aborted due to timeout");
      },
      youtube: async () => undefined,
      timing: FAST,
      log,
    });
    expect(await svc.find({ title: "t", artist: "a", id: "dQw4w9WgXcQ" })).toEqual({ synced: null, plain: null, instrumental: false, source: null });
    expect(logs.map((l) => l.obj)).toEqual([{ source: "lrclib", err: "The operation was aborted due to timeout" }]);
  });

  it("keeps plain YouTube lyrics only 5 min when the timed version failed", async () => {
    let now = 0;
    let calls = 0;
    const svc = new LyricsService({ lrclib: lrclibHit, youtube: async () => (calls++, { synced: null, plain: "p", partial: true }), timing: FAST, now: () => now });
    const ask = () => svc.find({ title: "t", artist: "a", id: "dQw4w9WgXcQ" });
    await ask();
    now = 4 * 60_000;
    await ask();
    expect(calls).toBe(1);
    now = 6 * 60_000;
    await ask();
    expect(calls).toBe(2);
  });

  it("caches hits for 24 h, misses for 1 h and answers past a failure for 5 min", async () => {
    let now = 0;
    let requests = 0;
    let mode: "hit" | "miss" | "fail" = "hit";
    const lrclib: JsonGet = async (u) => {
      requests++;
      if (mode === "fail") return { status: 503 };
      if (mode === "hit" && u.pathname === "/api/get") return { status: 200, body: entry(100, SYNCED) };
      return u.pathname === "/api/get" ? { status: 404 } : { status: 200, body: [] };
    };
    const svc = new LyricsService({ lrclib, youtube: async () => undefined, timing: FAST, now: () => now });
    const ask = (title: string) => svc.find({ title, artist: "Artist" });

    await ask("Hit");
    await ask(" hit "); // the same normalised query
    expect(requests).toBe(1);
    now = 23 * 3600_000;
    await ask("Hit");
    expect(requests).toBe(1);

    mode = "miss";
    await ask("Miss");
    const afterMiss = requests;
    now += 59 * 60_000;
    await ask("Miss");
    expect(requests).toBe(afterMiss);
    now += 2 * 60_000;
    await ask("Miss");
    expect(requests).toBeGreaterThan(afterMiss);

    mode = "fail";
    await ask("Fail");
    const afterFail = requests;
    now += 4 * 60_000;
    await ask("Fail");
    expect(requests).toBe(afterFail);
    now += 2 * 60_000;
    await ask("Fail");
    expect(requests).toBeGreaterThan(afterFail);
  });

  it("tries LRCLIB once more after a short Retry-After", async () => {
    const statuses = [503, 200];
    const svc = new LyricsService({
      lrclib: async (u) => {
        const status = statuses.shift() ?? 200;
        if (status === 503) return { status, retryAfterS: 0.01 };
        return u.pathname === "/api/get" ? { status: 200, body: entry(100, SYNCED) } : { status: 200, body: [] };
      },
      youtube: async () => undefined,
      timing: FAST,
    });
    expect((await svc.find({ title: "t", artist: "a" })).source).toBe("lrclib");
    expect(statuses).toEqual([]);
  });

  it("leaves LRCLIB alone for its Retry-After after a 429", async () => {
    let now = 0;
    let requests = 0;
    const svc = new LyricsService({
      lrclib: async () => (requests++, { status: 429, retryAfterS: 30 }),
      youtube: async () => undefined,
      timing: FAST,
      now: () => now,
    });
    await svc.find({ title: "one", artist: "a" });
    await svc.find({ title: "two", artist: "a" });
    expect(requests).toBe(1);
    now = 31_000;
    await svc.find({ title: "three", artist: "a" });
    expect(requests).toBe(2);
  });
});

describe("lyrics route", () => {
  let app: FastifyInstance;
  const auth = { authorization: `Bearer ${"t".repeat(64)}` };
  let client = 0;
  const get = (url: string, headers: Record<string, string> = auth) => app.inject({ url, headers, remoteAddress: `10.1.${Math.floor(++client / 250)}.${client % 250}` });
  const lrclibUrls: URL[] = [];
  const youtubeIds: string[] = [];
  let lrclibDown = false;

  beforeAll(async () => {
    const config = loadConfig({ API_TOKEN: "t".repeat(64), YTDLP_BIN: "/nonexistent", LOG_LEVEL: "silent", COOKIES_FILE: "/nonexistent/cookies.txt" });
    ({ app } = await buildApp(config, {
      lyrics: {
        timing: FAST,
        lrclib: async (u) => {
          lrclibUrls.push(u);
          if (lrclibDown) throw new Error("connect ECONNREFUSED");
          const track = u.searchParams.get("track_name");
          if (u.pathname === "/api/get") return track === "Instant Crush" ? { status: 200, body: entry(337, SYNCED) } : { status: 404 };
          return { status: 200, body: track === "Yüzünü Göremem" ? [entry(250, null, "plain words")] : [] };
        },
        youtube: async (id) => {
          youtubeIds.push(id);
          if (id === "7uLFd9VfvZE") return { synced: [{ t: 0, text: "" }, { t: 20570, text: "line one" }, { t: 23470, text: "line two" }], plain: "line one\nline two" };
          return id === "a5uQMwRMHcs" ? { synced: null, plain: "line one\nline two" } : undefined;
        },
      },
    }));
    await app.ready();
  });

  afterAll(async () => {
    await app.close();
  });

  it("keeps /lyrics behind the token and wants a title and a well-formed id", async () => {
    expect((await get("/lyrics?title=x", {})).statusCode).toBe(401);
    const noTitle = await get("/lyrics?artist=Daft%20Punk");
    expect(noTitle.statusCode).toBe(400);
    expect(noTitle.json()).toEqual({ error: "title" });
    expect((await get("/lyrics?title=%20%20")).statusCode).toBe(400);
    expect((await get("/lyrics?title=x&id=short")).json()).toEqual({ error: "id" });
  });

  it("answers LRCLIB's synced lyrics", async () => {
    const res = await get("/lyrics?title=Instant%20Crush&artist=Daft%20Punk&duration=337");
    expect(res.statusCode).toBe(200);
    expect(res.json()).toEqual({
      synced: [
        { t: 1000, text: "first line" },
        { t: 3000, text: "" },
        { t: 4500, text: "second line" },
      ],
      plain: "plain",
      instrumental: false,
      source: "lrclib",
    });
  });

  it("cleans a messy YouTube title before asking LRCLIB", async () => {
    const res = await get(`/lyrics?title=${encodeURIComponent("Bülent Ersoy - Yüzünü Göremem (Official Video)")}&artist=MuzikPlay`);
    expect(res.json()).toMatchObject({ plain: "plain words", source: "lrclib" });
  });

  it("answers YouTube Music's own lyrics for an id, before LRCLIB", async () => {
    const asked = lrclibUrls.length;
    const timed = await get("/lyrics?title=Instant%20Crush&artist=Daft%20Punk&duration=337&id=7uLFd9VfvZE");
    expect(timed.json()).toEqual({
      synced: [
        { t: 0, text: "" },
        { t: 20570, text: "line one" },
        { t: 23470, text: "line two" },
      ],
      plain: "line one\nline two",
      instrumental: false,
      source: "youtube",
    });
    const plain = await get("/lyrics?title=Instant%20Crush&artist=Daft%20Punk&id=a5uQMwRMHcs");
    expect(plain.json()).toEqual({ synced: null, plain: "line one\nline two", instrumental: false, source: "youtube" });
    expect(lrclibUrls.length).toBe(asked);
  });

  it("asks LRCLIB when YouTube Music has none, and answers empty when neither has any", async () => {
    const lr = await get("/lyrics?title=Instant%20Crush&artist=Daft%20Punk&duration=337&id=khnokW3Mw24");
    expect(lr.json()).toMatchObject({ source: "lrclib", synced: [{ t: 1000, text: "first line" }, { t: 3000, text: "" }, { t: 4500, text: "second line" }] });
    const none = await get("/lyrics?title=qwxzv%20plmok&artist=zzzz");
    expect(none.statusCode).toBe(200);
    expect(none.json()).toEqual({ synced: null, plain: null, instrumental: false, source: null });
    expect(youtubeIds).toEqual(["7uLFd9VfvZE", "a5uQMwRMHcs", "khnokW3Mw24"]);
  });

  it("answers 200 when LRCLIB is down", async () => {
    lrclibDown = true;
    try {
      const res = await get("/lyrics?title=Down%20Song&artist=Someone&id=dQw4w9WgXcQ");
      expect(res.statusCode).toBe(200);
      expect(res.json().source).toBeNull();
    } finally {
      lrclibDown = false;
    }
  });
});
