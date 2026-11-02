import type { FastifyInstance } from "fastify";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { buildApp } from "../src/app.js";
import {
  cookiesToHeader,
  feedChunk,
  feedVideo,
  fillAlbumRows,
  headerInfo,
  historyChunk,
  homeChunk,
  homePage,
  lockupItem,
  mixChunk,
  musicItem,
  musicSearchChunk,
  panelItem,
  pickThumb,
  playlistChunk,
  playlistPage,
  relatedChunk,
  findShorts,
  sequenceChunk,
  shortItem,
  shelfSection,
  thumbLink,
  trackItem,
  videoItem,
  videoSearchChunk,
} from "../src/browse.js";
import { loadConfig } from "../src/config.js";

describe("cookiesToHeader", () => {
  it("keeps unexpired youtube.com cookies, including HttpOnly lines", () => {
    const txt = [
      "# Netscape HTTP Cookie File",
      ".youtube.com\tTRUE\t/\tTRUE\t1893456000\tSID\tabc",
      "#HttpOnly_.youtube.com\tTRUE\t/\tTRUE\t1893456000\t__Secure-3PSID\tdef",
      ".youtube.com\tTRUE\t/\tTRUE\t1000\tOLD\texpired",
      ".google.com\tTRUE\t/\tTRUE\t1893456000\tNID\tnot-youtube",
      "music.youtube.com\tFALSE\t/\tTRUE\t0\tSESSION\tghi",
      "",
    ].join("\n");
    expect(cookiesToHeader(txt, 1_800_000_000)).toBe("SID=abc; __Secure-3PSID=def; SESSION=ghi");
  });
});

describe("item reduction", () => {
  it("reduces a music search song to the app's fields", () => {
    const song = {
      type: "MusicResponsiveListItem",
      item_type: "song",
      id: "Rgrt_8mXrK8",
      title: "Get Lucky (Radio Edit)",
      artists: [{ name: "Daft Punk" }, { name: "Pharrell Williams" }],
      album: { name: "Random Access Memories" },
      duration: { text: "4:09", seconds: 249 },
      thumbnails: [
        { url: "https://yt3.googleusercontent.com/x=w60-h60", width: 60 },
        { url: "https://yt3.googleusercontent.com/x=w120-h120", width: 120 },
      ],
    };
    expect(musicItem(song)).toEqual({
      kind: "song",
      id: "Rgrt_8mXrK8",
      title: "Get Lucky (Radio Edit)",
      subtitle: "Daft Punk, Pharrell Williams · Random Access Memories",
      durationSec: 249,
      thumb: thumbLink("https://yt3.googleusercontent.com/x=w120-h120"),
    });
  });

  it("drops items it cannot identify", () => {
    expect(musicItem({ item_type: "episode", id: "x" })).toBeUndefined();
    expect(musicItem({ item_type: "song" })).toBeUndefined();
  });

  it("reduces a video search result, treating live (0 s) as no duration", () => {
    const v = videoItem({
      type: "Video",
      id: "rFZHOHl-L8A",
      title: { text: "lofi hip hop radio" },
      author: { name: "Lofi Girl" },
      duration: { text: "", seconds: 0 },
      thumbnails: [{ url: "https://i.ytimg.com/vi/rFZHOHl-L8A/hq720.jpg", width: 720 }],
    });
    expect(v).toMatchObject({ kind: "video", id: "rFZHOHl-L8A", title: "lofi hip hop radio", subtitle: "Lofi Girl" });
    expect(v!.durationSec).toBeUndefined();
    expect(v!.thumb).toBe("/thumb?u=https%3A%2F%2Fi.ytimg.com%2Fvi%2FrFZHOHl-L8A%2Fhq720.jpg");
  });
});

// Node shapes below copy what youtubei.js 18.3 produced live on 2026-10-30 (structure only).
const thumbs = (w: number[]) => w.map((width) => ({ url: `https://i.ytimg.com/vi/x/${width}.jpg`, width }));

