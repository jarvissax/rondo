import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { renderMpd } from "../src/mpd.js";
import { parseYtdlp } from "../src/resolver.js";

const fixture = JSON.parse(readFileSync(new URL("./fixtures/ytdlp-dQw4w9WgXcQ.json", import.meta.url), "utf8"));
const video = parseYtdlp("dQw4w9WgXcQ", fixture);
const ranges = new Map([
  [136, { initRange: "0-740", indexRange: "741-1316" }],
  [135, { initRange: "0-739", indexRange: "740-1315" }],
  [134, { initRange: "0-738", indexRange: "739-1314" }],
  [140, { initRange: "0-631", indexRange: "632-927" }],
]);

describe("renderMpd", () => {
  const xml = renderMpd({ video, ranges, publicBase: "https://music.example.com" });

  it("is a static on-demand MPD with the video duration", () => {
    expect(xml).toContain('type="static"');
    expect(xml).toContain("urn:mpeg:dash:profile:isoff-on-demand:2011");
    expect(xml).toContain('mediaPresentationDuration="PT213S"');
  });

  it("lists video 136, 135, 134 in that order, then audio 140", () => {
    const ids = [...xml.matchAll(/Representation id="(\d+)"/g)].map((m) => m[1]);
    expect(ids).toEqual(["136", "135", "134", "140"]);
  });

  it("fills codecs, sizes, bandwidth, BaseURL and segment ranges", () => {
    expect(xml).toContain('<Representation id="136" codecs="avc1.4D401F" width="1280" height="720" bandwidth="993461">');
    expect(xml).toContain("<BaseURL>https://music.example.com/s/dQw4w9WgXcQ/136</BaseURL>");
    expect(xml).toContain('<SegmentBase indexRange="741-1316"><Initialization range="0-740"/></SegmentBase>');
    expect(xml).toContain('codecs="mp4a.40.2" audioSamplingRate="44100"');
  });

  it("skips a rendition whose ranges are unknown", () => {
    const partial = new Map(ranges);
    partial.delete(135);
    const ids = [...renderMpd({ video, ranges: partial, publicBase: "https://x" }).matchAll(/Representation id="(\d+)"/g)];
    expect(ids.map((m) => m[1])).toEqual(["136", "134", "140"]);
  });

  it("puts the query token on BaseURLs only when given, escaped", () => {
    expect(xml).not.toContain("token=");
    const withToken = renderMpd({ video, ranges, publicBase: "https://x", token: "a&b" });
    expect(withToken).toContain("<BaseURL>https://x/s/dQw4w9WgXcQ/140?token=a%26b</BaseURL>");
  });

  it("refuses to build without video or audio", () => {
    expect(() => renderMpd({ video, ranges: new Map([[140, ranges.get(140)!]]), publicBase: "https://x" })).toThrow();
  });
});
