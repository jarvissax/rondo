import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync } from "node:fs";
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import type { AddressInfo } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { gzipSync } from "node:zlib";
import type { FastifyInstance } from "fastify";
import { afterAll, afterEach, beforeAll, describe, expect, it } from "vitest";
import { buildApp } from "../src/app.js";
import { loadConfig } from "../src/config.js";
import { FeedError, FeedFetcher, type FeedSource, feedText, isPublicAddress } from "../src/feedFetch.js";
import { indexHeaders, type IndexGet, PodcastIndex } from "../src/podcastIndex.js";
import { FeedCache, feedPage } from "../src/rss.js";
import { cutBytes, HTML_ENTITIES, parseDate, parseDuration, parseFeed, plainText, typeFromUrl, wholeNumber, xmlText } from "../src/rssFeed.js";

const fixture = (name: string) => readFileSync(new URL(`./fixtures/rss/${name}`, import.meta.url), "utf8");
const TOKEN = "t".repeat(64);
const utc = (iso: string) => Date.parse(iso) / 1000;

describe("RSS dates", () => {
  it.each([
    ["Tue, 01 Oct 2024 10:00:00 GMT", "2024-10-01T10:00:00Z"],
    ["Tue, 1 Oct 2024 10:00:00 +0000", "2024-10-01T10:00:00Z"],
    ["1 Oct 2024 10:00 UT", "2024-10-01T10:00:00Z"],
    ["Tuesday, 01 oct 2024 10:00:00 UTC", "2024-10-01T10:00:00Z"],
    ["Thu, 05 Sept 2024 12:00:00 EDT", "2024-09-05T16:00:00Z"],
    ["Thu, 05 September 2024 12:00:00 -0500", "2024-09-05T17:00:00Z"],
    ["Thu, 05 Sep 2024 12:00:00 +05:30", "2024-09-05T06:30:00Z"],
    ["Thu, 05 Sep 2024 12:00:00 PST", "2024-09-05T20:00:00Z"],
    ["Thu, 05 Sep 2024 12:00:00 CEST", "2024-09-05T12:00:00Z"], // unknown zone: UTC
    ["Thu, 05 Sep 2024 12:00:00", "2024-09-05T12:00:00Z"], // no zone: UTC
    ["Mon, 30 Sep 24 9:05 -0400", "2024-09-30T13:05:00Z"],
    ["Fri, 31 Dec 99 23:59:59 Z", "1999-12-31T23:59:59Z"],
    ["  Wed, 02 Oct 2002 08:00:00 EST  ", "2002-10-02T13:00:00Z"],
    ["2024-05-01T10:00:00Z", "2024-05-01T10:00:00Z"],
    ["2024-05-01T10:00:00+02:00", "2024-05-01T08:00:00Z"],
    ["2024-05-01T10:00:00.123-0100", "2024-05-01T11:00:00Z"],
    ["2024-05-01", "2024-05-01T00:00:00Z"],
  ])("reads %s", (text, iso) => {
    expect(parseDate(text)).toBe(utc(iso));
  });

  it.each(["", "yesterday", "Tue, 01 Foo 2024 10:00:00 GMT", "Tue, 01 Oct 2024", "01/10/2024", "Tue, 41 Oct 2024 10:00:00 GMT", "Tue, 01 Oct 2024 25:00:00 GMT", "2024-13-01"])(
    "gives 0 for %j",
    (text) => {
      expect(parseDate(text)).toBe(0);
    },
  );
});

describe("RSS durations and numbers", () => {
  it.each([
    ["1:02:03", 3723],
    ["01:02:03", 3723],
    ["75:30", 4530],
    ["3:05", 185],
    ["3600", 3600],
    ["3600.5", 3600],
    [" 42 ", 42],
    ["0", 0],
    ["", 0],
    ["unknown", 0],
    ["-5", 0],
    ["1:2:3:4", 0],
    ["1h 2m", 0],
  ])("%j → %d", (text, s) => {
    expect(parseDuration(text)).toBe(s);
  });

  it("reads whole numbers only", () => {
    expect(wholeNumber(" 12345678 ")).toBe(12345678);
    expect(wholeNumber("3.0")).toBe(3);
    expect(wholeNumber("abc")).toBe(0);
    expect(wholeNumber("12abc")).toBe(0);
    expect(wholeNumber("-1")).toBe(0);
    expect(wholeNumber("")).toBe(0);
  });

  it("guesses an enclosure's type from its path", () => {
    expect(typeFromUrl("https://x.example/a/ep.MP3?dl=1#t")).toBe("audio/mpeg");
    expect(typeFromUrl("https://x.example/ep.m4b")).toBe("audio/mp4");
    expect(typeFromUrl("https://x.example/ep.opus")).toBe("audio/ogg");
    expect(typeFromUrl("https://x.example/ep.mov")).toBe("video/quicktime");
    expect(typeFromUrl("https://x.example/ep.mp3/")).toBe("");
    expect(typeFromUrl("https://x.example/download?file=ep.mp3")).toBe("");
    expect(typeFromUrl("not a url/ep.aac")).toBe("audio/aac");
  });
});