describe("thumbLink", () => {
  it("links only hosts /thumb serves", () => {
    expect(thumbLink("https://www.gstatic.com/youtube/media/ytm/images/pbg/liked-music-@576.png")).toMatch(/^\/thumb\?u=https%3A%2F%2Fwww\.gstatic\.com/);
    expect(thumbLink("//i.ytimg.com/vi/x/1.jpg")).toBe("/thumb?u=https%3A%2F%2Fi.ytimg.com%2Fvi%2Fx%2F1.jpg");
    expect(thumbLink("https://example.com/a.jpg")).toBeUndefined();
    expect(thumbLink("http://i.ytimg.com/vi/x/1.jpg")).toBeUndefined();
    expect(thumbLink("not a url")).toBeUndefined();
  });
});

describe("YouTube Music reducers", () => {
  it("strips the VL prefix from playlist cards and keeps their subtitle", () => {
    const card = {
      type: "MusicTwoRowItem",
      item_type: "playlist",
      id: "VLRDCLAK5uy_l5Vk-JYSb0gvaOJREhK5h6kEVVe650Nks",
      title: { text: "Ultimate Halloween Hits" },
      subtitle: { text: "The Rolling Stones, Lady Gaga, Muse" },
      thumbnail: thumbs([544, 226]),
    };
    expect(musicItem(card)).toEqual({
      kind: "playlist",
      id: "RDCLAK5uy_l5Vk-JYSb0gvaOJREhK5h6kEVVe650Nks",
      title: "Ultimate Halloween Hits",
      subtitle: "The Rolling Stones, Lady Gaga, Muse",
      durationSec: undefined,
      thumb: thumbLink("https://i.ytimg.com/vi/x/544.jpg"), // the largest
    });
    expect(musicItem({ ...card, id: "VLLM", title: { text: "Liked Music" } })!.id).toBe("LM");
  });

  it("uses the single author of video cards and album/playlist results", () => {
    const video = { type: "MusicTwoRowItem", item_type: "video", id: "Hg0RPrLQGQk", title: { text: "Song" }, subtitle: { text: "Singer • 31M views" }, author: { name: "Singer" }, thumbnail: thumbs([800, 400]) };
    expect(musicItem(video)).toMatchObject({ kind: "video", subtitle: "Singer" });
    const album = { type: "MusicResponsiveListItem", item_type: "album", id: "MPREb_K8qWMWVqXGi", title: "Random Access Memories", author: { name: "Daft Punk" }, year: "2013" };
    expect(musicItem(album)).toMatchObject({ kind: "album", id: "MPREb_K8qWMWVqXGi", subtitle: "Daft Punk · 2013" });
    const playlist = { type: "MusicResponsiveListItem", item_type: "playlist", id: "VLPLzoFBKNMTkx41CNWpg-hMNzlApL3ddM3q", title: "Mix", author: { name: "Omar" } };
    expect(musicItem(playlist)).toMatchObject({ id: "PLzoFBKNMTkx41CNWpg-hMNzlApL3ddM3q", subtitle: "Omar" });
  });

  it("falls back to the card subtitle, with app separators, when no artist is listed", () => {
    const single = { type: "MusicTwoRowItem", item_type: "album", id: "MPREb_X1DQ1j0PPrX", title: { text: "GLBTM" }, subtitle: { text: "Single • 2023" }, artists: [], year: "2023" };
    expect(musicItem(single)!.subtitle).toBe("Single · 2023");
    const artist = { type: "MusicResponsiveListItem", item_type: "artist", id: "UCRr1xG_2WIDs18a6cIiCxeA", name: "Daft Punk", subtitle: { text: "Artist • 68.3M monthly audience" } };
    expect(musicItem(artist)).toMatchObject({ kind: "artist", title: "Daft Punk", subtitle: "Artist · 68.3M monthly audience" });
    expect(musicItem({ item_type: "endpoint", title: { text: "New playlist" } })).toBeUndefined();
  });

  it("reduces queue rows, unwrapping PlaylistPanelVideoWrapper", () => {
    const row = {
      type: "PlaylistPanelVideo",
      video_id: "Kr4EQDVETuA",
      title: { text: "Billie Jean" },
      author: "Michael Jackson",
      artists: [{ name: "Michael Jackson" }],
      album: { id: "MPREb_dqWTncCjkSp", name: "Thriller" },
      duration: { text: "4:55", seconds: 295 },
      thumbnail: thumbs([60, 120, 226]),
    };
    const expected = { kind: "song", id: "Kr4EQDVETuA", title: "Billie Jean", subtitle: "Michael Jackson · Thriller", durationSec: 295, thumb: thumbLink("https://i.ytimg.com/vi/x/226.jpg") };
    expect(panelItem(row)).toEqual(expected);
    expect(trackItem({ type: "PlaylistPanelVideoWrapper", primary: row })).toEqual(expected);
    expect(panelItem({ ...row, video_id: undefined, endpoint: undefined })).toBeUndefined();
  });

  it("turns playlist/album rows into songs and gives artless rows the cover", () => {
    const row = { type: "MusicResponsiveListItem", item_type: "video", id: "IluRBvnYMoY", title: "Give Life Back to Music", authors: [{ name: "Daft Punk" }], duration: { seconds: 276 }, thumbnails: [] };
    expect(trackItem(row, "/thumb?u=cover")).toEqual({ kind: "song", id: "IluRBvnYMoY", title: "Give Life Back to Music", subtitle: "Daft Punk", durationSec: 276, thumb: "/thumb?u=cover" });
    expect(trackItem({ ...row, item_type: "album", id: "MPREb_x" })).toBeUndefined();
    expect(trackItem({ ...row, item_type: "episode" })).toBeUndefined();
  });

  it("builds sections from carousel and list shelves, skipping empty ones", () => {
    const carousel = { type: "MusicCarouselShelf", header: { title: { text: "Quick picks" } }, contents: [{ item_type: "song", id: "kOn-HdEg6AQ", title: "Beat It", artists: [{ name: "Michael Jackson" }], album: { name: "Thriller" } }] };
    expect(shelfSection(carousel)).toMatchObject({ title: "Quick picks", items: [{ kind: "song", id: "kOn-HdEg6AQ", subtitle: "Michael Jackson · Thriller" }] });
    const list = { type: "MusicShelf", title: { text: "Top songs" }, contents: [{ item_type: "song", id: "khnokW3Mw24", title: "Instant Crush" }] };
    expect(shelfSection(list)!.title).toBe("Top songs");
    expect(shelfSection({ type: "MusicDescriptionShelf", header: { title: { text: "About" } } })).toBeUndefined();
    const many = { header: { title: { text: "x" } }, contents: Array.from({ length: 30 }, (_, i) => ({ item_type: "song", id: `id${i}`, title: "t" })) };
    expect(shelfSection(many)!.items).toHaveLength(20);
  });

  it("reads album/playlist headers, treating youtubei.js 'N/A' as empty", () => {
    const album = {
      type: "MusicResponsiveHeader",
      title: { text: "Random Access Memories" },
      strapline_text_one: { text: "Daft Punk" },
      subtitle: { text: "Album • 2013" },
      second_subtitle: { text: "13 songs • 1 hour, 14 minutes" },
      thumbnail: { contents: thumbs([544, 226, 120, 60]) },
    };
    expect(headerInfo(album)).toEqual({ title: "Random Access Memories", subtitle: "Daft Punk · Album · 2013 · 13 songs", thumb: thumbLink("https://i.ytimg.com/vi/x/544.jpg") });
    const playlist = { ...album, title: { text: "Essentials" }, strapline_text_one: { toString: () => "N/A" }, subtitle: { text: "Playlist • 2026" }, second_subtitle: { text: "3.1M views • 30 tracks • 2 hours" } };
    expect(headerInfo(playlist).subtitle).toBe("Playlist · 2026 · 30 tracks");
    expect(headerInfo({ type: "MusicEditablePlaylistDetailHeader", header: album }).title).toBe("Random Access Memories");
    expect(headerInfo(undefined)).toEqual({ title: "", subtitle: "", thumb: undefined });
  });
});

