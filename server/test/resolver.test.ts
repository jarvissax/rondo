import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { loadConfig } from "../src/config.js";
import { InvalidVideoId, parseYtdlp, Resolver } from "../src/resolver.js";

const fixture = JSON.parse(readFileSync(new URL("./fixtures/ytdlp-dQw4w9WgXcQ.json", import.meta.url), "utf8"));
const config = loadConfig({ API_TOKEN: "t".repeat(64), COOKIES_FILE: "/nonexistent" });

describe("parseYtdlp", () => {
  it("keeps 134, 135, 136, 140 and the progressive 18 from the plain-https formats", () => {
    const v = parseYtdlp("dQw4w9WgXcQ", fixture);
    expect([...v.formats.keys()].sort((a, b) => a - b)).toEqual([18, 134, 135, 136, 140]);
    expect(v.formats.get(18)).toMatchObject({ mimeType: "video/mp4", codecs: "avc1.42001E,mp4a.40.2" });
    expect(v.duration).toBe(213);
  });

  it("maps codecs, bitrate, size, length and expiry", () => {
    const v = parseYtdlp("dQw4w9WgXcQ", fixture);
    const f136 = v.formats.get(136)!;
    expect(f136).toMatchObject({ mimeType: "video/mp4", codecs: "avc1.4D401F", width: 1280, height: 720 });
    expect(f136.bitrate).toBe(993461);
    expect(f136.contentLength).toBe(26455880);
    expect(f136.expiresAt).toBe(1793370585 * 1000);
    expect(f136.httpHeaders["User-Agent"]).toMatch(/Mozilla/);
    expect(v.formats.get(140)).toMatchObject({ mimeType: "audio/mp4", codecs: "mp4a.40.2", audioSamplingRate: 44100 });
  });

  it("falls back to 18 when no DASH formats exist", () => {
    const only18 = { ...fixture, formats: fixture.formats.filter((f: { format_id: string }) => f.format_id === "18") };
    expect([...parseYtdlp("dQw4w9WgXcQ", only18).formats.keys()]).toEqual([18]);
  });

  it("takes a dubbed video's original audio track, not a dub or the DRC copy", () => {
    const f140 = fixture.formats.find((f: { format_id: string }) => f.format_id === "140");
    const track = (format_id: string, language_preference: number, n: string) => ({ ...f140, format_id, language_preference, url: f140.url + "&n=" + n });
    const dubbed = { ...fixture, formats: [...fixture.formats.filter((f: { format_id: string }) => f.format_id !== "140"),
      track("140-drc", 10, "drc"), track("140-0", -1, "de"), track("140-9", 10, "original"), track("140-1", -1, "es")] };
    expect(parseYtdlp("dQw4w9WgXcQ", dubbed).formats.get(140)?.url).toMatch(/n=original$/);
    const onlyDrc = { ...fixture, formats: [...fixture.formats.filter((f: { format_id: string }) => f.format_id !== "140"), track("140-drc", 10, "drc")] };
    expect(parseYtdlp("dQw4w9WgXcQ", onlyDrc).formats.get(140)?.url).toMatch(/n=drc$/);
  });

  it("throws when nothing usable is there", () => {
    expect(() => parseYtdlp("dQw4w9WgXcQ", { formats: [] })).toThrow();
  });
});

describe("Resolver", () => {
  it("rejects ids that are not 11 YouTube characters", async () => {
    const r = new Resolver(config, async () => JSON.stringify(fixture));
    await expect(r.resolve("../etc/passwd")).rejects.toBeInstanceOf(InvalidVideoId);
  });

  it("runs yt-dlp once for concurrent requests and caches the result", async () => {
    let calls = 0;
    const r = new Resolver(config, async (args) => {
      calls++;
      expect(args).toContain("-J");
      expect(args.at(-1)).toBe("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
      await new Promise((ok) => setTimeout(ok, 20));
      // URLs that expire in an hour, so the cache entry is fresh
      const expire = Math.floor(Date.now() / 1000) + 3600;
      return JSON.stringify(fixture).replace(/expire=\d+/g, `expire=${expire}`);
    });
    await Promise.all([r.resolve("dQw4w9WgXcQ"), r.resolve("dQw4w9WgXcQ")]);
    await r.resolve("dQw4w9WgXcQ");
    expect(calls).toBe(1);
    r.invalidate("dQw4w9WgXcQ");
    await r.resolve("dQw4w9WgXcQ");
    expect(calls).toBe(2);
  });

  it("re-resolves when the URLs are within 10 minutes of expiry", async () => {
    let calls = 0;
    const r = new Resolver(config, async () => {
      calls++;
      const expire = Math.floor(Date.now() / 1000) + 5 * 60;
      return JSON.stringify(fixture).replace(/expire=\d+/g, `expire=${expire}`);
    });
    await r.resolve("dQw4w9WgXcQ");
    await r.resolve("dQw4w9WgXcQ");
    expect(calls).toBe(2);
  });
});