describe("RSS plain text", () => {
  it("decodes the XML layer: CDATA as it is, entities, comments gone, tags kept", () => {
    expect(xmlText("a &amp; b &lt;i&gt; &#38; &#x26; &nbsp;")).toBe("a & b <i> & & &nbsp;");
    expect(xmlText("<![CDATA[<b>&amp;</b>]]> &amp; <!-- no -->x")).toBe("<b>&amp;</b> & x");
    expect(xmlText('<a href="?a=1&amp;b=2">x</a>')).toBe('<a href="?a=1&b=2">x</a>');
  });

  it("turns HTML into lines", () => {
    expect(plainText("<p>One</p><p>Two<br>three<br/>four</p>")).toBe("One\nTwo\nthree\nfour");
    expect(plainText("<ul><li>a</li><li>b</li></ul>after")).toBe("• a\n• b\n\nafter"); // the ends of li and ul: two breaks
    expect(plainText("<h2>Title</h2><div>x</div><blockquote>q</blockquote><pre>p</pre><table><tr><td>c</td></tr></table>")).toBe("Title\nx\nq\np\nc");
    expect(plainText("<script>var a = '<p>'</script>kept<style>.a{}</style> <SCRIPT src=x></SCRIPT>")).toBe("kept");
    expect(plainText('<a href="x">link</a> <img src="y"> <span>text</span>')).toBe("link text");
    expect(plainText("a < b and 3<4")).toBe("a < b and 3<4");
  });

  it("decodes HTML's entities after the tags", () => {
    expect(plainText("&lt;b&gt; is a tag")).toBe("<b> is a tag");
    expect(plainText("caf&eacute; &hellip; &mdash; &ldquo;q&rdquo; &rsquo; &copy; &#8217; &#x1F600; &bogus; &")).toBe("café … — “q” ’ © ’ 😀 &bogus; &");
    expect(HTML_ENTITIES.nbsp).toBe(" ");
    expect(HTML_ENTITIES.yuml).toBe("ÿ");
    expect(HTML_ENTITIES.szlig).toBe("ß");
  });

  it("collapses spaces, trims lines, keeps at most one empty line", () => {
    expect(plainText("  a \t  b&nbsp;&nbsp;c  \r\n  d  \n\n\n\n e ")).toBe("a b c\nd\n\ne");
    expect(plainText("x<br><br><br><br>y")).toBe("x\n\ny");
  });

  it("puts a title on one line", () => {
    expect(plainText("Part\none<br>and two", { oneLine: true })).toBe("Part one and two");
  });

  it("cuts at a UTF-8 character's boundary with an ellipsis inside the limit", () => {
    const s = "é".repeat(100); // 200 bytes
    const cut = cutBytes(s, 50);
    expect(Buffer.byteLength(cut)).toBeLessThanOrEqual(50);
    expect(cut).toBe(`${"é".repeat(23)}…`);
    expect(cutBytes("short", 50)).toBe("short");
    expect(Buffer.byteLength(plainText("word ".repeat(2000), { maxBytes: 4000 }))).toBeLessThanOrEqual(4000);
  });
});

