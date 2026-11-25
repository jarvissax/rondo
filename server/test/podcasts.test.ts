import type { FastifyInstance } from "fastify";
import { afterAll, beforeAll, describe, expect, it } from "vitest";
import { buildApp } from "../src/app.js";
import { podcastNode, shelfSection } from "../src/browse.js";
import { loadConfig } from "../src/config.js";
import {
  chaptersOf,
  clockSeconds,
  episodeActions,
  episodeInfo,
  episodeItem,
  episodeList,
  libraryPodcasts,
  podcastSearch,
  showItem,
  showMore,
  showPage,
} from "../src/podcasts.js";

// YouTube Music's raw shapes, trimmed to what podcasts.ts reads (as answered on 2026-11-25)
const SHOW = "MPSPPLrAXtmErZgOdP_8GztsuKi9nrraNbKKp4";
const browse = (browseId: string, pageType: string) => ({
  browseEndpoint: { browseId, browseEndpointContextSupportedConfigs: { browseEndpointContextMusicConfig: { pageType } } },
});
const watch = (videoId: string, musicVideoType = "MUSIC_VIDEO_TYPE_PODCAST_EPISODE") => ({
  watchEndpoint: { videoId, watchEndpointMusicSupportedConfigs: { watchEndpointMusicConfig: { musicVideoType } } },
});
const thumb = (url: string) => ({ musicThumbnailRenderer: { thumbnail: { thumbnails: [{ url, width: 544, height: 544 }] } } });
const sep = { text: " • " };
const progress = (pct: number, length: string, token = "TOK") => ({
  musicPlaybackProgressRenderer: {
    playbackProgressPercentage: pct,
    durationText: { runs: [sep, { text: length }] },
    videoPlaybackPositionFeedbackToken: token,
  },
});
const multiRow = (id: string, title: string, opts: { date?: string; pct?: number; show?: boolean } = {}) => ({
  musicMultiRowListItemRenderer: {
    thumbnail: thumb(`https://i.ytimg.com/vi/${id}/hq720.jpg`),
    onTap: watch(id),
    subtitle: { runs: [{ text: "290K views" }, sep, { text: opts.date ?? "6d ago" }] },
    playbackProgress: progress(opts.pct ?? 0, "3 hr 21 min"),
    title: { runs: [{ text: title, navigationEndpoint: browse(`MPED${id}`, "MUSIC_PAGE_TYPE_NON_MUSIC_AUDIO_TRACK_PAGE") }] },
    ...(opts.show ? { secondTitle: { runs: [{ text: "Lex Fridman Podcast", navigationEndpoint: browse(SHOW, "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE") }] } } : {}),
  },
});
const header = (extra: Record<string, unknown> = {}) => ({
  musicResponsiveHeaderRenderer: {
    thumbnail: thumb("https://i.ytimg.com/pl_c/PLrAX/studio_square_thumbnail.jpg"),
    title: { runs: [{ text: "Lex Fridman Podcast" }] },
    straplineTextOne: { runs: [{ text: "Lex Fridman", navigationEndpoint: browse("UCSHZKyawb77ixDdsGog4iWA", "MUSIC_PAGE_TYPE_USER_CHANNEL") }] },
    description: { musicDescriptionShelfRenderer: { description: { runs: [{ text: "Conversations about AI." }] } } },
    ...extra,
  },
});
const page = (head: unknown, shelf: unknown) => ({
  contents: {
    twoColumnBrowseResultsRenderer: {
      tabs: [{ tabRenderer: { content: { sectionListRenderer: { contents: [head] } } } }],
      secondaryContents: { sectionListRenderer: { contents: [shelf] } },
    },
  },
});

