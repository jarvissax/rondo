// The Rondo's time zone. Rondo 2.0 has no Android to know it, so it asks its server once per Wi-Fi
// connection: the zone of the public address the request came from (Caddy's X-Forwarded-For), asked
// of ipwho.is, then of ipapi.co. A Rondo on the server's own network (a private address) gets the zone
// of the server's public address, which is the same place. Only the address goes to those services,
// and only when a Rondo asks; answers are kept 12 hours per address (a miss 10 minutes).

import type { FastifyBaseLogger, FastifyInstance } from "fastify";
import { request } from "undici";
import { TtlCache } from "./ttl.js";
import { dispatcher } from "./upstream.js";

export type TextGet = (url: string) => Promise<{ status: number; body: string }>;

const TIMEOUT_MS = 6000;
const HIT_TTL_MS = 12 * 3600_000;
const MISS_TTL_MS = 10 * 60_000;
const USER_AGENT = "Rondo server (https://github.com/jarvissax/rondo)";
// an IANA zone name: "Europe/Lisbon", "America/Argentina/Buenos_Aires", "Etc/UTC", "UTC"
const ZONE = /^[A-Za-z][A-Za-z0-9_+-]{0,30}(\/[A-Za-z0-9_+-]{1,30}){0,2}$/;

export const textGet: TextGet = async (url) => {
  const res = await request(url, {
    dispatcher,
    headers: { "user-agent": USER_AGENT, accept: "application/json, text/plain" },
    signal: AbortSignal.timeout(TIMEOUT_MS),
  });
  return { status: res.statusCode, body: await res.body.text() };
};

// addresses no lookup service can place: loopback, private, link-local, carrier-grade NAT
export function isPrivateAddress(ip: string): boolean {
  const a = ip.toLowerCase().replace(/^::ffff:/, "");
  const v4 = /^(\d{1,3})\.(\d{1,3})\.\d{1,3}\.\d{1,3}$/.exec(a);
  if (v4) {
    const [x, y] = [Number(v4[1]), Number(v4[2])];
    return x === 0 || x === 10 || x === 127 || (x === 169 && y === 254) || (x === 172 && y >= 16 && y <= 31) || (x === 192 && y === 168) || (x === 100 && y >= 64 && y <= 127);
  }
  return a === "::" || a === "::1" || /^f[cd]/.test(a) || /^fe[89ab]/.test(a) || !a.includes(":");
}

interface Source {
  name: string;
  url: (ip: string | null) => string;
  zone: (body: string) => string | null;
}

export const SOURCES: Source[] = [
  {
    name: "ipwho.is",
    url: (ip) => `https://ipwho.is/${ip ?? ""}?fields=success,timezone`,
    zone: (body) => {
      const j = JSON.parse(body) as { success?: boolean; timezone?: { id?: unknown } };
      return j.success && typeof j.timezone?.id === "string" ? j.timezone.id : null;
    },
  },
  {
    name: "ipapi.co",
    url: (ip) => (ip ? `https://ipapi.co/${ip}/timezone/` : "https://ipapi.co/timezone/"),
    zone: (body) => body.trim(),
  },
];

export class TimezoneService {
  private readonly cache = new TtlCache<string | null>((zone) => (zone ? HIT_TTL_MS : MISS_TTL_MS), 500);

  constructor(private readonly deps: { get?: TextGet; log?: FastifyBaseLogger } = {}) {}

  // the zone for a request from `ip` (null: no service could tell)
  zone(ip: string): Promise<string | null> {
    const asked = isPrivateAddress(ip) ? null : ip;
    return this.cache.get(asked ?? "", () => this.lookup(asked));
  }

  private async lookup(ip: string | null): Promise<string | null> {
    const get = this.deps.get ?? textGet;
    for (const s of SOURCES) {
      try {
        const res = await get(s.url(ip));
        if (res.status !== 200) {
          this.deps.log?.warn(`time zone: ${s.name} answered ${res.status}`);
          continue;
        }
        const zone = s.zone(res.body);
        if (zone && ZONE.test(zone)) return zone;
        this.deps.log?.warn(`time zone: ${s.name} named no zone`);
      } catch (e) {
        this.deps.log?.warn(`time zone: ${s.name} failed (${e instanceof Error ? e.message : String(e)})`);
      }
    }
    return null;
  }
}

export function registerTimezoneRoutes(app: FastifyInstance, deps: { timezone: TimezoneService }): void {
  // GET /timezone: {"timezone": "Europe/Lisbon"}, or {"timezone": null} when no service could tell
  app.get("/timezone", async (req) => ({ timezone: await deps.timezone.zone(req.ip) }));
}
