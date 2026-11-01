// CORS for the two paths a desktop DASH player (dash.js on another origin) fetches.
// Access is granted by the token, never by cookies, so any origin may read the response.

import type { FastifyInstance } from "fastify";

const CORS_PREFIXES = ["/mpd/", "/s/"];

export function registerCors(app: FastifyInstance): void {
  app.addHook("onRequest", async (req, reply) => {
    const path = req.url.split("?", 1)[0] ?? "";
    if (!CORS_PREFIXES.some((p) => path.startsWith(p))) return;
    reply
      .header("access-control-allow-origin", "*")
      .header("access-control-expose-headers", "Content-Range, Content-Length, Accept-Ranges");
    if (req.method === "OPTIONS") {
      return reply
        .code(204)
        .header("access-control-allow-methods", "GET, HEAD, OPTIONS")
        .header("access-control-allow-headers", "Range, Authorization")
        .header("access-control-max-age", "600")
        .send();
    }
  });
}
