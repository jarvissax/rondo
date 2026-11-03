import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { buildApp } from "../src/app.js";
import { loadConfig } from "../src/config.js";
import { ServerUpdate } from "../src/serverUpdate.js";
import { compareApi, type Docker, pickApiVersion, splitImage } from "../src/updater/docker.js";
import { type ContainerInfo, createBody, type ImageInfo, updateContainer } from "../src/updater/recreate.js";

describe("Docker API versions", () => {
  it("speaks the engine's version, capped at ours and never under its minimum", () => {
    expect(compareApi("1.44", "1.41")).toBeGreaterThan(0);
    expect(compareApi("1.9", "1.10")).toBeLessThan(0);
    expect(pickApiVersion("1.56", "1.40")).toBe("1.47"); // Docker 29
    expect(pickApiVersion("1.41", "1.12")).toBe("1.41"); // Docker 20.10
    expect(pickApiVersion("1.60", "1.50")).toBe("1.50"); // a future engine that drops 1.47
  });
  it("splits image names into repository and tag", () => {
    expect(splitImage("ghcr.io/jarvissax/rondo-server:latest")).toEqual({ repo: "ghcr.io/jarvissax/rondo-server", tag: "latest" });
    expect(splitImage("ghcr.io/jarvissax/rondo-server")).toEqual({ repo: "ghcr.io/jarvissax/rondo-server", tag: "latest" });
    expect(splitImage("localhost:5000/rondo-server")).toEqual({ repo: "localhost:5000/rondo-server", tag: "latest" });
    expect(splitImage("localhost:5000/rondo-server:1.0.2")).toEqual({ repo: "localhost:5000/rondo-server", tag: "1.0.2" });
  });
});

const IMAGE = "ghcr.io/jarvissax/rondo-server:latest";
const oldImage: ImageInfo = {
  Id: "sha256:old",
  Config: {
    Env: ["PATH=/usr/local/bin:/usr/bin", "NODE_VERSION=22.11.0", "YTDLP_BIN=/data/bin/yt-dlp", "PORT=3000"],
    Entrypoint: ["/entrypoint.sh"],
    WorkingDir: "/app",
    ExposedPorts: { "3000/tcp": {} },
    Labels: { "org.opencontainers.image.version": "1.0.1" },
  },
};
const container = (networks: Record<string, { Aliases?: string[] | null }>, mode = "rondo_default"): ContainerInfo => ({
  Id: "abcdef1234567890abcdef",
  Name: "/rondo",
  Image: "sha256:old",
  Config: {
    Image: IMAGE,
    Hostname: "abcdef123456",
    Env: [...oldImage.Config!.Env!, "COOKIES_FILE=/data/secrets/cookies.txt", "TRUST_PROXY=0"],
    Entrypoint: ["/entrypoint.sh"],
    WorkingDir: "/app",
    ExposedPorts: { "3000/tcp": {} },
    Labels: { "org.opencontainers.image.version": "1.0.1", "com.docker.compose.project": "rondo", "com.docker.compose.service": "rondo" },
  },
  HostConfig: { NetworkMode: mode, Binds: ["rondo-data:/data"], RestartPolicy: { Name: "unless-stopped" }, PortBindings: { "3000/tcp": [{ HostPort: "3000" }] } },
  NetworkSettings: { Networks: networks },
});

