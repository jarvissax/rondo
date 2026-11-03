// The update steps: check that this is a Y1 running the Rondo firmware, copy the new Rondo over
// and install it over the firmware's copy (`pm install -r`, which keeps every setting). Android
// accepts only an APK signed with the same key as the firmware's Rondo, then restarts Rondo by
// itself. Works on any Tango `Adb`, in the browser or in Node.
import { ReadableStream } from "@yume-chan/stream-extra";

export const PACKAGE = "com.jarvissax.rondo";
const TMP_APK = "/data/local/tmp/rondo-update.apk";

export class UpdateError extends Error {
  constructor(kind, message) {
    super(message);
    this.kind = kind; // "not-y1" | "no-firmware" | "other-build" | "older" | "install"
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// Android 4.2's adb only has the old "shell:" service (Tango's spawn uses "exec:", from Android 5);
// it runs through a terminal, so lines end in \r\n
async function shell(adb, command) {
  const socket = await adb.createSocket(`shell:${command}`);
  const reader = socket.readable.getReader();
  const decoder = new TextDecoder();
  let text = "";
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    text += decoder.decode(value, { stream: true });
  }
  return (text + decoder.decode()).replace(/\r/g, "");
}

const getProp = async (adb, key) => (await shell(adb, `getprop ${key}`)).trim();

// a Y1 whose firmware is Rondo's (its build name is "Rondo <version>"), and that firmware's number:
// an update for a newer firmware installs only with Innioasis Updater CE (firmwareLevel in
// app/app/build.gradle.kts; the first Rondo firmware, 1.0.0, had no number)
export async function checkDevice(adb) {
  const model = await getProp(adb, "ro.product.model");
  if (model !== "Y1") throw new UpdateError("not-y1", `This device is a "${model || "unknown"}", not an Innioasis Y1.`);
  const build = await getProp(adb, "ro.build.display.id");
  if (!build.startsWith("Rondo")) {
    throw new UpdateError("no-firmware", "This Y1 doesn't run the Rondo firmware yet. Install it with Innioasis Updater CE first (see the install guide), then come back for updates.");
  }
  const firmware = Number(await getProp(adb, "ro.rondo.firmware")) || 1;
  return { model, firmware, installed: await installedVersion(adb) };
}

// the version of Rondo on the Y1, or null
export async function installedVersion(adb) {
  const out = await shell(adb, `dumpsys package ${PACKAGE} | grep versionName`);
  return out.match(/versionName=(\S+)/)?.[1] ?? null;
}

// copies the APK to the Y1 (progress 0..1 as it goes) and installs it over Rondo
export async function installApk(adb, bytes, onProgress = () => {}) {
  const CHUNK = 64 * 1024;
  let sent = 0;
  const file = new ReadableStream({
    pull(controller) {
      if (sent >= bytes.length) return controller.close();
      const end = Math.min(sent + CHUNK, bytes.length);
      controller.enqueue(bytes.subarray(sent, end));
      sent = end;
      onProgress(sent / bytes.length);
    },
  });
  const sync = await adb.sync();
  try {
    await sync.write({ filename: TMP_APK, file, permission: 0o644 });
  } finally {
    await sync.dispose();
  }
  const out = await shell(adb, `pm install -r ${TMP_APK}; rm -f ${TMP_APK}`);
  if (/Success/.test(out)) return;
  const reason = out.match(/Failure \[([A-Z_]+)\]/)?.[1] ?? out.trim().split("\n").pop();
  if (/INCONSISTENT_CERTIFICATES|UPDATE_INCOMPATIBLE/.test(reason)) {
    throw new UpdateError("other-build", "That APK isn't a Rondo release: it's signed with a different key than the Rondo on this Y1.");
  }
  if (/VERSION_DOWNGRADE/.test(reason)) throw new UpdateError("older", "That's an older Rondo than the one on this Y1.");
  throw new UpdateError("install", `Android didn't install it (${reason}).`);
}

// Android restarts Rondo by itself after the update; true once it's on screen again
export async function waitForRondo(adb) {
  for (let i = 0; i < 20; i++) {
    await sleep(1000);
    if ((await shell(adb, "dumpsys window | grep mCurrentFocus")).includes(PACKAGE)) return true;
  }
  return false;
}
