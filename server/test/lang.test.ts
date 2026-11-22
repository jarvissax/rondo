// The languages (lang/*.json): the checker passes (tools/lang/check.mjs, also run by CI on its own),
// and the admin page's strings come in the language asked for, with English where it lacks one.
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { currentLang, langFetch, langFromHeader, languages, pageStrings, pickLanguage, say, withLang } from "../src/lang.js";

describe("lang/", () => {
  it("passes tools/lang/check.mjs", async () => {
    // @ts-expect-error: a plain Node script, without types
    const { check } = await import("../../tools/lang/check.mjs");
    const { errors } = check() as { errors: string[] };
    expect(errors).toEqual([]);
  });

  const dir = mkdtempSync(join(tmpdir(), "rondo-lang-"));
  writeFileSync(join(dir, "en.json"), JSON.stringify({ _name: "English", device: {}, dashboard: {}, admin: { "a.hi": "Hi", "a.songs": { one: "{count} song", other: "{count} songs" }, "a.only": "Only English" } }));
  writeFileSync(join(dir, "es.json"), JSON.stringify({ _name: "Español", device: {}, dashboard: {}, admin: { "a.hi": "Hola", "a.songs": { one: "{count} canción", other: "{count} canciones" } } }));

  it("lists the languages, English first", () => {
    expect(languages(dir)).toEqual([{ code: "en", name: "English" }, { code: "es", name: "Español" }]);
  });

  it("picks the nearest language a browser asks for", () => {
    expect(pickLanguage("es-MX,es;q=0.9,en;q=0.8", dir)).toBe("es");
    expect(pickLanguage("fr-FR,fr", dir)).toBe("en");
    expect(pickLanguage("de,es", dir)).toBe("es");
    expect(pickLanguage("../en", dir)).toBe("en");
  });

  it("fills in English where a language has no text", () => {
    const p = pageStrings("admin", "es", dir);
    expect(p.code).toBe("es");
    expect(p.name).toBe("Español");
    expect(p.strings).toEqual({ "a.hi": "Hola", "a.songs": { one: "{count} canción", other: "{count} canciones" }, "a.only": "Only English" });
  });
});

describe("a Rondo's language", () => {
  it("reads Accept-Language", () => {
    expect(langFromHeader("es")).toBe("es");
    expect(langFromHeader("pt-BR,pt;q=0.9")).toBe("pt-BR");
    expect(langFromHeader(undefined)).toBe("en");
    expect(langFromHeader("../../etc")).toBe("en");
  });

  it("says the server's own words in it, English where it has none", () => {
    expect(say("feed.songCount", { count: 12 })).toBe("12 songs");
    expect(say("feed.songCount", { count: 1 })).toBe("1 song");
    withLang("es", () => {
      expect(currentLang()).toBe("es");
      expect(say("feed.songCount", { count: 12 })).toBe("12 canciones");
      expect(say("feed.views", { views: "2,3 M" })).toBe("2,3 M visualizaciones");
    });
    withLang("es-MX", () => expect(say("feed.likedMusic")).toBe("Música que te gusta"));
    withLang("xx", () => expect(say("feed.mix")).toBe("Mix"));
    expect(say("no.such.key")).toBe("no.such.key");
  });

  it("asks YouTube in it (the context's hl, per request)", async () => {
    const seen: string[] = [];
    const real = globalThis.fetch;
    globalThis.fetch = (async (_input: unknown, init?: RequestInit) => {
      seen.push(JSON.parse(String(init?.body)).context.client.hl);
      return new Response("{}");
    }) as typeof fetch;
    try {
      const body = JSON.stringify({ context: { client: { hl: "en", clientName: "WEB" } }, browseId: "FEwhat_to_watch" });
      await langFetch("https://www.youtube.com/youtubei/v1/browse", { method: "POST", body });
      await withLang("es", () => langFetch("https://www.youtube.com/youtubei/v1/browse", { method: "POST", body }));
    } finally {
      globalThis.fetch = real;
    }
    expect(seen).toEqual(["en", "es"]);
  });
});