describe("the new container's settings", () => {
  it("keeps what the container adds and leaves the image's own defaults to the new image", () => {
    const { body } = createBody(container({ rondo_default: { Aliases: ["rondo", "abcdef123456"] } }), oldImage, IMAGE, "1.47");
    expect(body.Image).toBe(IMAGE);
    expect(body.Env).toEqual(["COOKIES_FILE=/data/secrets/cookies.txt", "TRUST_PROXY=0"]);
    expect(body.Labels).toEqual({ "com.docker.compose.project": "rondo", "com.docker.compose.service": "rondo" });
    for (const key of ["Entrypoint", "WorkingDir", "ExposedPorts", "Hostname"]) expect(body).not.toHaveProperty(key);
    expect(body.HostConfig).toMatchObject({ Binds: ["rondo-data:/data"], RestartPolicy: { Name: "unless-stopped" } });
    expect(body.NetworkingConfig).toEqual({ EndpointsConfig: { rondo_default: { Aliases: ["rondo"] } } });
  });

  it("keeps a container's own command and hostname", () => {
    const c = container({});
    c.Config.Hostname = "music";
    c.Config.Cmd = ["node", "dist/main.js", "--debug"];
    const { body } = createBody(c, oldImage, IMAGE, "1.47");
    expect(body.Hostname).toBe("music");
    expect(body.Cmd).toEqual(["node", "dist/main.js", "--debug"]);
  });

  it("joins one network at creation on older engines and the rest afterwards", () => {
    const c = container({ proxy: { Aliases: ["y1"] }, rondo_default: { Aliases: null } }, "proxy");
    const old = createBody(c, oldImage, IMAGE, "1.41");
    expect(Object.keys((old.body.NetworkingConfig as { EndpointsConfig: object }).EndpointsConfig)).toEqual(["proxy"]);
    expect(old.extraNetworks.map((n) => n.name)).toEqual(["rondo_default"]);
    const current = createBody(c, oldImage, IMAGE, "1.44");
    expect(Object.keys((current.body.NetworkingConfig as { EndpointsConfig: object }).EndpointsConfig).sort()).toEqual(["proxy", "rondo_default"]);
    expect(current.extraNetworks).toEqual([]);
  });

  it("gives host networking no networks of its own", () => {
    const { body, extraNetworks } = createBody(container({ host: {} }, "host"), oldImage, IMAGE, "1.47");
    expect(body.NetworkingConfig).toEqual({ EndpointsConfig: {} });
    expect(extraNetworks).toEqual([]);
  });
});

// an engine in memory: one container, images by name, and a record of the calls
function fakeDocker(opts: { newId: string; startsOk: boolean }) {
  const calls: string[] = [];
  const containers = new Map<string, ContainerInfo & { State: { Running: boolean; Status: string } }>();
  containers.set("rondo", { ...container({ rondo_default: { Aliases: ["rondo"] } }), State: { Running: true, Status: "running" } });
  const docker = {
    apiVersion: "1.47",
    async pull(ref: string) {
      calls.push(`pull ${ref}`);
    },
    async call(method: string, path: string): Promise<unknown> {
      calls.push(`${method} ${path.split("?")[0]}${path.includes("name=") ? ` ${decodeURIComponent(path.split("name=")[1]!)}` : ""}`);
      if (path.startsWith(`/images/${encodeURIComponent(IMAGE)}/json`)) return { Id: opts.newId };
      if (path.startsWith("/images/sha256:old/json")) return oldImage;
      if (path === "/containers/rondo/json") return containers.get("rondo");
      if (path.startsWith("/containers/create")) return { Id: "new" };
      if (path === "/containers/new/json") return { State: opts.startsOk ? { Running: true, Status: "running" } : { Running: false, Status: "exited" } };
      return null;
    },
  };
  return { docker: docker as unknown as Docker, calls };
}

describe("updating a container", () => {
  it("does nothing when the image hasn't changed", async () => {
    const { docker, calls } = fakeDocker({ newId: "sha256:old", startsOk: true });
    const r = await updateContainer(docker, "rondo", { answers: async () => true });
    expect(r.updated).toBe(false);
    expect(calls.some((c) => c.includes("/stop"))).toBe(false);
  });

  it("swaps onto the new image once the new server answers, then removes the old one", async () => {
    const { docker, calls } = fakeDocker({ newId: "sha256:new", startsOk: true });
    const r = await updateContainer(docker, "rondo", { answers: async () => true });
    expect(r).toMatchObject({ updated: true, from: "sha256:old", to: "sha256:new" });
    const id = "abcdef1234567890abcdef";
    expect(calls).toEqual([
      "GET /containers/rondo/json",
      "GET /images/sha256:old/json",
      `pull ${IMAGE}`,
      `GET /images/${encodeURIComponent(IMAGE)}/json`,
      `POST /containers/${id}/stop`,
      `POST /containers/${id}/rename rondo-previous`,
      "POST /containers/create rondo",
      "POST /containers/new/start",
      "GET /containers/new/json",
      `DELETE /containers/${id}`,
      "DELETE /images/sha256:old",
    ]);
  });

  it("goes on when the old image can no longer be read, using the new one's defaults", async () => {
    const { docker, calls } = fakeDocker({ newId: "sha256:new", startsOk: true });
    const call = docker.call.bind(docker);
    (docker as unknown as { call: typeof call }).call = async (method: string, path: string) => {
      if (path === "/images/sha256:old/json") throw new Error("No such image");
      return call(method, path);
    };
    expect((await updateContainer(docker, "rondo", { answers: async () => true })).updated).toBe(true);
    expect(calls).toContain("POST /containers/create rondo");
  });

  it("brings the old container back when the new one doesn't start", async () => {
    const { docker, calls } = fakeDocker({ newId: "sha256:new", startsOk: false });
    await expect(updateContainer(docker, "rondo", { answers: async () => true })).rejects.toThrow("didn't start answering");
    const id = "abcdef1234567890abcdef";
    expect(calls.slice(-3)).toEqual(["DELETE /containers/new", `POST /containers/${id}/rename rondo`, `POST /containers/${id}/start`]);
  });
});

