import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { isCookiesFile } from "../src/admin.js";
import { buildApp } from "../src/app.js";
import { loadConfig } from "../src/config.js";
import { renderMpd } from "../src/mpd.js";
import { hashPassword, verifyPassword } from "../src/store.js";

const ENV_TOKEN = "e".repeat(64);
const PASSWORD = "correct horse battery";
const apps: Array<{ close(): Promise<unknown> }> = [];

afterEach(async () => {
  await Promise.all(apps.splice(0).map((a) => a.close()));
});

async function server(env: Record<string, string> = {}) {
  const dir = mkdtempSync(join(tmpdir(), "rondo-admin-"));
  const config = loadConfig({
    API_TOKEN: ENV_TOKEN,
    YTDLP_BIN: "/nonexistent",
    LOG_LEVEL: "silent",
    COOKIES_FILE: join(dir, "cookies.txt"),
    CONFIG_FILE: join(dir, "config.json"),
    ...env,
  });
  const { app } = await buildApp(config, { newCode: () => "482913" });
  apps.push(app);
  return app;
}

const sessionCookie = (res: { headers: Record<string, unknown> }) => String(res.headers["set-cookie"]).split(";")[0]!;

describe("passwords", () => {
  it("verifies the right password only", () => {
    const hash = hashPassword(PASSWORD);
    expect(hash.startsWith("scrypt$")).toBe(true);
    expect(verifyPassword(PASSWORD, hash)).toBe(true);
    expect(verifyPassword("wrong password", hash)).toBe(false);
  });
});

describe("cookies.txt check", () => {
  it("wants a Netscape file with youtube.com cookies", () => {
    const line = [".youtube.com", "TRUE", "/", "TRUE", "1999999999", "SID", "x"].join("\t");
    expect(isCookiesFile(`# Netscape HTTP Cookie File\n${line}\n`)).toBe(true);
    expect(isCookiesFile(`#HttpOnly_${line}`)).toBe(true);
    expect(isCookiesFile([".example.com", "TRUE", "/", "TRUE", "0", "a", "b"].join("\t"))).toBe(false);
    expect(isCookiesFile("not a cookies file")).toBe(false);
  });
});

describe("admin setup and sign-in", () => {
  it("needs the setup code from the log, then a session", async () => {
    const app = await server();
    expect((await app.inject("/admin/api/state")).json()).toMatchObject({ setup: true, signedIn: false });
    expect((await app.inject("/admin/api/status")).statusCode).toBe(401);

    const wrong = await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "000000", password: PASSWORD } });
    expect(wrong.statusCode).toBe(403);
    const short = await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482-913", password: "short" } });
    expect(short.statusCode).toBe(400);

    const ok = await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482-913", password: PASSWORD } });
    expect(ok.statusCode).toBe(200);
    const cookie = sessionCookie(ok);
    expect(cookie.startsWith("rondo_admin=")).toBe(true);
    expect(String(ok.headers["set-cookie"])).toContain("HttpOnly");

    const status = await app.inject({ url: "/admin/api/status", headers: { cookie } });
    expect(status.statusCode).toBe(200);
    expect(status.json().device).toMatchObject({ paired: true, source: "env" });

    expect((await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: PASSWORD } })).statusCode).toBe(409);
    await app.inject({ method: "POST", url: "/admin/api/logout", headers: { cookie } });
    expect((await app.inject({ url: "/admin/api/status", headers: { cookie } })).statusCode).toBe(401);
  });

  it("slows down password guessing", async () => {
    const app = await server();
    await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: PASSWORD } });
    for (let i = 0; i < 5; i++) {
      expect((await app.inject({ method: "POST", url: "/admin/api/login", payload: { password: "nope" } })).statusCode).toBe(403);
    }
    const blocked = await app.inject({ method: "POST", url: "/admin/api/login", payload: { password: PASSWORD } });
    expect(blocked.statusCode).toBe(429);
    expect(blocked.json().retryAfterSec).toBeGreaterThan(0);
  });

  it("limits guessing from many addresses too", async () => {
    const app = await server();
    // five wrong setup codes from five addresses: a new code and a pause for everyone
    for (let i = 0; i < 5; i++) {
      const res = await app.inject({ method: "POST", url: "/admin/api/setup", remoteAddress: `10.0.0.${i}`, payload: { code: "000000", password: PASSWORD } });
      expect(res.statusCode).toBe(403);
    }
    const paused = await app.inject({ method: "POST", url: "/admin/api/setup", remoteAddress: "10.0.1.1", payload: { code: "482913", password: PASSWORD } });
    expect(paused.statusCode).toBe(429);

    const fresh = await server();
    await fresh.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: PASSWORD } });
    for (let i = 0; i < 30; i++) {
      await fresh.inject({ method: "POST", url: "/admin/api/login", remoteAddress: `10.1.${i}.1`, payload: { password: "nope" } });
    }
    const blocked = await fresh.inject({ method: "POST", url: "/admin/api/login", remoteAddress: "10.2.0.1", payload: { password: PASSWORD } });
    expect(blocked.statusCode).toBe(429);
  });

  it("signs out other browsers when the password changes", async () => {
    const app = await server();
    const first = sessionCookie(await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: PASSWORD } }));
    const second = sessionCookie(await app.inject({ method: "POST", url: "/admin/api/login", payload: { password: PASSWORD } }));
    const next = "a brand new password";
    const changed = await app.inject({ method: "POST", url: "/admin/api/password", headers: { cookie: first }, payload: { current: PASSWORD, next } });
    expect(changed.statusCode).toBe(200);
    expect((await app.inject({ url: "/admin/api/status", headers: { cookie: second } })).statusCode).toBe(401);
    expect((await app.inject({ url: "/admin/api/status", headers: { cookie: sessionCookie(changed) } })).statusCode).toBe(200);
    expect((await app.inject({ method: "POST", url: "/admin/api/login", payload: { password: next } })).statusCode).toBe(200);
  });
});