describe("YouTube feed reducers", () => {
  const lockup = {
    type: "LockupView",
    content_id: "5_Fayfx2NUM",
    content_type: "VIDEO",
    duration: { text: "14:54", seconds: 894 },
    content_image: { type: "ThumbnailView", image: thumbs([720, 360]) },
    metadata: {
      type: "LockupMetadataView",
      title: { text: "How I Survived Late-Stage Cancer" },
      metadata: {
        type: "ContentMetadataView",
        metadata_rows: [
          { metadata_parts: [{ text: { text: "Kurzgesagt" } }] },
          { metadata_parts: [{ text: { text: "5.6M views" } }, { text: { text: "2 days ago" } }] },
        ],
      },
    },
  };

  it("reduces a LockupView to channel and views", () => {
    expect(lockupItem(lockup)).toEqual({
      kind: "video",
      id: "5_Fayfx2NUM",
      title: "How I Survived Late-Stage Cancer",
      subtitle: "Kurzgesagt · 5.6M views",
      durationSec: 894,
      thumb: thumbLink("https://i.ytimg.com/vi/x/720.jpg"),
    });
  });

  it("adds 'views' to the bare counts of related videos", () => {
    const related = { ...lockup, metadata: { ...lockup.metadata, metadata: { metadata_rows: [{ metadata_parts: [{ text: { text: "George Michael" } }] }, { metadata_parts: [{ text: { text: "1.6B" } }, { text: { text: "17y ago" } }] }] } } };
    expect(lockupItem(related)!.subtitle).toBe("George Michael · 1.6B views");
  });

  it("drops promos, live lockups, playlists and shorts", () => {
    expect(feedVideo({ ...lockup, content_id: "sb-acq-c-projectemu-wn-NDc" })).toBeUndefined();
    expect(feedVideo({ ...lockup, duration: { text: "", seconds: 0 } })).toBeUndefined();
    expect(feedVideo({ ...lockup, content_type: "PLAYLIST" })).toBeUndefined();
    expect(feedVideo({ type: "ShortsLockupView", entity_id: "x" })).toBeUndefined();
    expect(feedVideo({ type: "ReelShelf", items: [] })).toBeUndefined();
    expect(feedVideo(lockup)!.id).toBe("5_Fayfx2NUM");
  });

  it("keeps regular Video nodes but not live or upcoming ones", () => {
    const v = { type: "Video", id: "wfTISOEGVqQ", title: { text: "Chicken" }, author: { name: "Nick" }, short_view_count: { text: "128M views" }, duration: { seconds: 30 }, thumbnails: thumbs([336, 168]) };
    expect(feedVideo(v)).toMatchObject({ kind: "video", id: "wfTISOEGVqQ", subtitle: "Nick · 128M views", durationSec: 30 });
    expect(feedVideo({ ...v, is_live: true })).toBeUndefined();
    expect(feedVideo({ ...v, is_upcoming: true, duration: { seconds: 0 } })).toBeUndefined();
  });
});

