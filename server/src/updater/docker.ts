// A small client for the Docker Engine API over its Unix socket, for the updater (no dependencies).
// It speaks the newest API version both sides know: the engine's own, capped at the one this code
// was written against, and never below the engine's minimum (Docker 29 refuses anything under 1.40).

import { request } from "node:http";

export const MAX_API = "1.47";

export class DockerError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

const parts = (v: string) => v.split(".").map((n) => Number(n) || 0);

export function compareApi(a: string, b: string): number {
  const [x, y] = [parts(a), parts(b)];
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    const d = (x[i] ?? 0) - (y[i] ?? 0);
    if (d) return d;
  }
  return 0;
}

export function pickApiVersion(engine: string, engineMin = "1.24"): string {
  const capped = compareApi(engine, MAX_API) > 0 ? MAX_API : engine;
  return compareApi(capped, engineMin) < 0 ? engineMin : capped;
}

// "ghcr.io/jarvissax/rondo-server:latest" → repository and tag ("latest" when none is named)
export function splitImage(ref: string): { repo: string; tag: string } {
  const at = ref.indexOf("@");
  if (at >= 0) return { repo: ref.slice(0, at), tag: ref.slice(at + 1) };
  const colon = ref.lastIndexOf(":");
  if (colon > ref.lastIndexOf("/")) return { repo: ref.slice(0, colon), tag: ref.slice(colon + 1) };
  return { repo: ref, tag: "latest" };
}

export class Docker {
  private version = "";

  constructor(private readonly socketPath = "/var/run/docker.sock") {}

  get apiVersion(): string {
    return this.version;
  }

  private raw(method: string, path: string, body?: unknown, timeoutMs = 120_000): Promise<{ status: number; text: string }> {
    return new Promise((resolve, reject) => {
      const data = body === undefined ? undefined : JSON.stringify(body);
      const req = request(
        {
          socketPath: this.socketPath,
          method,
          path,
          headers: data ? { "content-type": "application/json", "content-length": Buffer.byteLength(data) } : {},
          timeout: timeoutMs,
        },
        (res) => {
          let text = "";
          res.setEncoding("utf8");
          res.on("data", (chunk: string) => (text += chunk));
          res.on("end", () => resolve({ status: res.statusCode ?? 0, text }));
        },
      );
      req.on("timeout", () => req.destroy(new Error(`Docker took too long to answer ${method} ${path}`)));
      req.on("error", reject);
      if (data) req.write(data);
      req.end();
    });
  }

  async call<T = unknown>(method: string, path: string, body?: unknown, timeoutMs?: number): Promise<T> {
    if (!this.version) {
      const v = JSON.parse((await this.raw("GET", "/version")).text) as { ApiVersion?: string; MinAPIVersion?: string };
      this.version = pickApiVersion(v.ApiVersion ?? "1.40", v.MinAPIVersion);
    }
    const res = await this.raw(method, `/v${this.version}${path}`, body, timeoutMs);
    if (res.status >= 400) {
      let message = res.text.trim();
      try {
        message = (JSON.parse(res.text) as { message?: string }).message ?? message;
      } catch {
        // not JSON
      }
      throw new DockerError(res.status, `${method} ${path}: ${message || res.status}`);
    }
    if (!res.text) return null as T;
    try {
      return JSON.parse(res.text) as T;
    } catch {
      return res.text as T;
    }
  }

  // pulls an image; the engine answers with a stream of JSON lines, the last of which carries any error
  async pull(ref: string): Promise<void> {
    const { repo, tag } = splitImage(ref);
    const text = await this.call<string>("POST", `/images/create?fromImage=${encodeURIComponent(repo)}&tag=${encodeURIComponent(tag)}`, undefined, 30 * 60_000);
    const lines = (typeof text === "string" ? text : JSON.stringify(text)).split("\n").filter(Boolean);
    for (const line of lines) {
      try {
        const msg = JSON.parse(line) as { error?: string };
        if (msg.error) throw new DockerError(500, `pulling ${ref}: ${msg.error}`);
      } catch (e) {
        if (e instanceof DockerError) throw e;
      }
    }
  }
}
