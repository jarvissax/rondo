// Runtime configuration, read once from the environment.

export interface Config {
  // the device token from the environment: used until a Rondo is connected on the admin page ("" = none)
  apiToken: string;
  // absolute base for stream URLs in manifests; "" = relative URLs, so they follow whatever address
  // the device used (a home server by IP, a domain, http or https)
  publicBase: string;
  // admin password hash and the paired device token hash (docs/dev/server-api.md)
  configFile: string;
  // behind Caddy: trust X-Forwarded-For/-Proto (a home server reached directly should not)
  trustProxy: boolean;
  cookiesFile: string;
  tvTokensFile: string;
  ytdlpBin: string;
  port: number;
  logLevel: string;
  healthVideo: string;
  ytdlpUseCookies: boolean;
  debugTtlS?: number;
}

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const apiToken = env.API_TOKEN ?? "";
  if (apiToken && apiToken.length < 32) {
    throw new Error("API_TOKEN is shorter than 32 characters (leave it unset to connect a Rondo on the admin page)");
  }
  return {
    apiToken,
    publicBase: (env.PUBLIC_BASE ?? "").replace(/\/+$/, ""),
    configFile: env.CONFIG_FILE ?? "/data/config.json",
    trustProxy: env.TRUST_PROXY !== "0",
    cookiesFile: env.COOKIES_FILE ?? "/data/secrets/cookies.txt",
    // the admin page's "Sign in with a code" (tv.ts)
    tvTokensFile: env.TV_TOKENS_FILE ?? "/data/secrets/tv-oauth.json",
    ytdlpBin: env.YTDLP_BIN ?? "yt-dlp",
    port: Number(env.PORT ?? 3000),
    logLevel: env.LOG_LEVEL ?? "info",
    healthVideo: env.HEALTH_VIDEO ?? "dQw4w9WgXcQ",
    // yt-dlp resolves anonymously (verified working); account cookies switch it to clients that
    // may need PO tokens. Turn on only if YouTube starts challenging the VPS (plan, risk 2).
    ytdlpUseCookies: env.YTDLP_USE_COOKIES === "1",
    debugTtlS: env.DEBUG_TTL_S ? Number(env.DEBUG_TTL_S) : undefined,
  };
}
