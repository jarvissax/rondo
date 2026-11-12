// The server's admin page (docs/dev/server-api.md): first-run setup with a code from the log, admin
// sign-in, the YouTube account (cookies.txt), the one Rondo device and its token, yt-dlp, recent
// warnings. The page itself is static files in server/admin/, served at /admin/.

import { VERSION } from "./version.js";
import { execFile } from "node:child_process";
import { randomBytes, randomInt, timingSafeEqual } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { chmod, mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { dirname, join, normalize } from "node:path";
import { fileURLToPath } from "node:url";
import { promisify } from "node:util";
import type { FastifyBaseLogger, FastifyInstance, FastifyReply, FastifyRequest } from "fastify";
import type { Config } from "./config.js";
import type { Health } from "./health.js";
import type { Resolver } from "./resolver.js";
import type { TvAccount } from "./tv.js";
import { hashPassword, sha256, type Store, verifyPassword } from "./store.js";
import type { ServerUpdate } from "./serverUpdate.js";
import { isNewer, type Updates } from "./updates.js";

const run = promisify(execFile);
const SESSION_MS = 12 * 60 * 60 * 1000;
const CODE_MS = 10 * 60 * 1000;
const ACCOUNT_CHECK_MS = 15 * 60 * 1000;
const MIN_PASSWORD = 10;
const GLOBAL_FAILURES = 30;
const GLOBAL_WINDOW_MS = 60 * 60 * 1000;
// the server's version, from package.json (one folder up from both src/ and dist/)

export interface Account {
  name: string;
  handle: string | null;
}

// the last warnings and errors, for the Activity screen (filled from the logger's hook in app.ts)
export class LogRing {
  private readonly lines: Array<{ time: number; level: "warn" | "error"; msg: string }> = [];
  push(level: number, msg: string): void {
    if (level < 40 || !msg) return;
    this.lines.unshift({ time: Date.now(), level: level >= 50 ? "error" : "warn", msg: msg.slice(0, 300) });
    this.lines.length = Math.min(this.lines.length, 100);
  }
  all() {
    return this.lines;
  }
}

export interface AdminDeps {
  config: Config;
  store: Store;
  health: Health;
  resolver: Resolver;
  logs: LogRing;
  log: FastifyBaseLogger;
  // the signed-in account behind cookies.txt; null when signed out
  account: () => Promise<Account | null>;
  // setup and connect codes (tests pass a fixed one)
  newCode?: () => string;
  // "Sign in with a code" (tv.ts)
  tv?: TvAccount;
  // newer Rondo and yt-dlp releases (updates.ts); none when left out
  updates?: Updates;
  // updating this server from the admin page (serverUpdate.ts); none when left out
  serverUpdate?: ServerUpdate;
}

const randomCode = () => String(randomInt(0, 1_000_000)).padStart(6, "0");

export class Admin {
  private setupCode: string | null = null;
  private readonly sessions = new Map<string, number>();
  private readonly failures = new Map<string, number[]>();
  private globalFailures: number[] = [];
  private setupMisses = 0;
  private setupBlockedUntil = 0;
  private pairing: { code: string; expiresAt: number; tries: number } | null = null;
  private lastSeen: { at: number; ip: string; userAgent: string | null } | null = null;
  private accountState: { signedIn: boolean | null; account: Account | null; checkedAt: number | null } = {
    signedIn: null,
    account: null,
    checkedAt: null,
  };
  private timer: NodeJS.Timeout | undefined;
  private readonly started = Date.now();

  constructor(private readonly deps: AdminDeps) {
    if (!deps.store.data.adminHash) this.setupCode = (deps.newCode ?? randomCode)();
  }

  get tv(): TvAccount | undefined {
    return this.deps.tv;
  }

  get serverUpdate(): ServerUpdate | undefined {
    return this.deps.serverUpdate;
  }

  note(msg: string): void {
    this.deps.log.info(msg);
  }

  warn(msg: string, err: unknown): void {
    this.deps.log.warn({ err: String((err as Error)?.message ?? err).slice(0, 200) }, msg);
  }

  get needsSetup(): boolean {
    return !this.deps.store.data.adminHash;
  }

  // printed once at start: the only way to prove you own a fresh server
  announce(): void {
    if (!this.setupCode) return;
    const shown = `${this.setupCode.slice(0, 3)}-${this.setupCode.slice(3)}`;
    this.deps.log.warn(`Rondo setup: open /admin on this server (e.g. http://<server-ip>:${this.deps.config.port}/admin) and enter the code ${shown}`);
  }

  start(): void {
    void this.checkAccount();
    this.timer = setInterval(() => void this.checkAccount(), ACCOUNT_CHECK_MS);
    this.timer.unref();
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
  }

  // --- the device token: the paired one, or API_TOKEN from the environment until a pairing ---

  // API_TOKEN stops counting for good after the first pairing or disconnect
  tokenSource(): "paired" | "env" | null {
    const data = this.deps.store.data;
    if (data.device) return "paired";
    return this.deps.config.apiToken && !data.envRetired ? "env" : null;
  }

  isDeviceToken(token: string): boolean {
    const source = this.tokenSource();
    const expected =
      source === "paired" ? this.deps.store.data.device!.tokenHash : source === "env" ? sha256(this.deps.config.apiToken!) : null;
    if (!expected) return false;
    const a = Buffer.from(sha256(token), "hex");
    const b = Buffer.from(expected, "hex");
    return a.length === b.length && timingSafeEqual(a, b);
  }

  seen(req: FastifyRequest): void {
    const ua = req.headers["user-agent"];
    this.lastSeen = { at: Date.now(), ip: req.ip, userAgent: typeof ua === "string" ? ua.slice(0, 120) : null };
  }

  createCode(): { code: string; expiresAt: number } {
    this.pairing = { code: (this.deps.newCode ?? randomCode)(), expiresAt: Date.now() + CODE_MS, tries: 0 };
    return { code: this.pairing.code, expiresAt: this.pairing.expiresAt };
  }

  cancelCode(): void {
    this.pairing = null;
  }

  // the Rondo redeems a connect code for the one token; the previous device stops working
  pair(code: string, name: string | undefined): string | null {
    const p = this.pairing;
    if (!p || Date.now() > p.expiresAt) return null;
    if (code.replace(/\D/g, "") !== p.code) {
      if (++p.tries >= 5) this.pairing = null;
      return null;
    }
    this.pairing = null;
    const token = randomBytes(32).toString("hex");
    this.deps.store.update((d) => ({ ...d, device: { tokenHash: sha256(token), pairedAt: Date.now(), name: name?.slice(0, 40) }, envRetired: true }));
    this.lastSeen = null;
    this.deps.log.info("a Rondo was connected; the previous device token no longer works");
    return token;
  }

  disconnect(): void {
    this.deps.store.update((d) => {
      const { device: _gone, ...rest } = d;
      return { ...rest, envRetired: true };
    });
    this.lastSeen = null;
  }

  // --- admin sessions ---

  private newSession(reply: FastifyReply, req: FastifyRequest): string {
    const token = randomBytes(24).toString("hex");
    this.sessions.set(token, Date.now() + SESSION_MS);
    const secure = req.protocol === "https" ? "; Secure" : "";
    reply.header("set-cookie", `rondo_admin=${token}; Path=/admin; HttpOnly; SameSite=Strict; Max-Age=${SESSION_MS / 1000}${secure}`);
    return token;
  }

  signedIn(req: FastifyRequest): boolean {
    const token = cookie(req.headers.cookie, "rondo_admin");
    if (!token) return false;
    const expires = this.sessions.get(token);
    if (!expires || expires < Date.now()) {
      this.sessions.delete(token);
      return false;
    }
    return true;
  }

  // 5 wrong passwords a minute per address, and 30 an hour from all addresses together (guessing
  // from many addresses at once) → wait
  private throttled(ip: string): number {
    const now = Date.now();
    const recent = (this.failures.get(ip) ?? []).filter((t) => now - t < 60_000);
    if (recent.length) this.failures.set(ip, recent);
    else this.failures.delete(ip);
    this.globalFailures = this.globalFailures.filter((t) => now - t < GLOBAL_WINDOW_MS);
    const perIp = recent.length >= 5 ? recent[0]! + 60_000 - now : 0;
    const global = this.globalFailures.length >= GLOBAL_FAILURES ? this.globalFailures[0]! + GLOBAL_WINDOW_MS - now : 0;
    return Math.ceil(Math.max(perIp, global) / 1000);
  }

  private failed(ip: string): void {
    this.failures.set(ip, [...(this.failures.get(ip) ?? []), Date.now()]);
    this.globalFailures.push(Date.now());
  }

  setup(req: FastifyRequest, reply: FastifyReply, code: string, password: string) {
    if (!this.needsSetup) return reply.code(409).send({ error: "done" });
    const wait = Math.max(this.throttled(req.ip), Math.ceil((this.setupBlockedUntil - Date.now()) / 1000));
    if (wait > 0) return reply.code(429).send({ error: "wait", retryAfterSec: wait });
    if (!this.setupCode || code.replace(/\D/g, "") !== this.setupCode) {
      this.failed(req.ip);
      // five wrong setup codes from anywhere: a new code in the log and a minute's pause
      if (++this.setupMisses >= 5) {
        this.setupMisses = 0;
        this.setupBlockedUntil = Date.now() + 60_000;
        this.setupCode = (this.deps.newCode ?? randomCode)();
        this.deps.log.warn("five wrong setup codes: the setup code changed");
        this.announce();
      }
      return reply.code(403).send({ error: "code" });
    }
    if (password.length < MIN_PASSWORD) return reply.code(400).send({ error: "password" });
    this.deps.store.update((d) => ({ ...d, adminHash: hashPassword(password) }));
    this.setupCode = null;
    this.deps.log.info("admin password set; setup is done");
    this.newSession(reply, req);
    return { ok: true };
  }

  login(req: FastifyRequest, reply: FastifyReply, password: string) {
    const wait = this.throttled(req.ip);
    if (wait) return reply.code(429).send({ error: "wait", retryAfterSec: wait });
    const hash = this.deps.store.data.adminHash;
    if (!hash || !verifyPassword(password, hash)) {
      this.failed(req.ip);
      return reply.code(403).send({ error: "password" });
    }
    this.newSession(reply, req);
    return { ok: true };
  }

  logout(req: FastifyRequest, reply: FastifyReply) {
    const token = cookie(req.headers.cookie, "rondo_admin");
    if (token) this.sessions.delete(token);
    reply.header("set-cookie", "rondo_admin=; Path=/admin; HttpOnly; SameSite=Strict; Max-Age=0");
    return { ok: true };
  }

  // a new password signs out every other browser
  changePassword(req: FastifyRequest, reply: FastifyReply, current: string, next: string) {
    const wait = this.throttled(req.ip);
    if (wait) return reply.code(429).send({ error: "wait", retryAfterSec: wait });
    const hash = this.deps.store.data.adminHash;
    if (!hash || !verifyPassword(current, hash)) {
      this.failed(req.ip);
      return reply.code(403).send({ error: "password" });
    }
    if (next.length < MIN_PASSWORD) return reply.code(400).send({ error: "password" });
    this.deps.store.update((d) => ({ ...d, adminHash: hashPassword(next) }));
    this.sessions.clear();
    this.newSession(reply, req);
    return { ok: true };
  }

  // which account feeds the Rondo can show (GET /healthz): YouTube Music's library, YouTube's
  // subscriptions and history. Without an account both services run signed out and the Rondo hides
  // those tabs. Cookies count until a check finds them signed out.
  accountFeatures(): { music: boolean; videos: boolean } {
    const cookies = existsSync(this.deps.config.cookiesFile) && this.accountState.signedIn !== false;
    const has = cookies || !!this.deps.tv?.signedIn;
    return { music: has, videos: has };
  }

  // --- status ---

  // only what is newer than what runs here: a release this server can update to, a newer yt-dlp
  private updates(ytdlp: string | null | undefined) {
    const u = this.deps.updates?.get();
    return {
      server: u?.server && isNewer(u.server.version, VERSION) ? u.server : null,
      ytdlp: u?.ytdlp && isNewer(u.ytdlp.version, ytdlp) ? u.ytdlp : null,
      checkedAt: u?.checkedAt ?? null,
    };
  }

  status(req: FastifyRequest) {
    const h = this.deps.health.state();
    const device = this.deps.store.data.device;
    return {
      server: { version: VERSION, uptimeSec: Math.round((Date.now() - this.started) / 1000), https: req.protocol === "https" },
      ytdlp: { version: h.ytdlp, lastResolveOk: h.lastResolveOk },
      cookies: { present: h.cookiesOk || existsSync(this.deps.config.cookiesFile), ...this.accountState },
      device: {
        paired: this.tokenSource() !== null,
        source: this.tokenSource(),
        name: device?.name ?? null,
        pairedAt: device?.pairedAt ?? null,
        lastSeenAt: this.lastSeen?.at ?? null,
        lastIp: this.lastSeen?.ip ?? null,
        userAgent: this.lastSeen?.userAgent ?? null,
      },
      pairing: this.pairing && this.pairing.expiresAt > Date.now() ? { code: this.pairing.code, expiresAt: this.pairing.expiresAt } : null,
      tv: this.deps.tv?.state() ?? null,
      updates: this.updates(h.ytdlp),
    };
  }

  // --- YouTube account ---

  async checkAccount() {
    if (!existsSync(this.deps.config.cookiesFile)) {
      this.accountState = { signedIn: null, account: null, checkedAt: Date.now() };
      return { signedIn: false, account: null };
    }
    try {
      const account = await this.deps.account();
      this.accountState = { signedIn: account !== null, account, checkedAt: Date.now() };
      if (!account) this.deps.log.warn("cookies.txt is signed out: upload new cookies on the admin page");
    } catch (err) {
      this.accountState = { signedIn: false, account: null, checkedAt: Date.now() };
      this.deps.log.warn({ err: (err as Error).message }, "cookies.txt is signed out: upload new cookies on the admin page");
    }
    return { signedIn: this.accountState.signedIn ?? false, account: this.accountState.account };
  }

  async saveCookies(text: string) {
    if (!isCookiesFile(text)) return null;
    const file = this.deps.config.cookiesFile;
    await mkdir(dirname(file), { recursive: true });
    await writeFile(file, text, { mode: 0o600 });
    await chmod(file, 0o600);
    this.deps.log.info("cookies.txt replaced from the admin page");
    void this.deps.health.refresh();
    return this.checkAccount();
  }

  async removeCookies() {
    await rm(this.deps.config.cookiesFile, { force: true });
    this.accountState = { signedIn: null, account: null, checkedAt: Date.now() };
    void this.deps.health.refresh();
  }

  // --- yt-dlp ---

  private async ytdlpVersion(): Promise<string | null> {
    try {
      return (await run(this.deps.config.ytdlpBin, ["--version"], { timeout: 15_000 })).stdout.trim();
    } catch {
      return null;
    }
  }

  async updateYtdlp() {
    const before = await this.ytdlpVersion();
    try {
      await run(this.deps.config.ytdlpBin, ["-U"], { timeout: 180_000 });
    } catch (err) {
      return { ok: false, before, after: before, error: (err as Error).message.split("\n")[0] };
    }
    const after = await this.ytdlpVersion();
    void this.deps.health.refresh();
    return { ok: true, before, after };
  }

  async testYtdlp() {
    const started = Date.now();
    try {
      const video = await this.deps.resolver.resolve(this.deps.config.healthVideo, { force: true });
      if (video.formats.size === 0) return { ok: false, error: "no formats" };
      return { ok: true, ms: Date.now() - started };
    } catch (err) {
      return { ok: false, error: (err as Error).message.split("\n")[0] };
    }
  }

  logs() {
    return { lines: this.deps.logs.all() };
  }
}

// a Netscape cookies.txt (as browser extensions export it) with at least one youtube.com cookie
export function isCookiesFile(text: string): boolean {
  return text.split("\n").some((line) => {
    if (!line.trim() || (line.startsWith("#") && !line.startsWith("#HttpOnly_"))) return false;
    const fields = line.split("\t");
    return fields.length >= 7 && /(^|\.)youtube\.com$/i.test(fields[0]!.replace(/^#HttpOnly_/, ""));
  });
}

function cookie(header: string | undefined, name: string): string | undefined {
  return header
    ?.split(";")
    .map((c) => c.trim())
    .find((c) => c.startsWith(`${name}=`))
    ?.slice(name.length + 1);
}

// --- routes ---

const TYPES: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "application/javascript; charset=utf-8",
  ".svg": "image/svg+xml",
  ".ttf": "font/ttf",
  ".png": "image/png",
};

// server/admin/ next to dist/ or src/; the font comes from the app's assets in a checkout
const ADMIN_DIR = fileURLToPath(new URL("../admin/", import.meta.url));
const FONT_PATHS = [join(ADMIN_DIR, "fonts/Roboto-Regular.ttf"), fileURLToPath(new URL("../../native/assets/fonts/Roboto-Regular.ttf", import.meta.url))];

async function sendFile(reply: FastifyReply, path: string) {
  try {
    const body = await readFile(path);
    const ext = path.slice(path.lastIndexOf("."));
    return reply
      .type(TYPES[ext] ?? "application/octet-stream")
      .header("cache-control", ext === ".html" ? "no-cache" : "max-age=3600")
      .send(body);
  } catch {
    return reply.code(404).send({ error: "not_found" });
  }
}

export function registerAdminRoutes(app: FastifyInstance, admin: Admin): void {
  const guard = async (req: FastifyRequest, reply: FastifyReply) => {
    if (!admin.signedIn(req)) return reply.code(401).send({ error: "signin" });
  };
  const str = (v: unknown) => (typeof v === "string" ? v : "");

  // the page
  app.get("/admin", async (_req, reply) => reply.redirect("/admin/"));
  // the page names its script and styles with this start's time: after an update, a browser that
  // cached the old ones (max-age=3600) fetches the new ones with the new page
  const stamp = Date.now().toString(36);
  app.get("/admin/", async (_req, reply) => {
    try {
      const html = (await readFile(join(ADMIN_DIR, "index.html"), "utf8"))
        .replace('href="/admin/admin.css"', `href="/admin/admin.css?v=${stamp}"`)
        .replace('src="/admin/admin.js"', `src="/admin/admin.js?v=${stamp}"`);
      return reply.type("text/html; charset=utf-8").header("cache-control", "no-cache").send(html);
    } catch {
      return reply.code(404).send({ error: "not_found" });
    }
  });
  app.get("/admin/fonts/Roboto-Regular.ttf", async (_req, reply) => sendFile(reply, FONT_PATHS.find((p) => existsSync(p)) ?? FONT_PATHS[0]!));
  app.get<{ Params: { file: string } }>("/admin/:file", async (req, reply) => {
    const file = normalize(req.params.file);
    if (file.includes("/") || file.startsWith(".")) return reply.code(404).send({ error: "not_found" });
    return sendFile(reply, join(ADMIN_DIR, file));
  });

  // the API
  // the version, too: the admin page watches it come back new while the server updates itself
  app.get("/admin/api/state", async (req) => ({ setup: admin.needsSetup, signedIn: admin.signedIn(req), https: req.protocol === "https", version: VERSION }));
  app.post<{ Body: { code?: unknown; password?: unknown } }>("/admin/api/setup", async (req, reply) =>
    admin.setup(req, reply, str(req.body?.code), str(req.body?.password)),
  );
  app.post<{ Body: { password?: unknown } }>("/admin/api/login", async (req, reply) => admin.login(req, reply, str(req.body?.password)));
  app.post("/admin/api/logout", async (req, reply) => admin.logout(req, reply));

  app.register(async (scope) => {
    scope.addHook("onRequest", guard);
    scope.post<{ Body: { current?: unknown; next?: unknown } }>("/admin/api/password", async (req, reply) =>
      admin.changePassword(req, reply, str(req.body?.current), str(req.body?.next)),
    );
    scope.get("/admin/api/status", async (req) => admin.status(req));
    scope.post<{ Body: string }>("/admin/api/cookies", async (req, reply) => {
      const saved = await admin.saveCookies(typeof req.body === "string" ? req.body : "");
      if (!saved) return reply.code(400).send({ error: "format" });
      return { ok: true, ...saved };
    });
    scope.delete("/admin/api/cookies", async () => {
      await admin.removeCookies();
      return { ok: true };
    });
    scope.post("/admin/api/cookies/check", async () => admin.checkAccount());
    scope.post("/admin/api/ytdlp/update", async () => admin.updateYtdlp());
    // this server's own update, through the updater container (serverUpdate.ts)
    scope.get("/admin/api/server/update", async () => (await admin.serverUpdate?.status()) ?? { available: false });
    scope.post("/admin/api/server/update", async (_req, reply) => {
      const r = (await admin.serverUpdate?.start()) ?? { error: "unavailable" as const };
      if ("error" in r) return reply.code(r.error === "busy" ? 409 : 503).send(r);
      admin.note("server update started from the admin page");
      return r;
    });
    scope.post("/admin/api/ytdlp/test", async () => admin.testYtdlp());
    scope.post("/admin/api/device/code", async () => admin.createCode());
    scope.delete("/admin/api/device/code", async () => {
      admin.cancelCode();
      return { ok: true };
    });
    scope.delete("/admin/api/device", async () => {
      admin.disconnect();
      return { ok: true };
    });
    scope.get("/admin/api/logs", async () => admin.logs());
    // sign in with a code: a new code (replaces a waiting one), cancel it, or sign out
    scope.post("/admin/api/tv", async (_req, reply) => {
      const tv = admin.tv;
      if (!tv) return reply.code(404).send({ error: "not_found" });
      try {
        return await tv.begin();
      } catch (err) {
        admin.warn("no sign-in code from Google", err);
        return reply.code(502).send({ error: "google" });
      }
    });
    scope.delete("/admin/api/tv/code", async () => {
      admin.tv?.cancel();
      return { ok: true };
    });
    scope.delete("/admin/api/tv", async () => {
      await admin.tv?.signOut();
      return { ok: true };
    });
  });

  // the Rondo redeems a connect code (no token yet, so no auth header)
  app.post<{ Body: { code?: unknown; name?: unknown } }>("/device/pair", async (req, reply) => {
    const token = admin.pair(str(req.body?.code), str(req.body?.name) || undefined);
    if (!token) return reply.code(403).send({ error: "code" });
    return { token };
  });
}
