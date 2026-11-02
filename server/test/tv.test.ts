import { EventEmitter } from "node:events";
import { existsSync, mkdtempSync, readFileSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { buildApp } from "../src/app.js";
import { interleave } from "../src/browse.js";
import { loadConfig } from "../src/config.js";
import { clockSeconds, TvAccount, tvChunk, tvContinuation, tvItems, tvShorts } from "../src/tv.js";

// TV page pieces in the shapes YouTube's TV app gets (2026-10-31), trimmed to what tv.ts reads
const thumbs = (id: string) => ({ thumbnails: [{ url: `https://i.ytimg.com/vi/${id}/default.jpg`, width: 120 }, { url: `https://i.ytimg.com/vi/${id}/hqdefault.jpg`, width: 480 }] });
const line = (...texts: string[]) => ({ lineRenderer: { items: texts.map((t) => ({ lineItemRenderer: { text: { simpleText: t } } })) } });
const tile = (id: string, title: string, overlay: string, extra: Record<string, unknown> = {}) => ({
  tileRenderer: {
    contentId: id,
    contentType: "TILE_CONTENT_TYPE_VIDEO",
    header: { tileHeaderRenderer: { thumbnail: thumbs(id), thumbnailOverlays: [{ thumbnailOverlayTimeStatusRenderer: { text: { simpleText: overlay } } }] } },
    metadata: { tileMetadataRenderer: { title: { simpleText: title }, lines: [line("A Channel"), line("1.2M views", "•", "3 days ago")] } },
    onSelectCommand: { watchEndpoint: { videoId: id } },
    ...extra,
  },
});
const carousel = {
  tileRenderer: {
    contentId: "cccccccccc1",
    contentType: "TILE_CONTENT_TYPE_VIDEO",
    header: { tileHeaderRenderer: { thumbnail: thumbs("cccccccccc1"), thumbnailOverlays: [{ thumbnailOverlayTimeStatusRenderer: { text: { simpleText: "2:44" } } }] } },
    onSelectCommand: { watchEndpoint: { videoId: "cccccccccc1" } },
    onLongPressCommand: { showMenuCommand: { title: { simpleText: "Carousel song" }, subtitle: { simpleText: "Singer • @singer" } } },
  },
};
const lockup = (id: string, type: string, badge: string) => ({
  lockupViewModel: {
    contentId: id,
    contentType: type,
    contentImage: { thumbnailViewModel: { image: { sources: [{ url: `https://i.ytimg.com/vi/${id}/hqdefault.jpg`, width: 320 }] }, overlays: [{ thumbnailBottomOverlayViewModel: { badges: [{ thumbnailBadgeViewModel: { icon: { sources: [] }, text: badge } }] } }] } },
    metadata: { lockupMetadataViewModel: { title: { content: `Lockup ${id}` }, metadata: { contentMetadataViewModel: { metadataRows: [{ metadataParts: [{ text: { content: "Lofi Girl" } }] }, { metadataParts: [{ text: { content: "58M views" } }] }] } } } },
  },
});
const page = (items: unknown[], next?: string) => ({
  contents: {
    tvBrowseRenderer: {
      content: {
        tvSurfaceContentRenderer: {
          content: {
            sectionListRenderer: {
              contents: [{ shelfRenderer: { content: { horizontalListRenderer: { items, continuations: [{ nextContinuationData: { continuation: "SHELF-ONLY" } }] } } } }],
              ...(next ? { continuations: [{ nextContinuationData: { continuation: next } }] } : {}),
            },
          },
        },
      },
    },
  },
});

describe("TV pages", () => {
  it("reads videos from tiles and lockups, skipping shorts, live and channels", () => {
    const data = page([
      tile("aaaaaaaaaa1", "First video", "12:34"),
      tile("aaaaaaaaaa2", "Live now", "LIVE"),
      tile("aaaaaaaaaa3", "A short", "0:30", { onSelectCommand: { reelWatchEndpoint: { videoId: "aaaaaaaaaa3" } } }),
      { tileRenderer: { contentId: "UCxxxxxxxxxxxxxxxxxxxxxx", contentType: "TILE_CONTENT_TYPE_CHANNEL" } },
      carousel,
      lockup("bbbbbbbbbb1", "LOCKUP_CONTENT_TYPE_MUSIC", "3:05"),
      lockup("bbbbbbbbbb2", "LOCKUP_CONTENT_TYPE_SHORT", "0:20"),
      tile("aaaaaaaaaa1", "First video again", "12:34"),
    ]);
    const items = tvItems(data);
    expect(items.map((i) => i.id)).toEqual(["aaaaaaaaaa1", "cccccccccc1", "bbbbbbbbbb1"]);
    expect(items[0]).toMatchObject({ kind: "video", title: "First video", subtitle: "A Channel · 1.2M views · 3 days ago", durationSec: 754 });
    expect(items[0]!.thumbUrl).toContain("hqdefault");
    expect(items[1]).toMatchObject({ title: "Carousel song", subtitle: "Singer", durationSec: 164 });
    expect(items[2]).toMatchObject({ title: "Lockup bbbbbbbbbb1", subtitle: "Lofi Girl · 58M views", durationSec: 185 });
  });

  it("reads playlists and pages with the list's own continuation, not a shelf's", async () => {
    const playlists = page([
      { tileRenderer: { contentId: "PLabc", contentType: "TILE_CONTENT_TYPE_PLAYLIST", onSelectCommand: { browseEndpoint: { browseId: "VLPLabc" } }, metadata: { tileMetadataRenderer: { title: { simpleText: "Road trip" }, lines: [line("12 videos")] } } } },
    ]);
    expect(tvItems(playlists)).toEqual([expect.objectContaining({ kind: "playlist", id: "PLabc", title: "Road trip", subtitle: "12 videos" })]);

    const first = page([tile("aaaaaaaaaa1", "One", "1:00")], "PAGE-2");
    expect(tvContinuation(first)).toBe("PAGE-2");
    expect(tvContinuation(page([tile("aaaaaaaaaa1", "One", "1:00")]))).toBeUndefined();
    const asked: unknown[] = [];
    const chunk = tvChunk(async (args) => {
      asked.push(args);
      return { continuationContents: { sectionListContinuation: { contents: [{ shelfRenderer: { content: { horizontalListRenderer: { items: [tile("aaaaaaaaaa2", "Two", "2:00")] } } } }] } } };
    }, first);
    const next = await chunk.more!();
    expect(asked).toEqual([{ continuation: "PAGE-2" }]);
    expect(next.items.map((i) => i?.id)).toEqual(["aaaaaaaaaa2"]);
    expect(next.more).toBeUndefined();
  });

  it("reads clock durations and mixes topic pages", () => {
    expect(clockSeconds("1:02:03")).toBe(3723);
    expect(clockSeconds("LIVE")).toBeUndefined();
    expect(interleave([[1, 2, 3], [10], [20, 21]])).toEqual([1, 10, 20, 2, 21, 3]);
  });
});

// a stand-in for youtubei.js: Google's device flow without Google
class FakeSession extends EventEmitter {
  oauth = { oauth2_tokens: { access_token: "access", refresh_token: "refresh", expiry_date: "2099-01-01T00:00:00Z" }, client_id: { client_id: "cid", client_secret: "secret" } };
  signedOut = false;
  async signIn(credentials?: unknown): Promise<void> {
    if (credentials) {
      this.emit("auth", { credentials });
      return;
    }
    setImmediate(() => this.emit("auth-pending", { verification_url: "https://www.google.com/device", user_code: "ABC-DEF-GHI", expires_in: 1800 }));
    return new Promise((ok) => this.once("auth", () => ok()));
  }
  async signOut() {
    this.signedOut = true;
    return {};
  }
}

function fakeYoutube(pages: Record<string, unknown>) {
  const sessions: FakeSession[] = [];
  const create = async () => {
    const session = new FakeSession();
    sessions.push(session);
    return {
      session,
      account: { getInfo: async () => ({ contents: { contents: [{ account_name: "Alex", channel_handle: "@alex" }] } }) },
      actions: { execute: async (_path: string, args: { browseId?: string; client?: string }) => ({ data: pages[args.browseId ?? ""] ?? {} }) },
    } as never;
  };
  return { create, sessions };
}

const apps: Array<{ close(): Promise<unknown> }> = [];
afterEach(async () => {
  await Promise.all(apps.splice(0).map((a) => a.close()));
});

describe("sign in with a code", () => {
  it("shows a code, keeps the approved sign-in on disk and serves the account feeds with it", async () => {
    const dir = mkdtempSync(join(tmpdir(), "rondo-tv-"));
    const tokenFile = join(dir, "secrets", "tv-oauth.json");
    const youtube = fakeYoutube({
      FEsubscriptions: page([tile("ssssssssss1", "From a subscription", "8:00")]),
      FEplaylist_aggregation: page([
        { tileRenderer: { contentId: "PLown", contentType: "TILE_CONTENT_TYPE_PLAYLIST", metadata: { tileMetadataRenderer: { title: { simpleText: "Mine" }, lines: [] } } } },
        { tileRenderer: { contentId: "WL", contentType: "TILE_CONTENT_TYPE_PLAYLIST", metadata: { tileMetadataRenderer: { title: { simpleText: "Watch later" }, lines: [] } } } },
      ]),
    });
    const config = loadConfig({ API_TOKEN: "e".repeat(64), YTDLP_BIN: "/nonexistent", LOG_LEVEL: "silent", COOKIES_FILE: join(dir, "cookies.txt"), CONFIG_FILE: join(dir, "config.json"), TV_TOKENS_FILE: tokenFile });
    const quiet = { info: () => undefined, warn: () => undefined };
    const tv = new TvAccount(tokenFile, quiet, youtube.create);
    const { app } = await buildApp(config, { newCode: () => "482913", tv });
    apps.push(app);
    const cookie = String((await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: "correct horse battery" } })).headers["set-cookie"]).split(";")[0]!;
    const auth = { authorization: `Bearer ${"e".repeat(64)}` };

    // no account yet: the Rondo hides the account tabs, and those feeds say so
    expect((await app.inject("/healthz")).json().account).toEqual({ music: false, videos: false });
    expect((await app.inject({ url: "/video/subscriptions", headers: auth })).statusCode).toBe(503);

    const code = await app.inject({ method: "POST", url: "/admin/api/tv", headers: { cookie } });
    expect(code.json()).toMatchObject({ url: "https://www.google.com/device", code: "ABC-DEF-GHI" });
    expect((await app.inject({ url: "/admin/api/status", headers: { cookie } })).json().tv).toMatchObject({ signedIn: false, pending: { code: "ABC-DEF-GHI" } });

    youtube.sessions.at(-1)!.emit("auth", {}); // approved at google.com/device
    await new Promise((ok) => setTimeout(ok, 20));
    const status = (await app.inject({ url: "/admin/api/status", headers: { cookie } })).json().tv;
    expect(status).toMatchObject({ signedIn: true, pending: null, account: { name: "Alex", handle: "@alex" } });
    expect(statSync(tokenFile).mode & 0o777).toBe(0o600);
    expect(JSON.parse(readFileSync(tokenFile, "utf8"))).toMatchObject({ refresh_token: "refresh", client: { client_id: "cid" } });
    expect((await app.inject("/healthz")).json().account).toEqual({ music: true, videos: true });

    const subs = (await app.inject({ url: "/video/subscriptions", headers: auth })).json();
    expect(subs.items).toEqual([expect.objectContaining({ kind: "video", id: "ssssssssss1", title: "From a subscription", durationSec: 480 })]);
    expect(subs.items[0].thumb).toMatch(/^\/thumb\?u=/);
    const library = (await app.inject({ url: "/music/library", headers: auth })).json();
    expect(library.playlists.map((p: { id: string }) => p.id)).toEqual(["LM", "PLown"]);

    // a restart signs in from the saved tokens
    const again = new TvAccount(tokenFile, quiet, youtube.create);
    await again.load();
    expect(again.signedIn).toBe(true);

    await app.inject({ method: "DELETE", url: "/admin/api/tv", headers: { cookie } });
    expect(existsSync(tokenFile)).toBe(false);
    expect(youtube.sessions[0]!.signedOut).toBe(true); // revoked at Google too
    expect((await app.inject("/healthz")).json().account).toEqual({ music: false, videos: false });
  });

  it("gives back a sign-in approved after the code was canceled", async () => {
    const dir = mkdtempSync(join(tmpdir(), "rondo-tv-"));
    const youtube = fakeYoutube({});
    const tv = new TvAccount(join(dir, "tv.json"), { info: () => undefined, warn: () => undefined }, youtube.create);
    await tv.begin();
    tv.cancel();
    youtube.sessions[0]!.emit("auth", {});
    await new Promise((ok) => setTimeout(ok, 10));
    expect(tv.signedIn).toBe(false);
    expect(youtube.sessions[0]!.signedOut).toBe(true);
    expect(existsSync(join(dir, "tv.json"))).toBe(false);
  });
});

