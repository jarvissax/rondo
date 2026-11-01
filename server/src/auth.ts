// Bearer-token gate for every route except /healthz, the admin page (its own sign-in) and /device/pair
// (a connect code), plus a per-IP rate limit so a leaked token cannot turn the VPS into an open proxy.
// The token is the one device's (docs/dev/server-api.md): checked by a function, since pairing changes it.

import { createHash, timingSafeEqual } from "node:crypto";
import type { FastifyInstance, FastifyRequest } from "fastify";
import rateLimit from "@fastify/rate-limit";

// Desktop DASH players cannot send headers, so these paths also accept ?token=.
const QUERY_TOKEN_PREFIXES = ["/mpd/", "/s/"];

function digest(value: string): Buffer {
  return createHash("sha256").update(value).digest();
}

function presentedToken(req: FastifyRequest): string | undefined {
  const header = req.headers.authorization;
  if (header?.startsWith("Bearer ")) return header.slice("Bearer ".length).trim();
  const path = req.url.split("?", 1)[0] ?? "";
  if (QUERY_TOKEN_PREFIXES.some((p) => path.startsWith(p))) {
    const token = (req.query as Record<string, unknown> | undefined)?.token;
    if (typeof token === "string") return token;
  }
  return undefined;
}

export type TokenCheck = (token: string) => boolean;

// a fixed token (tests, simple setups) or a check that follows pairing; `seen` notes the device's visits
export async function registerAuth(app: FastifyInstance, token: string | TokenCheck, seen?: (req: FastifyRequest) => void): Promise<void> {
  const check: TokenCheck = typeof token === "string" ? fixed(token) : token;

  // a list screen loads ~20 thumbnails at once; /thumb only reaches Google's image hosts
  await app.register(rateLimit, { max: 10, timeWindow: 1000, allowList: (req) => req.url.startsWith("/thumb?") });

  app.addHook("onRequest", async (req, reply) => {
    const path = req.url.split("?", 1)[0] ?? "";
    if (path === "/healthz" || path === "/admin" || path.startsWith("/admin/") || path === "/device/pair") return;
    const presented = presentedToken(req);
    if (presented === undefined || !check(presented)) {
      return reply.code(401).send({ error: "unauthorized" });
    }
    seen?.(req);
  });
}

function fixed(apiToken: string): TokenCheck {
  const expected = digest(apiToken);
  return (t) => timingSafeEqual(digest(t), expected);
}

// Strips ?token= from URLs before they reach the logs.
export function redactUrl(url: string): string {
  return url.replace(/([?&]token=)[^&]*/g, "$1[redacted]");
}