describe("parseFeed", () => {
  it("reads a podcast's channel", () => {
    const feed = parseFeed(fixture("show.xml"))!;
    expect(feed).toMatchObject({
      title: "The Example & Friends Show",
      author: "Example Media", // itunes:owner's name, no itunes:author
      description: "A show about examples & friends.\nEvery week.", // itunes:summary over description
      art: "https://example.com/art.jpg", // itunes:image over image/url
      link: "https://example.com/show", // the first non-empty, not atom:link
      language: "en-us",
      serial: true,
      explicit: true,
      complete: true,
      newFeedUrl: "https://new.example.com/feed.xml",
      count: 3,
    });
  });

  it("reads its episodes in the feed's order, skipping one without an enclosure", () => {
    const { episodes } = parseFeed(fixture("show.xml"))!;
    expect(episodes.map((e) => e.title)).toEqual(["Episode 3: Bold moves", "Trailer", "Bonus with only an itunes title"]);
    expect(episodes[0]).toEqual({
      guid: "ep-3",
      title: "Episode 3: Bold moves",
      date: utc("2024-10-01T10:00:00Z"),
      duration: 3723,
      url: "https://cdn.example.com/ep3.mp3", // the audio one, not the PDF before it
      type: "audio/mpeg",
      size: 12345678,
      image: "https://example.com/ep3.jpg",
      chapters: "https://example.com/ep3.json",
      season: 2,
      episode: 3,
      episodeType: "full",
      notes: "Show notes for three.\n\n• One\n• Two …\n\nBye! — café", // content:encoded, script and style gone
    });
    expect(episodes[1]).toMatchObject({
      guid: "https://cdn.example.com/trailer.m4a", // an empty guid: the enclosure's address
      type: "audio/mp4", // no type: from the extension
      size: 0,
      duration: 4530,
      date: utc("2024-09-30T13:05:00Z"),
      episodeType: "trailer",
      notes: "Line one\nLine two", // an entity-escaped <br> in the description
    });
    expect(episodes[2]).toMatchObject({ guid: "bonus-1", type: "video/mp4", duration: 3600, date: utc("2024-09-01T06:30:00Z"), episodeType: "bonus", notes: "Summary & more" });
  });

  it("reads an old, plain feed", () => {
    const feed = parseFeed(fixture("plain.xml"))!;
    expect(feed).toMatchObject({ title: "Old Feed", author: "editor@example.org (The Editor)", description: "Escaped HTML.", art: "https://example.org/logo.png", language: "de", serial: false, explicit: false });
    expect(feed.episodes).toHaveLength(2);
    expect(feed.episodes[0]).toMatchObject({ title: "CDATA title & more", type: "audio/ogg", date: 0, duration: 0, notes: "" });
    // raw HTML (not escaped) in an item: a <p> left open, a <br> without its end
    expect(feed.episodes[1]).toMatchObject({ title: "Raw HTML in a title second line", size: 2048, date: utc("2024-09-05T16:00:00Z"), notes: "Raw paragraph onetwo\nthree" });
  });

  it("takes the first audio or video enclosure, else the first", () => {
    const xml = (encs: string) => `<rss><channel><item>${encs}</item></channel></rss>`;
    const url = (encs: string) => parseFeed(xml(encs))!.episodes[0]?.url;
    expect(url('<enclosure url="a.pdf" type="application/pdf"/><enclosure url="b.mp4" type="video/mp4"/><enclosure url="c.mp3" type="audio/mpeg"/>')).toBe("b.mp4");
    expect(url('<enclosure url="a.pdf" type="application/pdf"/><enclosure url="b.txt" type="text/plain"/>')).toBe("a.pdf");
    expect(url('<enclosure url="" type="audio/mpeg"/><enclosure url="b.mp3"/>')).toBe("b.mp3");
    expect(url('<enclosure type="audio/mpeg"/>')).toBeUndefined();
  });

  it("keeps channel fields apart from the image's and the items'", () => {
    const feed = parseFeed(`<rss><channel>
      <image><title>Image title</title><url>https://x.example/i.png</url><link>https://x.example/img</link></image>
      <item><title>Item</title><link>https://x.example/item</link><enclosure url="https://x.example/1.mp3"/></item>
      <title>Channel title</title><itunes:author>Host</itunes:author><managingEditor>e@x.example</managingEditor>
    </channel></rss>`)!;
    expect(feed).toMatchObject({ title: "Channel title", link: "", art: "https://x.example/i.png", author: "Host", count: 1 });
  });

  it("reads explicit and type words in any case", () => {
    const flags = (inner: string) => parseFeed(`<rss><channel>${inner}</channel></rss>`)!;
    expect(flags("<itunes:explicit>true</itunes:explicit>").explicit).toBe(true);
    expect(flags("<itunes:explicit>EXPLICIT</itunes:explicit>").explicit).toBe(true);
    expect(flags("<itunes:explicit>clean</itunes:explicit>").explicit).toBe(false);
    expect(flags("<itunes:explicit>no</itunes:explicit>").explicit).toBe(false);
    expect(flags("<itunes:type>episodic</itunes:type>").serial).toBe(false);
    expect(flags("<itunes:complete>No</itunes:complete>").complete).toBe(false);
    expect(flags("").count).toBe(0);
  });

  it("knows what isn't a feed", () => {
    expect(parseFeed(fixture("page.html"))).toBeNull();
    expect(parseFeed(fixture("atom.xml"))).toBeNull();
    expect(parseFeed('{"title": "json"}')).toBeNull();
    expect(parseFeed("")).toBeNull();
    expect(parseFeed("<rss version='2.0'></rss>")).toBeNull(); // no channel
  });

  it("reads the first channel only and stops at its end", () => {
    const feed = parseFeed(`<rss><channel><title>One</title><item><enclosure url="a.mp3"/></item></channel>
      <channel><title>Two</title><item><enclosure url="b.mp3"/></item></channel></rss>`)!;
    expect(feed.title).toBe("One");
    expect(feed.episodes.map((e) => e.url)).toEqual(["a.mp3"]);
  });

  it("doesn't trip over a close tag inside CDATA", () => {
    const feed = parseFeed(`<rss><channel><item><description><![CDATA[a </description> b]]></description><enclosure url="a.mp3"/></item></channel></rss>`)!;
    expect(feed.episodes[0]!.notes).toBe("a b");
  });
});

describe("feed text", () => {
  it("decodes by the BOM, the XML declaration, then the HTTP charset", () => {
    const latin1 = Buffer.concat([Buffer.from('<?xml version="1.0" encoding="ISO-8859-1"?><rss>'), Buffer.from([0x63, 0x61, 0x66, 0xe9]), Buffer.from("</rss>")]);
    expect(feedText(latin1, "application/rss+xml; charset=utf-8")).toContain("café");
    expect(feedText(Buffer.from([0x63, 0x61, 0x66, 0xe9]), "text/xml; charset=windows-1252")).toBe("café");
    expect(feedText(Buffer.from("﻿<rss>é</rss>"), "text/xml; charset=iso-8859-1")).toBe("<rss>é</rss>");
    expect(feedText(Buffer.from("<rss>é</rss>"), "text/xml; charset=no-such-thing")).toBe("<rss>é</rss>");
  });
});

