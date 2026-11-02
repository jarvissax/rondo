import { describe, expect, it } from "vitest";
import { isNewer, RONDO_RELEASES, Updates, YTDLP_RELEASES } from "../src/updates.js";

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
