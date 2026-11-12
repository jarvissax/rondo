// Builds the Fastify instance; main.ts listens, tests inject.

import { VERSION } from "./version.js";
import Fastify, { type FastifyInstance } from "fastify";
import { Admin, LogRing, registerAdminRoutes } from "./admin.js";
import { registerAuth, redactUrl } from "./auth.js";
import { type Browse, registerBrowseRoutes } from "./browse.js";
import { TvAccount } from "./tv.js";
import type { Config } from "./config.js";
import { registerCors } from "./cors.js";
import { EqService, registerEqRoutes } from "./eq.js";
import { Health } from "./health.js";
import { type LyricsDeps, LyricsService, lrclibGet, registerLyricsRoutes } from "./lyrics.js";
import { execRunner, Resolver, type YtdlpRunner } from "./resolver.js";
import type { ServerUpdate } from "./serverUpdate.js";
import { registerTimezoneRoutes, TimezoneService } from "./timezone.js";
import { registerUpdateRoutes, type Updates } from "./updates.js";
import { registerStreamRoutes, type UpstreamTiming } from "./stream.js";
import { Store } from "./store.js";

export interface App {
  app: FastifyInstance;
  health: Health;
  resolver: Resolver;
  admin: Admin;
  tv: TvAccount;
}

export async function buildApp(
  config: Config,
  opts: {
    ytdlpRunner?: YtdlpRunner;
    upstreamTiming?: UpstreamTiming;
    lyrics?: Partial<LyricsDeps>;
    newCode?: () => string;
    tv?: TvAccount;
    updates?: Updates;
    serverUpdate?: ServerUpdate;
    eq?: EqService;
    timezone?: TimezoneService;
  } = {},
): Promise<App> {
  const logs = new LogRing();
  const app = Fastify({
    // behind Caddy (the VPS) X-Forwarded-For/-Proto are trusted; a home server reached directly sets TRUST_PROXY=0
    trustProxy: config.trustProxy,
    logger: {
      level: config.logLevel,
      // warnings and errors also go to the admin page's Activity list (messages only, no request data)
      hooks: {
        logMethod(args, method, level) {
          const msg = args.find((a): a is string => typeof a === "string");
          if (msg) logs.push(level, msg);
          method.apply(this, args);
        },
      },
      redact: ["req.headers.authorization"],
      serializers: {
        req: (req) => ({ method: req.method, url: redactUrl(req.url), ip: req.ip }),
      },
    },
  });

  const health = new Health(config, app.log);
  const resolver = new Resolver(config, opts.ytdlpRunner ?? execRunner(config));
  health.setResolveProbe(async () => (await resolver.resolve(config.healthVideo, { force: true })).formats.size > 0);

  // the browse routes come after the auth hook (below); the admin page asks them about the account
  let browse: Browse | undefined;
  const tv = opts.tv ?? new TvAccount(config.tvTokensFile, app.log);
  const admin = new Admin({
    config,
    store: new Store(config.configFile),
    health,
    resolver,
    logs,
    log: app.log,
    account: async () => (browse ? browse.accountInfo() : null),
    newCode: opts.newCode,
    tv,
    updates: opts.updates,
    serverUpdate: opts.serverUpdate,
  });
  // account: which account feeds exist (the Rondo hides the others and runs signed out)
  app.get("/healthz", async () => ({ ...health.state(), version: VERSION, account: admin.accountFeatures() }));
  registerAdminRoutes(app, admin);

  registerCors(app); // before auth, so preflights and 401s carry the CORS headers
  await registerAuth(app, (t) => admin.isDeviceToken(t), (req) => admin.seen(req));
  registerStreamRoutes(app, { config, resolver, health, timing: opts.upstreamTiming });
  app.get("/time", async () => ({ epochMs: Date.now() })); // clock sync for the device
  registerTimezoneRoutes(app, { timezone: opts.timezone ?? new TimezoneService({ log: app.log }) }); // Rondo 2.0's time zone
  registerUpdateRoutes(app, opts.updates); // Settings › Software update
  // the first shorts of a Shorts list start resolving at once (their streams are asked for next)
  const routes = registerBrowseRoutes(app, { config, tv, prepare: (ids) => ids.forEach((id) => void resolver.resolve(id).catch(() => undefined)) });
  browse = routes;
  tv.onChange = () => routes.forget();
  const lyrics = new LyricsService({
    lrclib: lrclibGet,
    youtube: (id) => routes.musicLyrics(id),
    log: app.log,
    ...opts.lyrics,
  });
  registerLyricsRoutes(app, { lyrics });
  registerEqRoutes(app, opts.eq ?? new EqService()); // Rondo 2.0: Settings › Sound › Headphone match

  return { app, health, resolver, admin, tv };
}