describe("public addresses", () => {
  it.each(["127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.255", "192.168.1.10", "169.254.169.254", "100.64.0.1", "100.127.255.255", "0.0.0.0", "224.0.0.1", "255.255.255.255", "192.0.2.1", "198.18.0.1", "::1", "::", "fe80::1", "fe80::1%eth0", "fc00::1", "fd12:3456::1", "ff02::1", "::ffff:127.0.0.1", "::ffff:7f00:1", "::ffff:10.0.0.1", "64:ff9b::a00:1", "2002:c0a8:0101::1", "2001:db8::1", "2001::1", "not an address"])(
    "refuses %s",
    (ip) => {
      expect(isPublicAddress(ip)).toBe(false);
    },
  );

  it.each(["93.184.216.34", "8.8.8.8", "172.32.0.1", "100.128.0.1", "2606:4700:4700::1111", "2a00:1450:4001:80b::200e", "::ffff:8.8.8.8", "64:ff9b::808:808", "2002:0808:0808::1"])("allows %s", (ip) => {
    expect(isPublicAddress(ip)).toBe(true);
  });
});

// a podcast host on this machine, reached by made-up names: feeds.test answers 127.0.0.1, which the
// fetcher here counts as public; inside.test answers a private address
describe("fetching a feed", () => {
  let server: Server;
  let port = 0;
  const hits: IncomingMessage[] = [];
  const routes: Record<string, (req: IncomingMessage, res: ServerResponse) => void> = {};
  const resolve = async (host: string) => ({ localhost: ["127.0.0.1", "::1"], "feeds.test": ["127.0.0.1"], "other.test": ["127.0.0.1"], "inside.test": ["10.0.0.7"], "mixed.test": ["127.0.0.1", "192.168.0.2"] })[host] ?? Promise.reject(Object.assign(new Error("ENOTFOUND"), { code: "ENOTFOUND" }));
  const allowed = (ip: string) => ip === "127.0.0.1" || isPublicAddress(ip);
  const fetcher = (opts: { timeoutMs?: number; maxBytes?: number } = {}) => new FeedFetcher({ resolve, allowed, ...opts });
  const at = (path: string, host = "feeds.test") => `http://${host}:${port}${path}`;
  const failure = async (p: Promise<unknown>) => {
    try {
      await p;
    } catch (err) {
      expect(err).toBeInstanceOf(FeedError);
      return { code: (err as FeedError).code, status: (err as FeedError).status };
    }
    throw new Error("it didn't fail");
  };

  beforeAll(async () => {
    server = createServer((req, res) => {
      hits.push(req);
      const route = routes[req.url ?? ""];
      if (route) return route(req, res);
      res.writeHead(404).end();
    });
    await new Promise<void>((ok) => server.listen(0, "127.0.0.1", ok));
    port = (server.address() as AddressInfo).port;
    const show = fixture("show.xml");
    const send = (status: number, headers: Record<string, string>, body = "") => (_req: IncomingMessage, res: ServerResponse) => res.writeHead(status, headers).end(body);
    routes["/feed.xml"] = (req, res) => {
      if (req.headers["if-none-match"] === '"v1"') return res.writeHead(304).end();
      res.writeHead(200, { "content-type": "application/rss+xml", etag: '"v1"', "last-modified": "Tue, 01 Oct 2024 10:00:00 GMT" }).end(show);
    };
    routes["/gz.xml"] = (_req, res) => res.writeHead(200, { "content-encoding": "gzip" }).end(gzipSync(show));
    routes["/moved"] = send(301, { location: "/feed.xml" });
    routes["/temp"] = send(302, { location: "/moved" });
    routes["/to-other"] = send(308, { location: `http://other.test:${port}/feed.xml` });
    routes["/to-inside"] = send(302, { location: `http://inside.test:${port}/feed.xml` });
    routes["/to-ip"] = send(301, { location: "http://169.254.169.254/latest/meta-data/" });
    routes["/to-file"] = send(302, { location: "file:///etc/passwd" });
    for (let i = 0; i < 7; i++) routes[`/loop${i}`] = send(302, { location: `/loop${i + 1}` });
    routes["/gone"] = send(410, {});
    routes["/big"] = (_req, res) => res.writeHead(200, { "content-length": String(5000) }).end("x".repeat(5000));
    routes["/stream"] = (_req, res) => {
      res.writeHead(200);
      res.write("<rss>" + "x".repeat(3000));
      res.end("</rss>");
    };
    routes["/bomb"] = (_req, res) => res.writeHead(200, { "content-encoding": "gzip" }).end(gzipSync("y".repeat(100_000)));
    routes["/slow"] = (_req, res) => {
      res.writeHead(200);
      res.write("<rss>");
      // never ends
    };
    routes["/private"] = (req, res) => res.writeHead(200).end(req.headers.authorization ?? "none");
  });
  afterAll(async () => {
    server.closeAllConnections();
    await new Promise((ok) => server.close(ok));
  });
  afterEach(() => {
    hits.length = 0;
  });

  it("reads a feed with its ETag and Last-Modified, sending a Rondo user agent", async () => {
    const f = fetcher();
    const res = await f.fetch(at("/feed.xml"));
    expect(res.notModified).toBe(false);
    if (res.notModified) return;
    expect(parseFeed(res.text)?.title).toBe("The Example & Friends Show");
    expect(res.etag).toBe('"v1"');
    expect(res.lastModified).toBe("Tue, 01 Oct 2024 10:00:00 GMT");
    expect(hits[0]!.headers["user-agent"]).toMatch(/^Rondo server/);
    expect(await f.fetch(at("/feed.xml"), { etag: '"v1"' })).toEqual({ notModified: true, feedUrl: at("/feed.xml") });
    expect(hits[1]!.headers["if-none-match"]).toBe('"v1"');
    await f.close();
  });

  it("unpacks gzip", async () => {
    const f = fetcher();
    const res = await f.fetch(at("/gz.xml"));
    expect(!res.notModified && parseFeed(res.text)?.count).toBe(3);
    await f.close();
  });

  it("follows redirects; the address is the permanent one's", async () => {
    const f = fetcher();
    expect((await f.fetch(at("/moved"))).feedUrl).toBe(at("/feed.xml"));
    expect((await f.fetch(at("/temp"))).feedUrl).toBe(at("/temp")); // a temporary one first: the feed stays where it was
    expect((await f.fetch(at("/to-other"))).feedUrl).toBe(at("/feed.xml", "other.test"));
    expect(await failure(f.fetch(at("/loop0")))).toEqual({ code: "upstream", status: 302 }); // more than 5
    await f.close();
  });

  it("refuses addresses on the server's own network, redirects to them too", async () => {
    const f = new FeedFetcher({ resolve });
    for (const url of ["http://127.0.0.1/feed", "http://localhost/feed", "http://[::1]/feed", "http://10.0.0.1/", "http://169.254.169.254/latest/meta-data/", "http://100.64.1.1/", "http://2130706433/", "http://0x7f.1/", `http://inside.test:${port}/x`, `http://mixed.test:${port}/x`, "ftp://example.com/feed"]) {
      expect(await failure(f.fetch(url)), url).toEqual({ code: "url", status: 0 });
    }
    expect(hits).toHaveLength(0);
    const g = fetcher();
    for (const path of ["/to-inside", "/to-ip", "/to-file"]) expect(await failure(g.fetch(at(path))), path).toEqual({ code: "url", status: 0 });
    await Promise.all([f.close(), g.close()]);
  });

  it("checks the address again as it connects (a name that changes its answer)", async () => {
    let asked = 0;
    // public when checked, private when the socket looks it up
    const rebinding = async () => (++asked === 1 ? ["127.0.0.1"] : ["10.0.0.9"]);
    const f = new FeedFetcher({ resolve: rebinding, allowed });
    expect(await failure(f.fetch(at("/feed.xml", "rebind.test")))).toEqual({ code: "url", status: 0 });
    expect(asked).toBe(2);
    expect(hits).toHaveLength(0);
    await f.close();
  });

  it("says what the host answered", async () => {
    const f = fetcher();
    expect(await failure(f.fetch(at("/gone")))).toEqual({ code: "upstream", status: 410 });
    expect(await failure(f.fetch(at("/x", "nowhere.test")))).toEqual({ code: "upstream", status: 0 });
    await f.close();
  });

  it("refuses a feed over the limit, by its length or as it comes", async () => {
    const f = fetcher({ maxBytes: 1000 });
    expect(await failure(f.fetch(at("/big")))).toEqual({ code: "too_big", status: 0 });
    expect(await failure(f.fetch(at("/stream")))).toEqual({ code: "too_big", status: 0 });
    expect(await failure(f.fetch(at("/bomb")))).toEqual({ code: "too_big", status: 0 }); // counted unpacked
    await f.close();
  });

  it("gives up after its time", async () => {
    const f = fetcher({ timeoutMs: 300 });
    expect(await failure(f.fetch(at("/slow")))).toEqual({ code: "timeout", status: 0 });
    await f.close();
  });

  it("sends an address's user name and password as Basic auth, to that host only", async () => {
    const f = fetcher();
    const res = await f.fetch(`http://me:p%40ss@feeds.test:${port}/private`);
    expect(!res.notModified && res.text).toBe(`Basic ${Buffer.from("me:p@ss").toString("base64")}`);
    await f.close();
  });
});

