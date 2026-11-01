// /healthz state: yt-dlp version, last periodic resolve result, cookies presence.
// Refreshed every 15 minutes; a failure is logged as a warning so YouTube breakage
// shows up in `docker logs y1` before the Y1 notices.

import { execFile } from "node:child_process";
import { stat } from "node:fs/promises";
import { promisify } from "node:util";
import type { FastifyBaseLogger } from "fastify";
import type { Config } from "./config.js";

const run = promisify(execFile);
const INTERVAL_MS = 15 * 60 * 1000;

export interface HealthState {
  ok: boolean;
  ytdlp: string | null;
  lastResolveOk: boolean | null;
  cookiesOk: boolean;
}

export type ResolveProbe = () => Promise<boolean>;

export class Health {
  private ytdlp: string | null = null;
  private lastResolveOk: boolean | null = null;
  private cookiesOk = false;
  private timer: NodeJS.Timeout | undefined;
  private probe: ResolveProbe | undefined;

  constructor(
    private readonly config: Config,
    private readonly log: FastifyBaseLogger,
  ) {}

  setResolveProbe(probe: ResolveProbe): void {
    this.probe = probe;
  }

  state(): HealthState {
    return {
      ok: this.ytdlp !== null && this.lastResolveOk !== false,
      ytdlp: this.ytdlp,
      lastResolveOk: this.lastResolveOk,
      cookiesOk: this.cookiesOk,
    };
  }

  recordResolve(ok: boolean): void {
    this.lastResolveOk = ok;
  }

  async refresh(): Promise<void> {
    try {
      const { stdout } = await run(this.config.ytdlpBin, ["--version"], { timeout: 15_000 });
      this.ytdlp = stdout.trim();
    } catch (err) {
      this.ytdlp = null;
      this.log.warn({ err: (err as Error).message }, "yt-dlp --version failed");
    }
    try {
      this.cookiesOk = (await stat(this.config.cookiesFile)).size > 0;
    } catch {
      this.cookiesOk = false;
    }
    if (this.probe) {
      const ok = await this.probe().catch(() => false);
      this.recordResolve(ok);
      if (!ok) this.log.warn("periodic resolve of the health-check video failed");
    }
  }

  start(): void {
    void this.refresh();
    this.timer = setInterval(() => void this.refresh(), INTERVAL_MS);
    this.timer.unref();
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
  }
}
