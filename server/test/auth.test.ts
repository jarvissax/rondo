import { afterAll, beforeAll, describe, expect, it } from "vitest";
import type { FastifyInstance } from "fastify";
import { buildApp } from "../src/app.js";
import { redactUrl } from "../src/auth.js";
import { loadConfig } from "../src/config.js";

const TOKEN = "t".repeat(64);

describe("auth", () => {
  let app: FastifyInstance;

  beforeAll(async () => {
    const config = loadConfig({ API_TOKEN: TOKEN, YTDLP_BIN: "/nonexistent", LOG_LEVEL: "silent" });
    ({ app } = await buildApp(config));
    // stand-in for a browse route, registered after the auth hook like the real ones
    app.get("/test/protected", async () => ({ route: "library" }));
    await app.ready();
  });

  afterAll(async () => {
    await app.close();
  });

  it("serves /healthz without a token", async () => {
    const res = await app.inject({ url: "/healthz" });
    expect(res.statusCode).toBe(200);
    expect(res.json()).toMatchObject({ ytdlp: null, lastResolveOk: null });
    // the server's own version and the device–server protocol's number (src/version.ts)
    const { version, api } = res.json();
    expect(version).toMatch(/^\d+\.\d+\.\d+$/);
    expect(api).toBe(1);
  });

  it("rejects a request without a token", async () => {
    const res = await app.inject({ url: "/test/protected" });
    expect(res.statusCode).toBe(401);
  });

  it("rejects a wrong bearer token", async () => {
    const res = await app.inject({ url: "/test/protected", headers: { authorization: "Bearer nope" } });
    expect(res.statusCode).toBe(401);
  });

  it("accepts the bearer token", async () => {
    const res = await app.inject({ url: "/test/protected", headers: { authorization: `Bearer ${TOKEN}` } });
    expect(res.statusCode).toBe(200);
  });

  it("accepts ?token= on /mpd and /s only", async () => {
    // past auth, the real routes reject the malformed id with 400
    expect((await app.inject({ url: `/mpd/abc?token=${TOKEN}` })).statusCode).toBe(400);
    expect((await app.inject({ url: `/s/abc/140?token=${TOKEN}` })).statusCode).toBe(400);
    expect((await app.inject({ url: `/mpd/abc?token=wrong` })).statusCode).toBe(401);
    expect((await app.inject({ url: `/test/protected?token=${TOKEN}` })).statusCode).toBe(401);
  });

  it("rate-limits above 10 requests per second per IP", async () => {
    const codes: number[] = [];
    for (let i = 0; i < 12; i++) {
      const res = await app.inject({
        url: "/test/protected",
        headers: { authorization: `Bearer ${TOKEN}` },
        remoteAddress: "10.9.9.9",
      });
      codes.push(res.statusCode);
    }
    expect(codes).toContain(429);
  });

  it("serves /time with the token only", async () => {
    expect((await app.inject({ url: "/time" })).statusCode).toBe(401);
    const res = await app.inject({ url: "/time", headers: { authorization: `Bearer ${TOKEN}` } });
    expect(Math.abs(res.json().epochMs - Date.now())).toBeLessThan(5000);
  });

  it("redacts ?token= in logged URLs", () => {
    expect(redactUrl(`/mpd/abc?token=${TOKEN}&x=1`)).toBe("/mpd/abc?token=[redacted]&x=1");
  });

  it("refuses to start without a long token", () => {
    expect(() => loadConfig({ API_TOKEN: "short" })).toThrow();
  });
});
