import { describe, expect, it } from "vitest";
import Fastify from "fastify";
import { isNewer, registerUpdateRoutes, RONDO_LATEST, Updates, YTDLP_RELEASES } from "../src/updates.js";

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

// the update page's latest.json (.github/workflows/release.yml)
const latest = (version: string, more: Record<string, unknown> = {}) => ({
  version,
  file: "rondo.apk",
  size: 6413645,
  firmware: 1,
  notes: `https://github.com/jarvissax/rondo/releases/tag/v${version}`,
  published: "2026-11-02T10:00:00Z",
  ...more,
});

describe("Updates", () => {
  const release = (tag: string) => ({ tag_name: tag, html_url: `https://github.com/x/releases/tag/${tag}`, published_at: "2026-11-02T10:00:00Z" });

  it("asks the update page for Rondo and GitHub for yt-dlp, and keeps the answers", async () => {
    const asked: string[] = [];
    const u = new Updates(async (url) => {
      asked.push(url);
      return url === RONDO_LATEST ? latest("1.0.1") : release("2026.11.01.233542");
    });
    await u.check();
    expect(asked.sort()).toEqual([RONDO_LATEST, YTDLP_RELEASES].sort());
    expect(u.get()).toMatchObject({
      rondo: { version: "1.0.1", firmware: 1, publishedAt: Date.parse("2026-11-02T10:00:00Z"), apk: { url: "https://jarvissax.github.io/rondo/rondo.apk", size: 6413645 } },
      ytdlp: { version: "2026.11.01.233542" },
    });
  });

  it("reads a latest.json from before firmware numbers as the first firmware", async () => {
    const u = new Updates(async (url) => (url === RONDO_LATEST ? { version: "1.0.0", file: "rondo.apk", size: 6421197 } : null));
    await u.check();
    expect(u.get().rondo).toMatchObject({ version: "1.0.0", firmware: 1, url: "https://github.com/jarvissax/rondo/releases/tag/v1.0.0" });
  });

  it("keeps the last answer when a check fails", async () => {
    let fail = false;
    const u = new Updates(async () => {
      if (fail) throw new Error("rate limited");
      return latest("1.0.1");
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
  const withApk = latest("1.0.1");
  const appWith = async (updates?: Updates) => {
    const app = Fastify();
    registerUpdateRoutes(app, updates);
    await app.ready();
    return app;
  };

  it("names the newest Rondo with an update APK, checking first if it hasn't yet", async () => {
    const app = await appWith(new Updates(async () => withApk));
    const res = await app.inject({ url: "/update?firmware=1" });
    expect(res.json()).toEqual({ version: "1.0.1", size: 6413645, notes: "https://github.com/jarvissax/rondo/releases/tag/v1.0.1", firmware: 1 });
  });

  it("names a release for a newer firmware with its number, so the Rondo can point to Innioasis Updater CE", async () => {
    const app = await appWith(new Updates(async () => latest("2.0.0", { firmware: 2 })));
    expect((await app.inject({ url: "/update?firmware=1" })).json()).toMatchObject({ version: "2.0.0", firmware: 2 });
    // firmware 2 is Rondo 2.0, which installs bundles only: an APK is nothing for it
    expect((await app.inject({ url: "/update?firmware=2" })).json()).toEqual({ version: null });
  });

  it("keeps a Rondo from before firmware numbers (1.0.0) to first-firmware releases", async () => {
    const first = await appWith(new Updates(async () => withApk));
    expect((await first.inject({ url: "/update" })).json()).toMatchObject({ version: "1.0.1" });
    const second = await appWith(new Updates(async () => latest("2.0.0", { firmware: 2 })));
    expect((await second.inject({ url: "/update" })).json()).toEqual({ version: null });
  });

  it("names a 2.0 release's bundle to Rondo 2.0 (firmware 2), and tells 1.0.x it needs the new firmware", async () => {
    const two = { version: "2.0.0", file: "rondo.rondo", size: 31000000, bundle: "rondo.rondo", bundleSize: 31000000, firmware: 2 };
    const app = await appWith(new Updates(async () => two));
    expect((await app.inject({ url: "/update?firmware=2" })).json()).toEqual({
      version: "2.0.0",
      size: 31000000,
      notes: "https://github.com/jarvissax/rondo/releases/tag/v2.0.0",
      firmware: 2,
    });
    // 1.0.x (firmware 1, no APK in the release): told it is out and needs the firmware
    expect((await app.inject({ url: "/update?firmware=1" })).json()).toMatchObject({ version: "2.0.0", size: 0, firmware: 2 });
    expect((await app.inject({ url: "/update/rondo.apk" })).statusCode).toBe(404);
  });

  it("says there's nothing when the release has no APK, or nobody asks", async () => {
    const noApk = await appWith(new Updates(async () => ({ version: "1.0.1" })));
    expect((await noApk.inject({ url: "/update" })).json()).toEqual({ version: null });
    const off = await appWith(undefined);
    expect((await off.inject({ url: "/update" })).json()).toEqual({ version: null });
    expect((await off.inject({ url: "/update/rondo.apk" })).statusCode).toBe(404);
    expect((await off.inject({ url: "/update/rondo.rondo" })).statusCode).toBe(404);
  });
});