describe("pickThumb", () => {
  it("takes the largest thumbnail, whatever the order", () => {
    expect(pickThumb(thumbs([60, 544, 120]))).toBe("https://i.ytimg.com/vi/x/544.jpg");
    expect(pickThumb({ contents: thumbs([120, 60]) })).toBe("https://i.ytimg.com/vi/x/120.jpg");
    expect(pickThumb([{ url: "https://i.ytimg.com/a.jpg" }])).toBe("https://i.ytimg.com/a.jpg");
    expect(pickThumb([])).toBeUndefined();
    expect(pickThumb(undefined)).toBeUndefined();
  });
});

// youtubei.js page objects as the chunk reducers see them (structure from the 2026-10-30 probe)
describe("continuation chunks", () => {
  const song = (id: string) => ({ type: "MusicResponsiveListItem", item_type: "song", id, title: id });
  const lockup = (id: string, seconds = 60) => ({ type: "LockupView", content_id: id, content_type: "VIDEO", duration: { seconds }, metadata: { title: { text: id } } });
  const page = (fields: object, next?: object) => ({ ...fields, has_continuation: !!next, getContinuation: async () => next });

  it("reads music search shelves on page one and MusicShelfContinuation after it", async () => {
    const p2 = page({ contents: { type: "musicShelfContinuation", contents: [song("bbbbbbbbbbb")] } });
    const p1 = page({ contents: [{ type: "ItemSection", contents: [] }, { type: "MusicShelf", contents: [song("aaaaaaaaaaa"), { item_type: "episode", id: "x" }] }] }, p2);
    const c1 = musicSearchChunk(p1);
    expect(c1.items.filter(Boolean).map((i) => i!.id)).toEqual(["aaaaaaaaaaa"]);
    const c2 = await c1.more!();
    expect(c2.items.map((i) => i!.id)).toEqual(["bbbbbbbbbbb"]);
    expect(c2.more).toBeUndefined();
  });

  it("applies the feed filters to continuation pages too", async () => {
    const p2 = page({ videos: [lockup("ccccccccccc"), { type: "ShortsLockupView" }, lockup("ddddddddddd", 0)] });
    const c1 = feedChunk(page({ videos: [lockup("aaaaaaaaaaa"), { ...lockup("sb-acq-c-promo"), content_id: "sb-acq-c-promo" }] }, p2));
    expect(c1.items.filter(Boolean).map((i) => i!.id)).toEqual(["aaaaaaaaaaa"]);
    expect((await c1.more!()).items.filter(Boolean).map((i) => i!.id)).toEqual(["ccccccccccc"]);
  });

  it("reads history day sections, skipping shorts shelves", async () => {
    const c = historyChunk(page({ sections: [{ contents: [lockup("aaaaaaaaaaa"), { type: "ReelShelf", items: [] }] }, { contents: [lockup("bbbbbbbbbbb")] }] }));
    expect(c.items.filter(Boolean).map((i) => i!.id)).toEqual(["aaaaaaaaaaa", "bbbbbbbbbbb"]);
  });

  it("keeps only playable search videos", () => {
    const v = { type: "Video", id: "rFZHOHl-L8A", title: { text: "lofi" }, author: { name: "Lofi Girl" }, duration: { seconds: 60 } };
    const c = videoSearchChunk(page({ videos: [v, { type: "Video", id: "short" }] }));
    expect(c.items.filter(Boolean).map((i) => i!.id)).toEqual(["rFZHOHl-L8A"]);
  });

  it("reads home shelves and playlist rows", async () => {
    const shelf = { header: { title: { text: "Quick picks" } }, contents: [song("aaaaaaaaaaa")] };
    const home = homeChunk(page({ sections: [shelf, { type: "MusicTastebuilderShelf" }] }, page({ sections: [shelf] })));
    expect(home.items.filter(Boolean).map((s) => s!.title)).toEqual(["Quick picks"]);
    expect((await home.more!()).items).toHaveLength(1);
    const rows = playlistChunk(page({ contents: [song("aaaaaaaaaaa"), { type: "ContinuationItem" }] }, page({ contents: [song("bbbbbbbbbbb")] })));
    expect(rows.items.map((i) => i!.kind)).toEqual(["song"]);
    expect((await rows.more!()).items.map((i) => i!.id)).toEqual(["bbbbbbbbbbb"]);
  });

  it("follows the related column's ContinuationItem through appended items", async () => {
    const calls: unknown[] = [];
    const actions = { name: "actions" };
    const endpoint = {
      call: async (a: unknown, args: unknown) => {
        calls.push([a, args]);
        return { on_response_received_endpoints: [{ type: "AppendContinuationItemsAction", contents: [lockup("ccccccccccc")] }] };
      },
    };
    const c1 = relatedChunk(actions, [lockup("aaaaaaaaaaa"), { type: "ContinuationItem", endpoint }]);
    expect(c1.items.filter(Boolean).map((i) => i!.id)).toEqual(["aaaaaaaaaaa"]);
    const c2 = await c1.more!();
    expect(calls).toEqual([[actions, { parse: true }]]);
    expect(c2.items.map((i) => i!.id)).toEqual(["ccccccccccc"]);
    expect(c2.more).toBeUndefined();
  });

  it("pages a radio through PlaylistPanelContinuation, ending at an empty continuation", async () => {
    const row = (id: string) => ({ type: "PlaylistPanelVideo", video_id: id, title: { text: id }, duration: { seconds: 200 } });
    const calls: unknown[] = [];
    const responses = [
      { continuation_contents: { type: "playlistPanelContinuation", contents: [row("bbbbbbbbbbb")], continuation: "c2" } },
      { continuation_contents: { type: "playlistPanelContinuation", contents: [], continuation: "c3" } },
    ];
    const actions = { execute: async (endpoint: string, args: Record<string, unknown>) => (calls.push([endpoint, args]), responses.shift()) };
    const c1 = mixChunk(actions, "RDTMAK5uy_x", { type: "PlaylistPanel", contents: [row("aaaaaaaaaaa")], continuation: "c1" });
    expect(c1.items.map((i) => i!.id)).toEqual(["aaaaaaaaaaa"]);
    const c2 = await c1.more!();
    expect(calls[0]).toEqual(["/next", { playlistId: "RDTMAK5uy_x", continuation: "c1", client: "YTMUSIC", parse: true }]);
    expect(c2.items.map((i) => i!.id)).toEqual(["bbbbbbbbbbb"]);
    const c3 = await c2.more!();
    expect(c3.items).toEqual([]);
    expect(c3.more).toBeUndefined(); // YouTube would hand out empty pages forever
    expect(mixChunk(actions, "RDx", { contents: [row("aaaaaaaaaaa")] }).more).toBeUndefined();
  });

  it("shapes continuation pages of home and playlists", () => {
    expect(homePage({ items: [], next: "t" })).toEqual({ sections: [], next: "t" });
    expect(playlistPage({ items: [] })).toEqual({ title: "", subtitle: "", items: [] });
  });
});