describe("podcast reducers", () => {
  it("reads clocks only", () => {
    expect(clockSeconds("3:21:14")).toBe(12074);
    expect(clockSeconds("45:07")).toBe(2707);
    expect(clockSeconds("3 hr 21 min")).toBeUndefined();
    expect(clockSeconds(undefined)).toBeUndefined();
  });

  it("reads a show's page: header, saved, episodes newest first, the next page", () => {
    const saveToggle = { toggleButtonRenderer: { isToggled: true, defaultServiceEndpoint: { likeEndpoint: { status: "LIKE" } } } };
    const data = page(header({ buttons: [saveToggle] }), {
      musicShelfRenderer: {
        contents: [multiRow("v1X9Fqs5a14", "GTA 6 | #505"), multiRow("2hJkprFnzhY", "Ukraine | #504", { date: "Nov 11", pct: 100 })],
        continuations: [{ nextContinuationData: { continuation: "NEXT" } }],
      },
    });
    const sp = showPage(data, SHOW);
    expect(sp.title).toBe("Lex Fridman Podcast");
    expect(sp.subtitle).toBe("Lex Fridman");
    expect(sp.description).toBe("Conversations about AI.");
    expect(sp.saved).toBe(true);
    expect(sp.token).toBe("NEXT");
    expect(sp.items).toHaveLength(2);
    expect(sp.items[0]).toMatchObject({ kind: "episode", id: "v1X9Fqs5a14", subtitle: "6d ago · 3 hr 21 min", show: "Lex Fridman Podcast", showId: SHOW, date: "6d ago", length: "3 hr 21 min", progress: 0, played: false });
    expect(sp.items[1]).toMatchObject({ played: true, progress: 100, date: "Nov 11" });
    expect(sp.items[0]!.thumb).toMatch(/^\/thumb\?u=/);
  });

  it("leaves saved out when signed out (the toggle opens a sign-in prompt)", () => {
    const signIn = { toggleButtonRenderer: { isToggled: false, defaultNavigationEndpoint: { modalEndpoint: {} } } };
    expect(showPage(page(header({ buttons: [signIn] }), { musicShelfRenderer: { contents: [] } }), SHOW).saved).toBeUndefined();
  });

  it("reads a show's later page", () => {
    const more = showMore({ continuationContents: { musicShelfContinuation: { contents: [multiRow("ZAzWPE9CItg", "#503")], continuations: [{ nextContinuationData: { continuation: "N2" } }] } } }, { title: "Lex", id: SHOW });
    expect(more.token).toBe("N2");
    expect(more.items[0]).toMatchObject({ id: "ZAzWPE9CItg", show: "Lex", showId: SHOW, subtitle: "6d ago · 3 hr 21 min" });
  });

  it("names the show on rows of mixed lists (New Episodes)", () => {
    expect(episodeItem(multiRow("brzkTni-nTQ", "Power of Now", { date: "1d ago", show: true }))).toMatchObject({
      show: "Lex Fridman Podcast",
      showId: SHOW,
      subtitle: "Lex Fridman Podcast · 1d ago · 3 hr 21 min",
    });
  });

  it("reads Episodes for Later rows: the show, the date before it, the clock", () => {
    const row = {
      musicResponsiveListItemRenderer: {
        thumbnail: thumb("https://i.ytimg.com/vi/brzkTni-nTQ/hqdefault.jpg"),
        flexColumns: [
          { musicResponsiveListItemFlexColumnRenderer: { text: { runs: [{ text: "Power of Now", navigationEndpoint: browse("MPEDbrzkTni-nTQ", "MUSIC_PAGE_TYPE_NON_MUSIC_AUDIO_TRACK_PAGE") }] } } },
          { musicResponsiveListItemFlexColumnRenderer: { text: { runs: [{ text: "1d ago" }, sep, { text: "Huberman Lab", navigationEndpoint: browse("MPSPPLPNW_gerXa4Pc8S2qoUQc5e8Ir97RLuVW", "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE") }] } } },
        ],
        fixedColumns: [{ musicResponsiveListItemFixedColumnRenderer: { text: { runs: [{ text: "2:06:13" }] } } }],
        playlistItemData: { videoId: "brzkTni-nTQ" },
      },
    };
    const list = episodeList(page({ musicResponsiveHeaderRenderer: { title: { runs: [{ text: "Episodes for Later" }] } } }, { musicPlaylistShelfRenderer: { contents: [row] } }));
    expect(list.title).toBe("Episodes for Later");
    expect(list.items[0]).toMatchObject({ id: "brzkTni-nTQ", durationSec: 7573, show: "Huberman Lab", date: "1d ago", subtitle: "Huberman Lab · 1d ago" });
  });

  it("reads a search's episode row by its marks, not its words (the date sits before the show)", () => {
    const row = {
      musicResponsiveListItemRenderer: {
        overlay: { musicItemThumbnailOverlayRenderer: { content: { musicPlayButtonRenderer: { playNavigationEndpoint: watch("_bBRVNkAfkQ") } } } },
        flexColumns: [
          { musicResponsiveListItemFlexColumnRenderer: { text: { runs: [{ text: "Ancient Civilizations" }] } } },
          { musicResponsiveListItemFlexColumnRenderer: { text: { runs: [{ text: "Episodio" }, sep, { text: "13 dic 2025" }, sep, { text: "Lex Fridman Podcast", navigationEndpoint: browse(SHOW, "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE") }] } } },
        ],
        playlistItemData: { videoId: "_bBRVNkAfkQ" },
      },
    };
    const song = { musicResponsiveListItemRenderer: { ...row.musicResponsiveListItemRenderer, overlay: { musicItemThumbnailOverlayRenderer: { content: { musicPlayButtonRenderer: { playNavigationEndpoint: watch("aaaaaaaaaaa", "MUSIC_VIDEO_TYPE_ATV") } } } } } };
    const res = podcastSearch({ contents: { tabbedSearchResultsRenderer: { tabs: [{ tabRenderer: { content: { sectionListRenderer: { contents: [{ musicShelfRenderer: { contents: [row, song], continuations: [{ nextContinuationData: { continuation: "S2" } }] } }] } } } }] } } }, "episode");
    expect(res.token).toBe("S2");
    expect(res.items[0]).toMatchObject({ id: "_bBRVNkAfkQ", date: "13 dic 2025", show: "Lex Fridman Podcast" });
    expect(res.items[1]).toBeUndefined(); // a song isn't an episode
  });

  it("reads shows from search rows and the library's cards", () => {
    const searchRow = {
      musicResponsiveListItemRenderer: {
        navigationEndpoint: browse(SHOW, "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE"),
        flexColumns: [
          { musicResponsiveListItemFlexColumnRenderer: { text: { runs: [{ text: "Lex Fridman Podcast" }] } } },
          { musicResponsiveListItemFlexColumnRenderer: { text: { runs: [{ text: "Podcast" }, sep, { text: "Lex Fridman" }] } } },
        ],
      },
    };
    expect(showItem(searchRow)).toEqual({ kind: "podcast", id: SHOW, title: "Lex Fridman Podcast", subtitle: "Lex Fridman" });
    const card = (browseId: string, title: string, sub: string, pageType = "MUSIC_PAGE_TYPE_PLAYLIST") => ({
      musicTwoRowItemRenderer: { navigationEndpoint: browse(browseId, pageType), title: { runs: [{ text: title }] }, subtitle: { runs: [{ text: sub }] } },
    });
    const lib = libraryPodcasts({
      contents: {
        grid: [
          { musicTwoRowItemRenderer: { title: { runs: [{ text: "Add podcast" }] }, navigationEndpoint: { openPopupAction: {} } } },
          card("VLRDPN", "New Episodes", "Auto playlist • 12 episodes"),
          card("VLSE", "Episodes for Later", "Auto playlist • 1 episode"),
          card("MPSPPLPNW_gerXa4Pc8S2qoUQc5e8Ir97RLuVW", "Huberman Lab", "Andrew Huberman", "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE"),
        ],
      },
    });
    expect(lib.shows.map((s) => s.title)).toEqual(["Huberman Lab"]);
    expect(lib.later).toBe("Auto playlist • 1 episode");
    expect(lib.latest).toBe("Auto playlist • 12 episodes");
  });

  it("finds the Mark as played toggle by its token, and Episodes for Later's", () => {
    const later = { toggleButtonRenderer: { isToggled: true, defaultServiceEndpoint: { commandExecutorCommand: {} }, toggledServiceEndpoint: { playlistEditEndpoint: { playlistId: "SE" } } } };
    const menu = {
      menuRenderer: {
        items: [
          { toggleMenuServiceItemRenderer: { isToggled: false, defaultServiceEndpoint: { feedbackEndpoint: { feedbackToken: "PIN" } }, toggledServiceEndpoint: { feedbackEndpoint: { feedbackToken: "UNPIN" } } } },
          { toggleMenuServiceItemRenderer: { isToggled: true, defaultServiceEndpoint: { feedbackEndpoint: { feedbackToken: "TOK" } }, toggledServiceEndpoint: { feedbackEndpoint: { feedbackToken: "UNTOK" } } } },
        ],
      },
    };
    const data = page(header({ progress: progress(100, "2 hr"), buttons: [later, menu] }), {});
    expect(episodeActions(data)).toEqual({ played: "TOK", unplayed: "UNTOK", isPlayed: true, later: true });
    // signed out: the toggle opens a sign-in prompt, no tokens
    const out = page(header({ progress: progress(0, "2 hr"), buttons: [{ toggleButtonRenderer: { defaultServiceEndpoint: { modalEndpoint: {} }, toggledServiceEndpoint: { playlistEditEndpoint: { playlistId: "SE" } } } }] }), {});
    expect(episodeActions(out)).toEqual({});
  });

  it("reads chapters from YouTube's markers, and the length from the queue's row", () => {
    const chapter = (title: string, ms: number) => ({ chapterRenderer: { title: { simpleText: title }, timeRangeStartMillis: ms } });
    const web = {
      playerOverlays: {
        playerOverlayRenderer: {
          decoratedPlayerBarRenderer: {
            decoratedPlayerBarRenderer: {
              playerBar: {
                multiMarkersPlayerBarRenderer: {
                  markersMap: [{ key: "DESCRIPTION_CHAPTERS", value: { chapters: [chapter("Introduction", 0), chapter("GTA 6", 106000), chapter("Miami", 1373000)] } }],
                },
              },
            },
          },
        },
      },
    };
    expect(chaptersOf(web)).toEqual([
      { ms: 0, title: "Introduction" },
      { ms: 106000, title: "GTA 6" },
      { ms: 1373000, title: "Miami" },
    ]);
    expect(chaptersOf({})).toEqual([]);
    const next = { contents: { panel: [{ playlistPanelVideoRenderer: { videoId: "x0000000000", lengthText: { runs: [{ text: "1:00" }] } } }, { playlistPanelVideoRenderer: { videoId: "v1X9Fqs5a14", lengthText: { runs: [{ text: "3:21:14" }] } } }] } };
    const mped = page(header({ title: { runs: [{ text: "GTA 6 | #505" }] }, straplineTextOne: { runs: [{ text: "Lex Fridman Podcast", navigationEndpoint: browse(SHOW, "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE") }] }, subtitle: { runs: [{ text: "290K views" }, sep, { text: "6d ago" }] }, progress: progress(40, "3 hr 21 min") }), {
      musicDescriptionShelfRenderer: { description: { runs: [{ text: "Larry Lawton…" }] } },
    });
    const info = episodeInfo("v1X9Fqs5a14", mped, next, web);
    expect(info).toMatchObject({ id: "v1X9Fqs5a14", title: "GTA 6 | #505", show: "Lex Fridman Podcast", showId: SHOW, durationSec: 12074, date: "6d ago", description: "Larry Lawton…", progress: 40, played: false });
    expect(info?.chapters).toHaveLength(3);
  });

  it("turns home shelves' shows and episodes into podcast kinds only when asked", () => {
    const show = { type: "MusicTwoRowItem", item_type: "video", id: SHOW, title: { text: "Lex Fridman Podcast" }, subtitle: { text: "Podcast • Lex Fridman" }, endpoint: { payload: browse(SHOW, "MUSIC_PAGE_TYPE_PODCAST_SHOW_DETAIL_PAGE").browseEndpoint } };
    const ep = { type: "MusicTwoRowItem", item_type: "video", id: "v1X9Fqs5a14", title: { text: "GTA 6" }, subtitle: { text: "Episode • Lex Fridman Podcast" }, endpoint: { payload: watch("v1X9Fqs5a14").watchEndpoint } };
    expect(podcastNode(show)).toMatchObject({ kind: "podcast", id: SHOW, subtitle: "Lex Fridman" });
    expect(podcastNode(ep)).toMatchObject({ kind: "episode", id: "v1X9Fqs5a14" });
    const shelf = { header: { title: { text: "Podcasts" } }, contents: [show, ep] };
    expect(shelfSection(shelf, 20, true)?.items.map((i) => i.kind)).toEqual(["podcast", "episode"]);
    // an older Rondo: the show's card (a "video" whose id isn't a video's) is left out, the episode plays as a video
    expect(shelfSection(shelf, 20, false)?.items.map((i) => [i.kind, i.id])).toEqual([["video", "v1X9Fqs5a14"]]);
  });
});

