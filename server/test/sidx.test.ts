import { describe, expect, it } from "vitest";
import { probeRanges, SidxCache, type ByteFetcher } from "../src/sidx.js";
import { box, concat, dashFile } from "./mp4.js";

function fetcherFor(file: Uint8Array, log: Array<[number, number]> = []): ByteFetcher {
  return async (start, end) => {
    log.push([start, end]);
    return file.slice(start, end + 1);
  };
}

describe("probeRanges", () => {
  it("finds init and index ranges in one fetch", async () => {
    const log: Array<[number, number]> = [];
    const r = await probeRanges(fetcherFor(dashFile(2000), log));
    expect(r).toEqual({ initRange: "0-2023", indexRange: "2024-2123" });
    expect(log).toEqual([[0, 65535]]);
  });

  it("fetches again when moov is larger than the first 64 KB", async () => {
    const log: Array<[number, number]> = [];
    const r = await probeRanges(fetcherFor(dashFile(100_000), log));
    expect(r).toEqual({ initRange: "0-100023", indexRange: "100024-100123" });
    expect(log.length).toBe(2);
    expect(log[1]![0]).toBe(100_024);
  });

  it("reads 64-bit box sizes", async () => {
    const file = concat(box("ftyp", 24), box("moov", 3000, true), box("sidx", 80), box("mdat", 100));
    expect(await probeRanges(fetcherFor(file))).toEqual({ initRange: "0-3023", indexRange: "3024-3103" });
  });

  it("fails when media starts without an index", async () => {
    const file = concat(box("ftyp", 24), box("moov", 500), box("moof", 100), box("mdat", 100));
    await expect(probeRanges(fetcherFor(file))).rejects.toThrow(/no sidx/);
  });
});

describe("SidxCache", () => {
  it("probes once per key", async () => {
    const log: Array<[number, number]> = [];
    const cache = new SidxCache();
    const f = fetcherFor(dashFile(), log);
    await Promise.all([cache.get("a:140", f), cache.get("a:140", f)]);
    await cache.get("a:140", f);
    expect(log.length).toBe(1);
  });
});