describe("one device", () => {
  it("swaps a connect code for the one token and retires the old one", async () => {
    const app = await server();
    const setup = await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: PASSWORD } });
    const cookie = sessionCookie(setup);

    // until a pairing, the environment token is the device token
    expect((await app.inject({ url: "/time", headers: { authorization: `Bearer ${ENV_TOKEN}` } })).statusCode).toBe(200);

    expect((await app.inject({ method: "POST", url: "/device/pair", payload: { code: "482913" } })).statusCode).toBe(403); // no code yet
    const code = await app.inject({ method: "POST", url: "/admin/api/device/code", headers: { cookie } });
    expect(code.json().code).toBe("482913");
    expect((await app.inject({ method: "POST", url: "/device/pair", payload: { code: "111111" } })).statusCode).toBe(403);

    const paired = await app.inject({ method: "POST", url: "/device/pair", payload: { code: "482 913", name: "Rondo" } });
    expect(paired.statusCode).toBe(200);
    const token = paired.json().token as string;
    expect(token).toMatch(/^[0-9a-f]{64}$/);

    expect((await app.inject({ url: "/time", headers: { authorization: `Bearer ${ENV_TOKEN}` } })).statusCode).toBe(401);
    expect((await app.inject({ url: "/time", headers: { authorization: `Bearer ${token}` } })).statusCode).toBe(200);
    const status = (await app.inject({ url: "/admin/api/status", headers: { cookie } })).json();
    expect(status.device).toMatchObject({ paired: true, source: "paired" });
    expect(status.device.lastSeenAt).toBeGreaterThan(0);
    expect(status.pairing).toBeNull();

    // a used code does not work twice
    expect((await app.inject({ method: "POST", url: "/device/pair", payload: { code: "482913" } })).statusCode).toBe(403);

    expect(status.device.name).toBe("Rondo");

    await app.inject({ method: "DELETE", url: "/admin/api/device", headers: { cookie } });
    expect((await app.inject({ url: "/time", headers: { authorization: `Bearer ${token}` } })).statusCode).toBe(401);
    // the environment token does not come back after a disconnect
    expect((await app.inject({ url: "/time", headers: { authorization: `Bearer ${ENV_TOKEN}` } })).statusCode).toBe(401);
    expect((await app.inject({ url: "/admin/api/status", headers: { cookie } })).json().device).toMatchObject({ paired: false, source: null });
  });

  it("cancels a code after five wrong tries", async () => {
    const app = await server();
    const cookie = sessionCookie(await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: PASSWORD } }));
    await app.inject({ method: "POST", url: "/admin/api/device/code", headers: { cookie } });
    for (let i = 0; i < 5; i++) await app.inject({ method: "POST", url: "/device/pair", payload: { code: "000000" } });
    expect((await app.inject({ method: "POST", url: "/device/pair", payload: { code: "482913" } })).statusCode).toBe(403);
  });

  it("works without an environment token, once a Rondo is connected", async () => {
    const app = await server({ API_TOKEN: "" });
    expect((await app.inject({ url: "/time", headers: { authorization: `Bearer ${ENV_TOKEN}` } })).statusCode).toBe(401);
    const cookie = sessionCookie(await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: PASSWORD } }));
    expect((await app.inject({ url: "/admin/api/status", headers: { cookie } })).json().device).toMatchObject({ paired: false, source: null });
    await app.inject({ method: "POST", url: "/admin/api/device/code", headers: { cookie } });
    const token = (await app.inject({ method: "POST", url: "/device/pair", payload: { code: "482913" } })).json().token;
    expect((await app.inject({ url: "/time", headers: { authorization: `Bearer ${token}` } })).statusCode).toBe(200);
  });
});

describe("cookies from the admin page", () => {
  it("rejects files that are not cookies.txt", async () => {
    const app = await server();
    const cookie = sessionCookie(await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: PASSWORD } }));
    const res = await app.inject({ method: "POST", url: "/admin/api/cookies", headers: { cookie, "content-type": "text/plain" }, payload: "hello" });
    expect(res.statusCode).toBe(400);
    expect(res.json().error).toBe("format");
  });
});

describe("manifests without PUBLIC_BASE", () => {
  it("use relative stream URLs that follow the device's address", () => {
    const video = {
      id: "abc",
      durationMs: 1000,
      formats: new Map([
        [140, { itag: 140, mime: "audio/mp4", codecs: "mp4a.40.2", bitrate: 128000, url: "u" }],
        [134, { itag: 134, mime: "video/mp4", codecs: "avc1.4d401e", bitrate: 600000, width: 640, height: 360, fps: 30, url: "u" }],
      ]),
    };
    const ranges = new Map([
      [140, { init: [0, 600], index: [601, 900] }],
      [134, { init: [0, 700], index: [701, 1000] }],
    ]);
    const xml = renderMpd({ video: video as never, ranges: ranges as never, publicBase: "" });
    expect(xml).toContain("<BaseURL>/s/abc/134</BaseURL>");
  });
});