describe("browse routes", () => {
  let app: FastifyInstance;
  const auth = { authorization: `Bearer ${"t".repeat(64)}` };
  // a client address per request, so the 10 req/s rate limit does not trip
  let client = 0;
  const get = (url: string, headers: Record<string, string> = auth) => app.inject({ url, headers, remoteAddress: `10.0.${Math.floor(++client / 250)}.${client % 250}` });

  beforeAll(async () => {
    const config = loadConfig({ API_TOKEN: "t".repeat(64), YTDLP_BIN: "/nonexistent", LOG_LEVEL: "silent", COOKIES_FILE: "/nonexistent/cookies.txt" });
    ({ app } = await buildApp(config));
    await app.ready();
  });

  afterAll(async () => {
    await app.close();
  });

  it.each([
    "/music/next/short",
    "/music/next/dQw4w9WgXcQ!",
    "/video/related/abc",
    "/music/album/UCRr1xG_2WIDs18a6cIiCxeA",
    "/music/artist/MPREb_K8qWMWVqXGi",
    "/music/playlist/a",
    "/music/playlist/PL%20x",
  ])("rejects a bad id: %s", async (url) => {
    const res = await get(url);
    expect(res.statusCode).toBe(400);
    expect(res.json()).toEqual({ error: "id" });
  });

  it("answers account feeds with no_cookies when there is no cookies.txt", async () => {
    for (const url of ["/music/library", "/video/subscriptions", "/video/history"]) {
      const res = await get(url);
      expect(res.statusCode).toBe(503);
    }
  });

  it("keeps the browse routes behind the token", async () => {
    expect((await get("/music/home", {})).statusCode).toBe(401);
  });

  it.each([
    "/music/search?next=unknown",
    "/music/search?q=daft%20punk&type=song&next=unknown",
    "/video/search?q=lofi&next=unknown",
    "/music/home?next=unknown",
    "/music/playlist/LM?next=unknown",
    "/video/home?next=unknown",
    "/video/related/dQw4w9WgXcQ?next=unknown",
  ])("answers an unknown or expired next token with 410: %s", async (url) => {
    const res = await get(url);
    expect(res.statusCode).toBe(410);
    expect(res.json()).toEqual({ error: "expired" });
  });

  it("checks cookies before a next token on account feeds", async () => {
    const res = await get("/video/history?next=unknown");
    expect(res.statusCode).toBe(503);
  });

  it("serves thumbnails only from Google's image hosts", async () => {
    const bad = await get(`/thumb?u=${encodeURIComponent("https://example.com/a.jpg")}&w=78`);
    expect(bad.statusCode).toBe(400);
    expect(bad.json()).toEqual({ error: "host" });
    expect((await get("/thumb?u=nope")).statusCode).toBe(400);
  });
});