describe("tvShorts", () => {
  const short = (id: string, title?: string) => ({
    tileRenderer: {
      contentType: "TILE_CONTENT_TYPE_VIDEO",
      onSelectCommand: { reelWatchEndpoint: { videoId: id } },
      header: { tileHeaderRenderer: { thumbnail: { thumbnails: [{ url: `https://i.ytimg.com/vi/${id}/oardefault.jpg`, width: 405 }] } } },
      ...(title ? { metadata: { tileMetadataRenderer: { title: { simpleText: title } } } } : {}),
    },
  });
  it("finds the Shorts shelf's tiles, which tvItems leaves out", () => {
    const page = { contents: [{ shelfRenderer: { content: { horizontalListRenderer: { items: [short("LolNRjKG8Xs", "Fötr şapka"), short("ej3m_-QUnIQ"), short("LolNRjKG8Xs")] } } } }] };
    expect(tvShorts(page)).toEqual([
      { kind: "video", id: "LolNRjKG8Xs", title: "Fötr şapka", subtitle: "", thumbUrl: "https://i.ytimg.com/vi/LolNRjKG8Xs/oardefault.jpg" },
      { kind: "video", id: "ej3m_-QUnIQ", title: "", subtitle: "", thumbUrl: "https://i.ytimg.com/vi/ej3m_-QUnIQ/oardefault.jpg" },
    ]);
    expect(tvItems(page)).toEqual([]);
  });
});
