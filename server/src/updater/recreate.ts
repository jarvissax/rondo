// Swapping a container onto a newer image, the way `docker compose up` would: same name, settings,
// volumes, ports and networks, with only the image's own defaults (its environment, command,
// labels…) taken from the new image. The old container waits, stopped and renamed, until the new
// one answers; if it doesn't, the old one comes back as it was.

import { compareApi, type Docker } from "./docker.js";

type Json = Record<string, unknown>;

export interface ContainerInfo {
  Id: string;
  Name: string;
  Image: string; // the image ID it runs
  Config: Json & { Image?: string; Env?: string[]; Labels?: Record<string, string>; Hostname?: string };
  HostConfig: Json & { NetworkMode?: string };
  NetworkSettings?: { Networks?: Record<string, Json & { Aliases?: string[] | null }> };
  State?: { Running?: boolean; Status?: string; ExitCode?: number };
  RestartCount?: number;
}

export interface ImageInfo {
  Id: string;
  Config?: (Json & { Env?: string[]; Labels?: Record<string, string> }) | null;
}

// settings a container takes from its image unless it sets its own
const FROM_IMAGE = ["Cmd", "Entrypoint", "WorkingDir", "User", "Healthcheck", "ExposedPorts", "Volumes", "StopSignal", "Shell", "OnBuild"];

const same = (a: unknown, b: unknown) => JSON.stringify(a ?? null) === JSON.stringify(b ?? null);

// what to create the new container with, and the networks to join after creating it (engines
// before API 1.44 take only one network at creation)
export function createBody(
  c: ContainerInfo,
  oldImage: ImageInfo,
  imageRef: string,
  apiVersion: string,
): { body: Json; extraNetworks: { name: string; endpoint: Json }[] } {
  const image = oldImage.Config ?? {};
  const config: Json = structuredClone(c.Config);
  config.Image = imageRef;
  // what the container adds to its image, so the new image's own defaults apply
  config.Env = (c.Config.Env ?? []).filter((e) => !(image.Env ?? []).includes(e));
  const labels: Record<string, string> = {};
  for (const [k, v] of Object.entries(c.Config.Labels ?? {})) if (image.Labels?.[k] !== v) labels[k] = v;
  config.Labels = labels;
  for (const key of FROM_IMAGE) if (same(config[key], image[key])) delete config[key];
  // Docker names a container's host after its short ID unless told otherwise
  if (c.Config.Hostname === c.Id.slice(0, 12)) delete config.Hostname;

  const shortId = c.Id.slice(0, 12);
  const mode = c.HostConfig.NetworkMode ?? "";
  const networks = Object.entries(c.NetworkSettings?.Networks ?? {}).map(([name, n]) => {
    const endpoint: Json = {};
    const aliases = (n.Aliases ?? []).filter((a) => a !== shortId && a !== c.Id);
    if (aliases.length) endpoint.Aliases = aliases;
    for (const key of ["IPAMConfig", "Links", "DriverOpts", "MacAddress"]) if (n[key]) endpoint[key] = n[key];
    return { name, endpoint };
  });
  // host, none, container:… : no networks of their own
  const ownNetworks = mode === "host" || mode === "none" || mode.startsWith("container:") ? [] : networks;
  const multi = compareApi(apiVersion, "1.44") >= 0;
  const primary = ownNetworks.find((n) => n.name === mode) ?? ownNetworks[0];
  const atCreate = multi ? ownNetworks : primary ? [primary] : [];
  const body: Json = {
    ...config,
    HostConfig: c.HostConfig,
    NetworkingConfig: { EndpointsConfig: Object.fromEntries(atCreate.map((n) => [n.name, n.endpoint])) },
  };
  return { body, extraNetworks: ownNetworks.filter((n) => !atCreate.includes(n)) };
}

export type Progress = (state: "pulling" | "restarting", message: string) => void;

export interface UpdateResult {
  updated: boolean;
  message: string;
  from: string; // image IDs
  to: string;
}

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

// waits until the container runs and `answers()` says yes; false once it has stopped or restarted
// (its restart policy brings a crashing server back again and again), or after `timeoutMs`
async function comesUp(docker: Docker, id: string, answers: () => Promise<boolean>, timeoutMs: number): Promise<boolean> {
  const end = Date.now() + timeoutMs;
  while (Date.now() < end) {
    const c = await docker.call<ContainerInfo>("GET", `/containers/${id}/json`);
    if (["exited", "dead", "restarting"].includes(c.State?.Status ?? "") || (c.RestartCount ?? 0) > 0) return false;
    if (c.State?.Running && (await answers().catch(() => false))) return true;
    await sleep(2000);
  }
  return false;
}

// pulls the image `target` runs (by name, such as ghcr.io/jarvissax/rondo-server:latest) and, when
// it has changed, moves `target` onto it
export async function updateContainer(
  docker: Docker,
  target: string,
  opts: { answers: () => Promise<boolean>; timeoutMs?: number; progress?: Progress },
): Promise<UpdateResult> {
  const progress = opts.progress ?? (() => {});
  const c = await docker.call<ContainerInfo>("GET", `/containers/${encodeURIComponent(target)}/json`);
  const ref = c.Config.Image ?? "";
  if (!ref || ref.startsWith("sha256:")) throw new Error(`${target} runs an image without a name, so there's nothing to pull`);
  // the old image's defaults, before the pull moves its name: Docker's containerd image store
  // stops showing an image by its ID once nothing names it
  const oldImage = await docker.call<ImageInfo>("GET", `/images/${c.Image}/json`).catch(() => null);
  progress("pulling", `Downloading ${ref}`);
  await docker.pull(ref);
  const next = await docker.call<ImageInfo>("GET", `/images/${encodeURIComponent(ref)}/json`);
  if (next.Id === c.Image) return { updated: false, message: "Already the newest image", from: c.Image, to: next.Id };

  const name = c.Name.replace(/^\//, "");
  // without the old image, the new one's defaults are the closest guess at what came from an image
  const { body, extraNetworks } = createBody(c, oldImage ?? next, ref, docker.apiVersion);

  progress("restarting", `Restarting ${name} on the new image`);
  await docker.call("POST", `/containers/${c.Id}/stop?t=20`, undefined, 60_000);
  await docker.call("POST", `/containers/${c.Id}/rename?name=${encodeURIComponent(`${name}-previous`)}`);
  let created: string | null = null;
  try {
    created = (await docker.call<{ Id: string }>("POST", `/containers/create?name=${encodeURIComponent(name)}`, body)).Id;
    for (const n of extraNetworks) {
      await docker.call("POST", `/networks/${encodeURIComponent(n.name)}/connect`, { Container: created, EndpointConfig: n.endpoint });
    }
    await docker.call("POST", `/containers/${created}/start`);
    if (!(await comesUp(docker, created, opts.answers, opts.timeoutMs ?? 120_000))) throw new Error("the new server didn't start answering");
  } catch (err) {
    // back to the old container, as it was
    if (created) await docker.call("DELETE", `/containers/${created}?force=true`).catch(() => {});
    await docker.call("POST", `/containers/${c.Id}/rename?name=${encodeURIComponent(name)}`).catch(() => {});
    await docker.call("POST", `/containers/${c.Id}/start`).catch(() => {});
    throw err;
  }
  await docker.call("DELETE", `/containers/${c.Id}`).catch(() => {});
  // the old image, unless something else still uses it
  await docker.call("DELETE", `/images/${c.Image}`).catch(() => {});
  return { updated: true, message: "Updated", from: c.Image, to: next.Id };
}