describe("podcast routes", () => {
  let app: FastifyInstance;
  const auth = { authorization: `Bearer ${"t".repeat(64)}` };
  let client = 0;
  const addr = () => `10.1.${Math.floor(++client / 250)}.${client % 250}`;
  const get = (url: string) => app.inject({ url, headers: auth, remoteAddress: addr() });
  const post = (url: string, payload: unknown) => app.inject({ method: "POST", url, headers: auth, payload: payload as Record<string, unknown>, remoteAddress: addr() });

  beforeAll(async () => {
    const config = loadConfig({ API_TOKEN: "t".repeat(64), YTDLP_BIN: "/nonexistent", LOG_LEVEL: "silent", COOKIES_FILE: "/nonexistent/cookies.txt" });
    ({ app } = await buildApp(config));
    await app.ready();
  });
  afterAll(async () => {
    await app.close();
  });

  it.each(["/music/podcast/MPSPa%20b", "/music/episode/short"])("rejects a bad id: %s", async (url) => {
    const res = await get(url);
    expect(res.statusCode).toBe(400);
    expect(res.json()).toEqual({ error: "id" });
  });

  it("needs cookies.txt for the account's podcasts", async () => {
    for (const url of ["/music/podcasts", "/music/podcasts/later", "/music/podcasts/new"]) expect((await get(url)).statusCode).toBe(503);
    expect((await post("/music/episode/v1X9Fqs5a14/played", { played: true })).statusCode).toBe(503);
  });

  it("checks what an action asks before the account", async () => {
    expect((await post("/music/episode/v1X9Fqs5a14/later", { later: "yes" })).statusCode).toBe(400);
    expect((await post(`/music/podcast/${SHOW}/save`, {})).statusCode).toBe(400);
  });

  it("answers an expired token of a show's pages and podcast search with 410", async () => {
    for (const url of [`/music/podcast/${SHOW}?next=unknown`, "/music/search?type=podcast&q=lex&next=unknown"]) expect((await get(url)).statusCode).toBe(410);
  });
});