describe("the server's side", () => {
  const dir = mkdtempSync(join(tmpdir(), "rondo-updater-"));
  const tokenFile = join(dir, "token");
  writeFileSync(tokenFile, "t".repeat(48) + "\n");
  const seen: Array<{ url: string; auth: string | null; method: string }> = [];
  const fakeFetch = (answer: { status: number; body?: unknown } | Error) =>
    (async (url: string | URL, init?: RequestInit) => {
      seen.push({ url: String(url), auth: new Headers(init?.headers).get("authorization"), method: init?.method ?? "GET" });
      if (answer instanceof Error) throw answer;
      return new Response(JSON.stringify(answer.body ?? {}), { status: answer.status });
    }) as typeof fetch;

  it("asks the updater with the shared token", async () => {
    const u = new ServerUpdate("http://rondo-updater:8080/", tokenFile, fakeFetch({ status: 200, body: { state: "idle", message: "" } }));
    expect(await u.status()).toEqual({ available: true, state: "idle", message: "", at: undefined });
    expect(seen.at(-1)).toEqual({ url: "http://rondo-updater:8080/v1/status", auth: `Bearer ${"t".repeat(48)}`, method: "GET" });
  });

  it("isn't available without a token or an updater", async () => {
    expect(await new ServerUpdate("http://x", join(dir, "missing"), fakeFetch({ status: 200 })).status()).toEqual({ available: false });
    expect(await new ServerUpdate("http://x", tokenFile, fakeFetch(new Error("ENOTFOUND"))).status()).toEqual({ available: false });
    expect(await new ServerUpdate("http://x", tokenFile, fakeFetch(new Error("ENOTFOUND"))).start()).toEqual({ error: "unavailable" });
  });

  it("reports a busy updater", async () => {
    expect(await new ServerUpdate("http://x", tokenFile, fakeFetch({ status: 409 })).start()).toEqual({ error: "busy" });
    expect(await new ServerUpdate("http://x", tokenFile, fakeFetch({ status: 202, body: { started: true } })).start()).toEqual({ ok: true });
  });
});

describe("the admin routes", () => {
  const apps: Array<{ close(): Promise<unknown> }> = [];
  afterEach(async () => {
    await Promise.all(apps.splice(0).map((a) => a.close()));
  });

  it("start an update for a signed-in admin only", async () => {
    const dir = mkdtempSync(join(tmpdir(), "rondo-admin-"));
    const config = loadConfig({ YTDLP_BIN: "/nonexistent", LOG_LEVEL: "silent", COOKIES_FILE: join(dir, "c.txt"), CONFIG_FILE: join(dir, "config.json") });
    let started = 0;
    const serverUpdate = { status: async () => ({ available: true, state: "idle" }), start: async () => (started++, { ok: true as const }) } as unknown as ServerUpdate;
    const { app } = await buildApp(config, { newCode: () => "482913", serverUpdate });
    apps.push(app);
    expect((await app.inject({ method: "POST", url: "/admin/api/server/update" })).statusCode).toBe(401);
    const setup = await app.inject({ method: "POST", url: "/admin/api/setup", payload: { code: "482913", password: "correct horse battery" } });
    const cookie = String(setup.headers["set-cookie"]).split(";")[0]!;
    expect((await app.inject({ method: "GET", url: "/admin/api/server/update", headers: { cookie } })).json()).toEqual({ available: true, state: "idle" });
    expect((await app.inject({ method: "POST", url: "/admin/api/server/update", headers: { cookie } })).json()).toEqual({ ok: true });
    expect(started).toBe(1);
  });
});
