// Headphone corrections for Rondo 2.0's Settings › Sound › Headphone match: AutoEq's parametric EQ
// for a headphone model (github.com/jaakkopasanen/AutoEq, its results folder: a preamp and about ten
// peaking and shelf filters that bring the model to the Harman target). The Y1 never talks to
// GitHub; this server fetches the index once a day and a model's file once a week (metadata only).
//
//   GET /eq/search?q=jbl tune   → { items: [{ id, name, source }] }   (at most 20, best first)
//   GET /eq/preset?id=<id>      → { id, name, source, preampDb, filters: [{ type, freq, gainDb, q }] }
//
// An id is the model's folder under results/ ("oratory1990/over-ear/Sennheiser HD 650").

import type { FastifyInstance } from "fastify";
import { request } from "undici";
import { TtlCache } from "./ttl.js";
import { dispatcher } from "./upstream.js";

const RAW = "https://raw.githubusercontent.com/jaakkopasanen/AutoEq/master/results";

export interface EqEntry {
  id: string; // the folder under results/, decoded
  name: string;
  source: string; // who measured it ("oratory1990", "crinacle on 711")
}

export interface EqFilter {
  type: "PK" | "LSC" | "HSC" | "LPQ" | "HPQ";
  freq: number;
  gainDb: number;
  q: number;
}

export interface EqPreset extends EqEntry {
  preampDb: number;
  filters: EqFilter[];
}

// INDEX.md's lines: "- [Name](./source/category/Name%20With%20Spaces) by source[ on rig]"
export function parseIndex(md: string): EqEntry[] {
  const out: EqEntry[] = [];
  for (const line of md.split("\n")) {
    const m = /^- \[(.+?)\]\(\.\/(.+)\) by (.+?)\s*$/.exec(line);
    if (!m) continue;
    let id: string;
    try {
      id = decodeURIComponent(m[2]!);
    } catch {
      continue;
    }
    out.push({ id, name: m[1]!, source: m[3]! });
  }
  return out;
}

// "Preamp: -6.6 dB" and "Filter 1: ON LSC Fc 105 Hz Gain 8.2 dB Q 0.70" (filters that are OFF are left out)
export function parseParametric(text: string): { preampDb: number; filters: EqFilter[] } {
  let preampDb = 0;
  const filters: EqFilter[] = [];
  for (const line of text.split(/\r?\n/)) {
    const p = /^Preamp:\s*(-?[\d.]+)\s*dB/i.exec(line);
    if (p) preampDb = Number(p[1]);
    const f = /^Filter\s+\d+:\s+ON\s+(PK|LSC|HSC|LPQ|HPQ|LS|HS)\s+Fc\s+([\d.]+)\s*Hz(?:\s+Gain\s+(-?[\d.]+)\s*dB)?(?:\s+Q\s+([\d.]+))?/i.exec(line);
    if (!f) continue;
    const kind = f[1]!.toUpperCase();
    const type = (kind === "LS" ? "LSC" : kind === "HS" ? "HSC" : kind) as EqFilter["type"];
    const freq = Number(f[2]), gainDb = f[3] === undefined ? 0 : Number(f[3]), q = f[4] === undefined ? 0.71 : Number(f[4]);
    if (!Number.isFinite(freq) || !Number.isFinite(gainDb) || !Number.isFinite(q) || freq <= 0) continue;
    filters.push({ type, freq, gainDb, q });
  }
  return { preampDb: Number.isFinite(preampDb) ? preampDb : 0, filters };
}

// The words typed, against a model's name: every word must be in it; whole-word and prefix matches
// rank first, and for one model the measurement AutoEq prefers (oratory1990, then crinacle) first.
export function search(entries: EqEntry[], q: string, limit = 20): EqEntry[] {
  const norm = (s: string) => s.toLowerCase().normalize("NFKD").replace(/[̀-ͯ]/g, "").replace(/[^a-z0-9]+/g, " ").trim();
  const words = norm(q).split(" ").filter(Boolean);
  if (!words.length) return [];
  const sourceRank = (s: string) => (s.startsWith("oratory1990") ? 0 : s.startsWith("crinacle") ? 1 : s.startsWith("Rtings") ? 2 : 3);
  const scored: { e: EqEntry; score: number }[] = [];
  for (const e of entries) {
    const name = norm(e.name), parts = name.split(" ");
    if (!words.every((w) => name.includes(w))) continue;
    let score = 0;
    for (const w of words) score += parts.includes(w) ? 0 : parts.some((p) => p.startsWith(w)) ? 1 : 3;
    score += Math.abs(parts.length - words.length) * 0.5 + sourceRank(e.source) * 0.25;
    scored.push({ e, score });
  }
  scored.sort((a, b) => a.score - b.score || a.e.name.localeCompare(b.e.name));
  return scored.slice(0, limit).map((s) => s.e);
}

export type TextGet = (url: string) => Promise<string>;

export const githubGet: TextGet = async (url) => {
  const res = await request(url, { dispatcher, headersTimeout: 15_000, bodyTimeout: 30_000 });
  if (res.statusCode !== 200) {
    await res.body.dump();
    throw new Error(`HTTP ${res.statusCode}`);
  }
  return res.body.text();
};

export class EqService {
  private readonly index = new TtlCache<EqEntry[]>(24 * 3600_000, 1);
  private readonly presets = new TtlCache<EqPreset | null>(7 * 24 * 3600_000, 500);
  constructor(private readonly get: TextGet = githubGet) {}

  entries(): Promise<EqEntry[]> {
    return this.index.get("index", async () => parseIndex(await this.get(`${RAW}/INDEX.md`)));
  }

  async search(q: string): Promise<EqEntry[]> {
    return search(await this.entries(), q);
  }

  async preset(id: string): Promise<EqPreset | null> {
    const entry = (await this.entries()).find((e) => e.id === id);
    if (!entry) return null;
    return this.presets.get(id, async () => {
      const name = id.split("/").pop()!;
      const path = id.split("/").map(encodeURIComponent).join("/");
      const text = await this.get(`${RAW}/${path}/${encodeURIComponent(`${name} ParametricEQ.txt`)}`);
      const p = parseParametric(text);
      return p.filters.length ? { ...entry, ...p } : null;
    });
  }
}

export function registerEqRoutes(app: FastifyInstance, eq: EqService): void {
  app.get<{ Querystring: { q?: string } }>("/eq/search", async (req, reply) => {
    const q = String(req.query.q ?? "").slice(0, 80);
    try {
      return { items: await eq.search(q) };
    } catch (e) {
      req.log.warn(`eq: the index didn't load: ${(e as Error).message}`);
      return reply.code(502).send({ error: "upstream" });
    }
  });
  app.get<{ Querystring: { id?: string } }>("/eq/preset", async (req, reply) => {
    const id = String(req.query.id ?? "");
    if (!id || id.includes("..") || id.length > 300) return reply.code(400).send({ error: "id" });
    try {
      const p = await eq.preset(id);
      return p ?? reply.code(404).send({ error: "not found" });
    } catch (e) {
      req.log.warn(`eq: a preset didn't load: ${(e as Error).message}`);
      return reply.code(502).send({ error: "upstream" });
    }
  });
}