describe("the feed cache", () => {
  const show = fixture("show.xml");

  it("keeps a feed 10 minutes, then asks again with its ETag", async () => {
    let now = 1_000_000;
    const asked: Array<{ etag?: string }> = [];
    const source: FeedSource = async (_url, cond = {}) => {
      asked.push(cond);
      return cond.etag ? { notModified: true, feedUrl: "https://x.example/feed" } : { notModified: false, text: show, feedUrl: "https://x.example/feed", etag: '"a"' };
    };
    const cache = new FeedCache(source, { now: () => now });
    const first = await cache.get("https://x.example/feed");
    expect(first.count).toBe(3);
    now += 9 * 60_000;
    expect(await cache.get("https://x.example/feed")).toBe(first);
    expect(asked).toHaveLength(1);
    now += 2 * 60_000;
    expect(await cache.get("https://x.example/feed")).toBe(first);
    expect(asked).toEqual([{}, { etag: '"a"', lastModified: undefined }]);
  });

  it("shares a read under way and drops what failed", async () => {
    let calls = 0;
    const source: FeedSource = async () => {
      calls++;
      if (calls === 1) return { notModified: false, text: "<html></html>", feedUrl: "u" };
      return { notModified: false, text: show, feedUrl: "u" };
    };
    const cache = new FeedCache(source);
    await expect(cache.get("u")).rejects.toMatchObject({ code: "not_feed" });
    const [a, b] = await Promise.all([cache.get("u"), cache.get("u")]);
    expect(a).toBe(b);
    expect(calls).toBe(2);
  });

  it("lets the least recently used feeds go past its size", async () => {
    const asked: string[] = [];
    const source: FeedSource = async (url) => (asked.push(url), { notModified: false, text: show, feedUrl: url });
    const probe = new FeedCache(source);
    await probe.get("a");
    const size = probe.size.bytes;
    expect(size).toBeGreaterThan(1000);
    const cache = new FeedCache(source, { maxBytes: size * 2.5 });
    await cache.get("a");
    await cache.get("b");
    await cache.get("a"); // a used last
    await cache.get("c"); // b goes
    expect(cache.size).toEqual({ entries: 2, bytes: size * 2 });
    asked.length = 0;
    await cache.get("a");
    await cache.get("c");
    expect(asked).toEqual([]);
    await cache.get("b");
    expect(asked).toEqual(["b"]);
    // a feed bigger than the whole cache is answered, not kept
    const small = new FeedCache(source, { maxBytes: 10 });
    expect((await small.get("a")).count).toBe(3);
    expect(small.size.entries).toBe(0);
  });
});

