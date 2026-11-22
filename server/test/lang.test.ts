// The languages (lang/*.json): the checker passes (tools/lang/check.mjs, also run by CI on its own),
// and the admin page's strings come in the language asked for, with English where it lacks one.
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { describe, expect, it } from "vitest";
import { languages, pageStrings, pickLanguage } from "../src/lang.js";

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
