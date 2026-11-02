import { describe, expect, it } from "vitest";
import Fastify from "fastify";
import { isNewer, registerUpdateRoutes, RONDO_RELEASES, Updates, YTDLP_RELEASES } from "../src/updates.js";

describe("isNewer", () => {
  it("compares versions part by part", () => {
    expect(isNewer("1.0.10", "1.0.9")).toBe(true);
    expect(isNewer("1.1.0", "1.0.0")).toBe(true);
    expect(isNewer("1.0.0", "1.0.0")).toBe(false);
    expect(isNewer("1.0.0", "1.1.0")).toBe(false);
  });
  it("compares yt-dlp's dated builds", () => {
    expect(isNewer("2026.11.01.233542", "2026.10.29.232701")).toBe(true);
    expect(isNewer("2026.10.29.232701", "2026.10.29.232701")).toBe(false);
  });
  it("says no when either side is unknown", () => {
    expect(isNewer(undefined, "1.0.0")).toBe(false);
    expect(isNewer("1.0.0", null)).toBe(false);
  });
});

describe("Updates", () => {
  const release = (tag: string) => ({ tag_name: tag, html_url: `https://github.com/x/releases/tag/${tag}`, published_at: "2026-11-02T10:00:00Z" });

  it("asks GitHub for both releases and keeps the answer", async () => {
    const asked: string[] = [];
    const u = new Updates(async (url) => {
      asked.push(url);
      return url === RONDO_RELEASES ? release("v1.0.1") : release("2026.11.01.233542");
    });
    await u.check();
    expect(asked.sort()).toEqual([RONDO_RELEASES, YTDLP_RELEASES].sort());
    expect(u.get()).toMatchObject({ rondo: { version: "1.0.1", publishedAt: Date.parse("2026-11-02T10:00:00Z") }, ytdlp: { version: "2026.11.01.233542" } });
  });

  it("keeps the last answer when GitHub fails", async () => {
    let fail = false;
    const u = new Updates(async () => {
      if (fail) throw new Error("rate limited");
      return release("v1.0.1");
    });
    await u.check();
    fail = true;
    await u.check();
    expect(u.get().rondo?.version).toBe("1.0.1");
  });

  it("never asks when turned off", async () => {
    const u = new Updates(null);
    await u.check();
    expect(u.get()).toEqual({ rondo: null, ytdlp: null, checkedAt: null });
  });
});

describe("update routes", () => {
  const withApk = {
    tag_name: "v1.0.1",
    html_url: "https://github.com/jarvissax/rondo/releases/tag/v1.0.1",
    published_at: "2026-11-02T10:00:00Z",
    assets: [
      { name: "rom.zip", browser_download_url: "https://github.com/x/rom.zip", size: 537000000 },
      { name: "rondo-1.0.1.apk", browser_download_url: "https://github.com/x/rondo-1.0.1.apk", size: 6413645 },
    ],
  };
  const appWith = async (updates?: Updates) => {
    const app = Fastify();
    registerUpdateRoutes(app, updates);
    await app.ready();
    return app;
  };

  it("names the newest Rondo with an update APK, checking GitHub first if it hasn't yet", async () => {
    const app = await appWith(new Updates(async () => withApk));
    const res = await app.inject({ url: "/update" });
    expect(res.json()).toEqual({ version: "1.0.1", size: 6413645, notes: "https://github.com/jarvissax/rondo/releases/tag/v1.0.1" });
  });

  it("says there's nothing when the release has no APK, or nobody asks GitHub", async () => {
    const noApk = await appWith(new Updates(async () => ({ ...withApk, assets: [withApk.assets[0]] })));
    expect((await noApk.inject({ url: "/update" })).json()).toEqual({ version: null });
    const off = await appWith(undefined);
    expect((await off.inject({ url: "/update" })).json()).toEqual({ version: null });
    expect((await off.inject({ url: "/update/rondo.apk" })).statusCode).toBe(404);
  });
});