describe("feed pages", () => {
  const feed = { feed: "https://x.example/feed", ...parseFeed(fixture("show.xml"))! };

  it("pages the episodes with offset, limit and next", () => {
    const first = feedPage(feed, { limit: "2" });
    expect(first).toMatchObject({ feed: "https://x.example/feed", title: "The Example & Friends Show", count: 3, offset: 0, next: 2 });
    expect(first.episodes.map((e) => e.guid)).toEqual(["ep-3", "https://cdn.example.com/trailer.m4a"]);
    const last = feedPage(feed, { offset: "2", limit: "2" });
    expect(last).toMatchObject({ offset: 2, next: null });
    expect(last.episodes.map((e) => e.guid)).toEqual(["bonus-1"]);
    expect(feedPage(feed, { offset: "10" })).toMatchObject({ offset: 10, next: null, episodes: [] });
    // the defaults, and what's out of range
    expect(feedPage(feed, {})).toMatchObject({ offset: 0, next: null });
    expect(feedPage(feed, { offset: "-4", limit: "0" })).toMatchObject({ offset: 0, next: 1 });
    expect(feedPage(feed, { limit: "9999" }).episodes).toHaveLength(3);
    expect(Object.keys(first)).toEqual(["feed", "title", "author", "description", "art", "link", "language", "serial", "explicit", "complete", "newFeedUrl", "count", "offset", "next", "episodes"]);
  });

  it("with since, pages only the episodes dated after it, count still every one", () => {
    const since = utc("2024-09-15T00:00:00Z");
    const page = feedPage(feed, { since: String(since), limit: "1" });
    expect(page).toMatchObject({ count: 3, offset: 0, next: 1 });
    expect(page.episodes.map((e) => e.guid)).toEqual(["ep-3"]);
    const next = feedPage(feed, { since: String(since), offset: "1", limit: "1" });
    expect(next).toMatchObject({ count: 3, offset: 1, next: null });
    expect(next.episodes.map((e) => e.guid)).toEqual(["https://cdn.example.com/trailer.m4a"]);
    // the episode at exactly `since` isn't new
    expect(feedPage(feed, { since: String(utc("2024-10-01T10:00:00Z")) }).episodes).toEqual([]);
    expect(feedPage(feed, { since: "0" }).episodes).toHaveLength(3);
    expect(feedPage(feed, { since: "soon" }).episodes).toHaveLength(3); // not a number: left out
  });
});

