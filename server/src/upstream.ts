// Byte-range requests to googlevideo. Only yt-dlp's own per-format headers are sent;
// nothing from the device is forwarded. The range goes in the `range=` query parameter
// only: googlevideo treats that slice as the whole resource and applies a Range header on
// top of it (checked 2026-10-30), so sending both breaks every range not starting at 0.

import { Agent, interceptors, request } from "undici";
import type { StreamFormat } from "./resolver.js";

// shared by /thumb and /lyrics (LRCLIB) as well: keep-alive pool with redirects followed
export const dispatcher = new Agent({ keepAliveTimeout: 30_000, connections: 32 }).compose(
  interceptors.redirect({ maxRedirections: 3 }),
);

export type UpstreamResponse = Awaited<ReturnType<typeof request>>;

export function withRangeParam(url: string, start: number, end?: number): string {
  const u = new URL(url);
  u.searchParams.set("range", end === undefined ? `${start}-` : `${start}-${end}`);
  return u.toString();
}

export function fetchRange(
  format: Pick<StreamFormat, "url" | "httpHeaders">,
  start: number,
  end?: number,
  signal?: AbortSignal,
): Promise<UpstreamResponse> {
  return request(withRangeParam(format.url, start, end), {
    method: "GET",
    dispatcher,
    headers: { ...format.httpHeaders },
    signal,
    headersTimeout: 15_000,
    // a paused player stops reading; give it two minutes before the upstream is dropped
    bodyTimeout: 120_000,
  });
}
