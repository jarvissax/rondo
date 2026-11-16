import { describe, expect, it } from "vitest";
import { TtlCache } from "../src/ttl.js";

describe("TtlCache", () => {
  it("serves a value until it expires, sharing one in-flight load", async () => {
    let now = 0;
    let loads = 0;
    const cache = new TtlCache<number>(1000, 10, () => now);
    const load = async () => ++loads;
    const [a, b] = await Promise.all([cache.get("k", load), cache.get("k", load)]);
    expect([a, b, loads]).toEqual([1, 1, 1]);
    now = 999;
    expect(await cache.get("k", load)).toBe(1);
    now = 1000;
    expect(await cache.get("k", load)).toBe(2);
  });

  it("does not keep failures", async () => {
    const cache = new TtlCache<string>(1000);
    await expect(cache.get("k", () => Promise.reject(new Error("boom")))).rejects.toThrow("boom");
    await Promise.resolve();
    expect(await cache.get("k", async () => "ok")).toBe("ok");
  });

  it("evicts the oldest entry beyond its size and can be cleared", async () => {
    let loads = 0;
    const cache = new TtlCache<number>(60_000, 2);
    const load = async () => ++loads;
    await cache.get("a", load);
    await cache.get("b", load);
    await cache.get("c", load); // evicts a
    expect(await cache.get("b", load)).toBe(2);
    expect(await cache.get("a", load)).toBe(4);
    cache.clear();
    expect(await cache.get("b", load)).toBe(5);
  });

  it("can choose the TTL per value once the load resolves", async () => {
    let now = 0;
    let loads = 0;
    const cache = new TtlCache<string>((v) => (v === "hit" ? 1000 : 100), 10, () => now);
    const load = (v: string) => async () => (loads++, v);
    await cache.get("a", load("hit"));
    await cache.get("b", load("miss"));
    now = 500;
    await cache.get("a", load("hit"));
    await cache.get("b", load("miss"));
    expect(loads).toBe(3); // b expired after 100 ms, a is good for 1000
  });

  it("shares a pending per-value load however long it takes", async () => {
    let now = 0;
    let release: (v: string) => void = () => {};
    const cache = new TtlCache<string>(() => 10, 10, () => now);
    const first = cache.get("k", () => new Promise<string>((ok) => (release = ok)));
    now = 1000;
    const second = cache.get("k", async () => "other");
    release("v");
    expect([await first, await second]).toEqual(["v", "v"]);
  });
});

describe("TtlCache.getStale", () => {
  it("serves an expired value while a fresh one loads, then the fresh one", async () => {
    let now = 0;
    const cache = new TtlCache<string>(100, 10, () => now);
    expect(await cache.getStale("home", async () => "v1", 1000)).toBe("v1");
    now = 150; // expired, within the stale window
    let release: (v: string) => void = () => {};
    let loads = 0;
    const slow = () => (loads++, new Promise<string>((ok) => (release = ok)));
    expect(await cache.getStale("home", slow, 1000)).toBe("v1");
    expect(await cache.getStale("home", slow, 1000)).toBe("v1"); // one reload at a time
    expect(loads).toBe(1);
    release("v2");
    await new Promise((ok) => setTimeout(ok, 0));
    expect(await cache.getStale("home", async () => "v3", 1000)).toBe("v2");
  });

  it("keeps the old value when the reload fails, and waits for a load past the stale window", async () => {
    let now = 0;
    const cache = new TtlCache<string>(100, 10, () => now);
    await cache.getStale("home", async () => "v1", 1000);
    now = 150;
    expect(await cache.getStale("home", async () => Promise.reject(new Error("offline")), 1000)).toBe("v1");
    await new Promise((ok) => setTimeout(ok, 0));
    expect(await cache.getStale("home", async () => "v2", 1000)).toBe("v1"); // tried again in the background
    now = 5000; // long past: the screen waits for a fresh page
    expect(await cache.getStale("home", async () => "v9", 1000)).toBe("v9");
  });
});