describe("Podcast Index", () => {
  const key = { key: "UXKCGDSYGUUEVQJSYDZH", secret: "yzJe2eE7XV-3eY576dyRZ6wXyAbndh6LUrCZ8KN|" };

  it("signs a request as Podcast Index asks", () => {
    const h = indexHeaders(key, 1732500000.7);
    expect(h["X-Auth-Key"]).toBe(key.key);
    expect(h["X-Auth-Date"]).toBe("1732500000");
    expect(h.Authorization).toBe(createHash("sha1").update(`${key.key}${key.secret}1732500000`).digest("hex"));
    expect(h.Authorization).toMatch(/^[0-9a-f]{40}$/);
    expect(h["User-Agent"]).toMatch(/^Rondo server/);
  });

  it("answers its shows in the Rondo's shape", async () => {
    const asked: Array<{ url: string; headers: Record<string, string> }> = [];
    const get: IndexGet = async (url, headers) => {
      asked.push({ url, headers });
      return {
        status: 200,
        body: {
          status: "true",
          feeds: [
            { id: 920666, title: "Podcasting 2.0", url: "https://mp3s.nashownotes.com/pc20rss.xml", author: "Podcast Index LLC", ownerName: "x", image: "https://i.example/i.jpg", artwork: "https://i.example/a.jpg", itunesId: 1584274529, episodeCount: 210, language: "en-US", explicit: false },
            { id: 75075, title: "No art", url: "https://feeds.example/2", author: "", ownerName: "Owner", image: "https://i.example/2.jpg", artwork: "", itunesId: null, episodeCount: 5, language: "es", explicit: true },
            { id: 3, title: "Dead", url: "https://feeds.example/3", dead: 1 },
            { id: 4, title: "No feed", url: "" },
          ],
        },
      };
    };
    const index = new PodcastIndex(() => key, get, () => 1732500000_000);
    const res = await index.search("podcasting 2.0", 20);
    expect(res.items).toEqual([
      { feed: "https://mp3s.nashownotes.com/pc20rss.xml", title: "Podcasting 2.0", author: "Podcast Index LLC", art: "https://i.example/a.jpg", id: 920666, itunesId: 1584274529, count: 210, language: "en-us", explicit: false },
      { feed: "https://feeds.example/2", title: "No art", author: "Owner", art: "https://i.example/2.jpg", id: 75075, itunesId: null, count: 5, language: "es", explicit: true },
    ]);
    expect(asked[0]!.url).toBe("https://api.podcastindex.org/api/1.0/search/byterm?q=podcasting+2.0&max=20");
    expect(asked[0]!.headers.Authorization).toBe(indexHeaders(key, 1732500000).Authorization);
    await index.search("Podcasting 2.0", 20); // kept a while
    expect(asked).toHaveLength(1);
  });

  it("says when there's no key, a key it refuses, or no answer", async () => {
    expect(await new PodcastIndex(() => undefined).check()).toEqual({ ok: false, error: "no_key" });
    await expect(new PodcastIndex(() => undefined).search("x", 1)).rejects.toMatchObject({ code: "no_key" });
    expect(await new PodcastIndex(() => key, async () => ({ status: 401 })).check()).toEqual({ ok: false, error: "key", status: 401 });
    expect(await new PodcastIndex(() => key, async () => ({ status: 500 })).check()).toEqual({ ok: false, error: "upstream", status: 500 });
    expect(await new PodcastIndex(() => key, async () => Promise.reject(new Error("down"))).check()).toEqual({ ok: false, error: "upstream" });
    expect(await new PodcastIndex(() => key, async () => ({ status: 200, body: { feeds: [] } })).check()).toMatchObject({ ok: true });
  });
});

