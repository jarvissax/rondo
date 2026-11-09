import { describe, expect, it } from "vitest";
import Fastify from "fastify";
import { isPrivateAddress, registerTimezoneRoutes, type TextGet, TimezoneService } from "../src/timezone.js";

describe("isPrivateAddress", () => {
  it("knows the addresses no lookup service can place", () => {
    for (const ip of ["127.0.0.1", "10.1.2.3", "172.16.0.1", "172.31.255.1", "192.168.1.20", "169.254.1.1", "100.64.0.1", "::1", "fd00::1", "fe80::1", "::ffff:192.168.0.2"])
      expect(isPrivateAddress(ip), ip).toBe(true);
    for (const ip of ["8.8.8.8", "172.32.0.1", "100.128.0.1", "2a00:1450::1", "::ffff:8.8.8.8"]) expect(isPrivateAddress(ip), ip).toBe(false);
  });
});

describe("TimezoneService", () => {
  const answers = (map: Record<string, { status: number; body: string } | Error>) => {
    const asked: string[] = [];
    const get: TextGet = async (url) => {
      asked.push(url);
      const key = Object.keys(map).find((k) => url.startsWith(k));
      const a = (key ? map[key] : undefined) ?? { status: 404, body: "" };
      if (a instanceof Error) throw a;
      return a;
    };
    return { get, asked };
  };

  it("asks ipwho.is about the Rondo's public address", async () => {
    const { get, asked } = answers({ "https://ipwho.is/": { status: 200, body: '{"success":true,"timezone":{"id":"Europe/Lisbon"}}' } });
    expect(await new TimezoneService({ get }).zone("203.0.113.7")).toBe("Europe/Lisbon");
    expect(asked).toEqual(["https://ipwho.is/203.0.113.7?fields=success,timezone"]);
  });

  it("asks about the server's own address for a Rondo on its network", async () => {
    const { get, asked } = answers({ "https://ipwho.is/": { status: 200, body: '{"success":true,"timezone":{"id":"Europe/Berlin"}}' } });
    expect(await new TimezoneService({ get }).zone("192.168.1.20")).toBe("Europe/Berlin");
    expect(asked).toEqual(["https://ipwho.is/?fields=success,timezone"]);
  });

  it("falls back to ipapi.co, and leaves out what isn't a zone name", async () => {
    const down = answers({ "https://ipwho.is/": new Error("timeout"), "https://ipapi.co/": { status: 200, body: "America/Argentina/Buenos_Aires\n" } });
    expect(await new TimezoneService({ get: down.get }).zone("203.0.113.7")).toBe("America/Argentina/Buenos_Aires");
    expect(down.asked[1]).toBe("https://ipapi.co/203.0.113.7/timezone/");
    const odd = answers({ "https://ipwho.is/": { status: 200, body: '{"success":false}' }, "https://ipapi.co/": { status: 200, body: "<html>rate limited</html>" } });
    expect(await new TimezoneService({ get: odd.get }).zone("203.0.113.7")).toBeNull();
  });

  it("keeps an answer for the same address", async () => {
    const { get, asked } = answers({ "https://ipwho.is/": { status: 200, body: '{"success":true,"timezone":{"id":"Europe/Lisbon"}}' } });
    const tz = new TimezoneService({ get });
    await tz.zone("203.0.113.7");
    await tz.zone("203.0.113.7");
    expect(asked.length).toBe(1);
  });
});

describe("GET /timezone", () => {
  it("answers with the zone of the address the request came from", async () => {
    const app = Fastify();
    const seen: string[] = [];
    registerTimezoneRoutes(app, {
      timezone: new TimezoneService({
        get: async (url) => {
          seen.push(url);
          return { status: 200, body: '{"success":true,"timezone":{"id":"Europe/Lisbon"}}' };
        },
      }),
    });
    const res = await app.inject({ url: "/timezone", remoteAddress: "203.0.113.7" });
    expect(res.json()).toEqual({ timezone: "Europe/Lisbon" });
    expect(seen[0]).toContain("203.0.113.7");
  });
});