describe("fillAlbumRows", () => {
  const row = (id: string, subtitle = "") => ({ kind: "song" as const, id, title: "Track " + id, subtitle });
  it("names the album's artist and the album on rows that have neither", () => {
    expect(fillAlbumRows([row("aaaaaaaaaaa")], "Glass Harbor", "Northbound")[0]?.subtitle).toBe("Glass Harbor · Northbound");
  });
  it("keeps the artists a row names itself", () => {
    expect(fillAlbumRows([row("bbbbbbbbbbb", "Glass Harbor, Guest")], "Glass Harbor", "Northbound")[0]?.subtitle).toBe("Glass Harbor, Guest");
  });
  it("leaves rows alone when the header names nothing", () => {
    expect(fillAlbumRows([row("ccccccccccc")], "", "")[0]?.subtitle).toBe("");
  });
});

describe("shorts", () => {
  const lockup = (id: string, title: string, views: string) => ({
    type: "ShortsLockupView",
    on_tap_endpoint: { payload: { videoId: id } },
    overlay_metadata: { primary_text: { text: title }, secondary_text: { text: views } },
    thumbnail: [{ url: "https://i.ytimg.com/vi/" + id + "/oar2.jpg", width: 405 }],
  });
  it("reads a short from a search shelf with its title and views", () => {
    expect(shortItem(lockup("50MR8HO7cy0", "Nutella Jar Chocolate", "140M views"))).toMatchObject({
      kind: "video", id: "50MR8HO7cy0", title: "Nutella Jar Chocolate", subtitle: "140M views",
    });
  });
  it("reads a short from the watch-next chain, which has no title", () => {
    const step = { type: "NavigationEndpoint", payload: { videoId: "YiDHzDpXLcs", thumbnail: { thumbnails: [{ url: "https://i.ytimg.com/vi/YiDHzDpXLcs/frame0.jpg", width: 1080 }] } } };
    expect(shortItem(step)).toMatchObject({ id: "YiDHzDpXLcs", title: "", thumb: expect.stringContaining("/thumb?u=") });
  });
  it("drops anything without a usable video id", () => {
    expect(shortItem(lockup("nope", "x", "y"))).toBeUndefined();
    expect(shortItem({ type: "Video" })).toBeUndefined();
  });
  it("finds shorts nested in sections and shelves", () => {
    const results = [{ type: "IncludingResultsFor" }, { type: "GridShelfView", contents: [lockup("aaaaaaaaaaa", "A", "1 view"), { type: "Section", items: [lockup("bbbbbbbbbbb", "B", "2 views")] }] }];
    expect(findShorts(results).map((n) => shortItem(n)?.id)).toEqual(["aaaaaaaaaaa", "bbbbbbbbbbb"]);
  });
  it("follows the chain while YouTube has more", async () => {
    const step = (id: string) => ({ payload: { videoId: id } });
    const info = {
      watch_next_feed: [step("aaaaaaaaaaa")],
      wn_has_continuation: true,
      async getWatchNextContinuation() {
        this.watch_next_feed = [step("bbbbbbbbbbb")];
        this.wn_has_continuation = false;
        return this;
      },
    };
    const first = sequenceChunk(info);
    expect(first.items.map((i) => i?.id)).toEqual(["aaaaaaaaaaa"]);
    const second = await first.more!();
    expect(second.items.map((i) => i?.id)).toEqual(["bbbbbbbbbbb"]);
    expect(second.more).toBeUndefined();
  });
});