describe("RSS podcast routes", () => {
  let app: FastifyInstance;
  let feedFails: FeedError | undefined;
  let indexAnswer: { status: number; body?: unknown } = { status: 200, body: { feeds: [] } };
  const feeds: Record<string, string> = { "https://feeds.example.com/show.xml": fixture("show.xml"), "https://feeds.example.com/page.html": fixture("page.html") };
  const auth = { authorization: `Bearer ${TOKEN}` };
  let client = 0;
  const get = (url: string, headers: Record<string, string> = auth) => app.inject({ url, headers, remoteAddress: `10.9.${Math.floor(++client / 250)}.${client % 250}` });
  const q = (url: string) => encodeURIComponent(url);

  beforeAll(async () => {
    const dir = mkdtempSync(join(tmpdir(), "rondo-rss-"));
    const config = loadConfig({ API_TOKEN: TOKEN, YTDLP_BIN: "/nonexistent", LOG_LEVEL: "silent", COOKIES_FILE: "/nonexistent/cookies.txt", CONFIG_FILE: join(dir, "config.json") });
    ({ app } = await buildApp(config, {
      newCode: () => "482913",
      rss: {
        source: async (url) => {
          if (feedFails) throw feedFails;
          const text = feeds[url];
          if (text === undefined) throw new FeedError("upstream", 404);
          return { notModified: false, text, feedUrl: url };
        },
        indexGet: async () => indexAnswer,
      },
    }));
    await app.ready();
  });
  afterAll(async () => {
    await app.close();
  });

  it("needs the device token", async () => {
    expect((await get(`/podcasts/feed?url=${q("https://feeds.example.com/show.xml")}`, {})).statusCode).toBe(401);
    expect((await get("/podcasts/index/search?q=news", {})).statusCode).toBe(401);
  });

  it("answers a feed's page", async () => {
    const res = await get(`/podcasts/feed?url=${q("https://feeds.example.com/show.xml")}&offset=1&limit=1`);
    expect(res.statusCode).toBe(200);
    const body = res.json();
    expect(body).toMatchObject({ feed: "https://feeds.example.com/show.xml", title: "The Example & Friends Show", count: 3, offset: 1, next: 2 });
    expect(body.episodes).toHaveLength(1);
    expect(body.episodes[0]).toMatchObject({ title: "Trailer", notes: "Line one\nLine two" });
    const recent = (await get(`/podcasts/feed?url=${q("https://feeds.example.com/show.xml")}&since=${utc("2024-09-30T12:00:00Z")}`)).json();
    expect(recent).toMatchObject({ count: 3, next: null });
    expect(recent.episodes.map((e: { guid: string }) => e.guid)).toEqual(["ep-3", "https://cdn.example.com/trailer.m4a"]);
  });

  it.each(["", "url=", "url=feeds.example.com/x", `url=${q("ftp://feeds.example.com/x")}`, `url=${q("javascript:alert(1)")}`, `url=${q(`https://x.example/${"a".repeat(5000)}`)}`])(
    "refuses a bad address: %s",
    async (query) => {
      const res = await get(`/podcasts/feed?${query}`);
      expect(res.statusCode).toBe(400);
      expect(res.json()).toEqual({ error: "url" });
    },
  );

  it("says why a feed couldn't be read", async () => {
    expect((await get(`/podcasts/feed?url=${q("https://feeds.example.com/page.html")}`)).json()).toEqual({ error: "not_feed" });
    const missing = await get(`/podcasts/feed?url=${q("https://feeds.example.com/missing.xml")}`);
    expect([missing.statusCode, missing.json()]).toEqual([502, { error: "upstream", status: 404 }]);
    for (const [err, status, body] of [
      [new FeedError("url"), 400, { error: "url" }],
      [new FeedError("timeout"), 504, { error: "timeout" }],
      [new FeedError("too_big"), 413, { error: "too_big" }],
      [new FeedError("upstream"), 502, { error: "upstream", status: 0 }],
    ] as const) {
      feedFails = err;
      const res = await get(`/podcasts/feed?url=${q(`https://feeds.example.com/${err.code}.xml`)}`);
      expect([res.statusCode, res.json()]).toEqual([status, body]);
    }
    feedFails = undefined;
  });

  it("refuses the server's own network through the route too", async () => {
    const { app: real } = await buildApp(loadConfig({ API_TOKEN: TOKEN, YTDLP_BIN: "/nonexistent", LOG_LEVEL: "silent", COOKIES_FILE: "/nonexistent/cookies.txt", CONFIG_FILE: join(mkdtempSync(join(tmpdir(), "rondo-rss-")), "config.json") }));
    for (const url of ["http://127.0.0.1:3000/admin/api/status", "http://localhost/", "http://169.254.169.254/latest/meta-data/", "http://[fd00::1]/feed", "http://192.168.1.1/"]) {
      const res = await real.inject({ url: `/podcasts/feed?url=${q(url)}`, headers: auth });
      expect([res.statusCode, res.json()], url).toEqual([400, { error: "url" }]);
    }
    await real.close();
  });

  it("searches Podcast Index with the server's key", async () => {
    expect((await get("/podcasts/index/search")).json()).toEqual({ error: "query" });
    expect((await get("/podcasts/index/search?q=%20%20")).statusCode).toBe(400);
    const none = await get("/podcasts/index/search?q=news");
    expect([none.statusCode, none.json()]).toEqual([503, { error: "no_key" }]);

    // the owner pastes a key on the admin page
    const setup = await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: "correct horse battery" } });
    const cookie = String(setup.headers["set-cookie"]).split(";")[0]!;
    const bad = await app.inject({ method: "POST", url: "/admin/api/podcasts/key", headers: { cookie }, payload: { key: "has spaces in it", secret: "x".repeat(40) } });
    expect([bad.statusCode, bad.json()]).toEqual([400, { error: "key" }]);
    const saved = await app.inject({ method: "POST", url: "/admin/api/podcasts/key", headers: { cookie }, payload: { key: " UXKCGDSYGUUEVQJSYDZH ", secret: "yzJe2eE7XV-3eY576dyRZ6wXyAbndh6LUrCZ8KN|" } });
    expect(saved.statusCode).toBe(200);
    expect(saved.json()).toMatchObject({ ok: true, podcastIndex: { set: true, key: "UXKC…" } });
    const status = await app.inject({ url: "/admin/api/status", headers: { cookie } });
    expect(status.json().podcastIndex).toMatchObject({ set: true, key: "UXKC…" });
    expect(status.body).not.toContain("yzJe2eE7XV");
    expect(status.body).not.toContain("UXKCGDSYGUUEVQJSYDZH");
    expect((await app.inject({ method: "POST", url: "/admin/api/podcasts/check", headers: { cookie } })).json()).toMatchObject({ ok: true });

    indexAnswer = { status: 200, body: { feeds: [{ id: 1, title: "News Hour", url: "https://news.example/feed", episodeCount: 9, itunesId: 0 }] } };
    const found = await get("/podcasts/index/search?q=news&max=5");
    expect(found.statusCode).toBe(200);
    expect(found.json()).toEqual({ items: [{ feed: "https://news.example/feed", title: "News Hour", author: "", art: "", id: 1, itunesId: null, count: 9, language: "", explicit: false }] });

    indexAnswer = { status: 401 };
    expect((await app.inject({ method: "POST", url: "/admin/api/podcasts/check", headers: { cookie } })).json()).toEqual({ ok: false, error: "key", status: 401 });
    const refused = await get("/podcasts/index/search?q=something+else");
    expect([refused.statusCode, refused.json()]).toEqual([502, { error: "upstream" }]);

    expect((await app.inject({ method: "DELETE", url: "/admin/api/podcasts/key", headers: { cookie } })).json()).toEqual({ ok: true });
    expect((await app.inject({ url: "/admin/api/status", headers: { cookie } })).json().podcastIndex).toEqual({ set: false, key: null, setAt: null });
    expect((await get("/podcasts/index/search?q=news")).statusCode).toBe(503);
    // the key routes are the admin's
    expect((await app.inject({ method: "POST", url: "/admin/api/podcasts/key", payload: { key: "x", secret: "y" } })).statusCode).toBe(401);
  });
});
