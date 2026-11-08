import { describe, expect, it } from "vitest";
import { EqService, parseIndex, parseParametric, search } from "../src/eq.js";

const INDEX = [
  "# Index",
  "This is a list of all equalization profiles. Target is in parentheses if there are results with multiple targets",
  "from the same source.",
  "",
  "- [1MORE Quad Driver](./oratory1990/in-ear/1MORE%20Quad%20Driver) by oratory1990",
  "- [1MORE Quad Driver](./Rtings/HMS%20II.3%20in-ear/1MORE%20Quad%20Driver) by Rtings on HMS II.3",
  "- [JBL Tune Flex](./Rtings/HMS%20II.3%20in-ear/JBL%20Tune%20Flex) by Rtings on HMS II.3",
  "- [JBL Tune 760NC](./Rtings/HMS%20II.3%20over-ear/JBL%20Tune%20760NC) by Rtings on HMS II.3",
  "- [Sennheiser HD 650](./oratory1990/over-ear/Sennheiser%20HD%20650) by oratory1990",
  "- [Sennheiser HD 650](./crinacle/GRAS%2043AG-7%20over-ear/Sennheiser%20HD%20650) by crinacle on GRAS 43AG-7",
].join("\n");

const PARAMETRIC = [
  "Preamp: -6.6 dB",
  "Filter 1: ON LSC Fc 105 Hz Gain 8.2 dB Q 0.70",
  "Filter 2: ON PK Fc 105 Hz Gain -6.9 dB Q 0.37",
  "Filter 3: OFF PK Fc 8263 Hz Gain 6.3 dB Q 1.29",
  "Filter 6: ON HSC Fc 10000 Hz Gain -6.4 dB Q 0.70",
].join("\n");

describe("parseIndex", () => {
  it("reads every entry with its decoded folder and its source", () => {
    const e = parseIndex(INDEX);
    expect(e).toHaveLength(6);
    expect(e[0]).toEqual({ id: "oratory1990/in-ear/1MORE Quad Driver", name: "1MORE Quad Driver", source: "oratory1990" });
    expect(e[1]!.source).toBe("Rtings on HMS II.3");
  });
});

describe("parseParametric", () => {
  it("reads the preamp and the filters that are on", () => {
    expect(parseParametric(PARAMETRIC)).toEqual({
      preampDb: -6.6,
      filters: [
        { type: "LSC", freq: 105, gainDb: 8.2, q: 0.7 },
        { type: "PK", freq: 105, gainDb: -6.9, q: 0.37 },
        { type: "HSC", freq: 10000, gainDb: -6.4, q: 0.7 },
      ],
    });
  });
});

describe("search", () => {
  const entries = parseIndex(INDEX);
  it("finds a model from a Bluetooth name, whole words first", () => {
    expect(search(entries, "JBL TUNE FLEX")[0]!.name).toBe("JBL Tune Flex");
  });
  it("ranks oratory1990 before crinacle for the same model", () => {
    const r = search(entries, "hd 650");
    expect(r.map((e) => e.source)).toEqual(["oratory1990", "crinacle on GRAS 43AG-7"]);
  });
  it("needs every word", () => {
    expect(search(entries, "jbl 650")).toEqual([]);
    expect(search(entries, "")).toEqual([]);
  });
});

describe("EqService", () => {
  it("fetches the index once and a model's file by its folder, spaces encoded", async () => {
    const urls: string[] = [];
    const eq = new EqService(async (url) => {
      urls.push(url);
      return url.endsWith("INDEX.md") ? INDEX : PARAMETRIC;
    });
    expect((await eq.search("tune flex"))[0]!.id).toBe("Rtings/HMS II.3 in-ear/JBL Tune Flex");
    const p = await eq.preset("Rtings/HMS II.3 in-ear/JBL Tune Flex");
    expect(p?.preampDb).toBe(-6.6);
    expect(p?.filters).toHaveLength(3);
    expect(urls.filter((u) => u.endsWith("INDEX.md"))).toHaveLength(1);
    expect(urls.at(-1)).toBe(
      "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results/Rtings/HMS%20II.3%20in-ear/JBL%20Tune%20Flex/JBL%20Tune%20Flex%20ParametricEQ.txt",
    );
    expect(await eq.preset("not/in/the index")).toBeNull();
  });
});
