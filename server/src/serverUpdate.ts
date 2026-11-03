// The server's own updates from the admin page (docs/server.md). With the updater container beside
// it (infra/compose.*.yaml, src/updater/main.ts), the admin page's Update button asks the updater to
// download the newest server image and restart this server on it. The two share a token through a
// small volume; without the updater (a server built from source, or an older compose file) the page
// shows the manual steps instead.

import { readFileSync } from "node:fs";

export interface UpdaterStatus {
  available: boolean;
  state?: string; // idle | pulling | restarting | updated | current | failed
  message?: string;
  at?: number;
}

export class ServerUpdate {
  constructor(
    private readonly url: string,
    private readonly tokenFile: string,
    private readonly fetchImpl: typeof fetch = fetch,
  ) {}

  private token(): string {
    try {
      return readFileSync(this.tokenFile, "utf8").trim();
    } catch {
      return "";
    }
  }

  private async ask(method: string, path: string): Promise<Response | null> {
    const token = this.token();
    if (!token) return null;
    try {
      return await this.fetchImpl(this.url.replace(/\/+$/, "") + path, {
        method,
        headers: { authorization: `Bearer ${token}` },
        signal: AbortSignal.timeout(4000),
      });
    } catch {
      return null;
    }
  }

  async status(): Promise<UpdaterStatus> {
    const res = await this.ask("GET", "/v1/status");
    if (!res?.ok) return { available: false };
    const s = (await res.json().catch(() => ({}))) as { state?: string; message?: string; at?: number };
    return { available: true, state: s.state, message: s.message, at: s.at };
  }

  async start(): Promise<{ ok: true } | { error: "unavailable" | "busy" | "failed" }> {
    const res = await this.ask("POST", "/v1/update");
    if (!res) return { error: "unavailable" };
    if (res.status === 409) return { error: "busy" };
    return res.ok ? { ok: true } : { error: "failed" };
  }
}
