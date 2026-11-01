// The install steps, the same ones tools/install.sh runs: check that this is a Y1 on Android 4.2,
// copy the APK over and install it, then make Rondo the home screen by tapping through Android's
// "Complete action using" dialog (the click wheel can't move through Android's own lists).
// Nothing else on the Y1 is changed. Works on any Tango `Adb`, in the browser or in Node.
import { ReadableStream } from "@yume-chan/stream-extra";

export const PACKAGE = "com.jarvissax.rondo";
const TMP_APK = "/data/local/tmp/rondo-install.apk";
const TMP_UI = "/data/local/tmp/rondo-ui.xml";

export class InstallError extends Error {
  constructor(kind, message) {
    super(message);
    this.kind = kind; // "not-y1" | "old-android" | "other-build" | "install" | "home"
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

export async function checkDevice(adb) {
  const model = await getProp(adb, "ro.product.model");
  const sdk = await getProp(adb, "ro.build.version.sdk");
  if (model !== "Y1") throw new InstallError("not-y1", `This device is a "${model || "unknown"}", not an Innioasis Y1.`);
  if (sdk !== "17") throw new InstallError("old-android", "This Y1 isn't running Android 4.2. Flash \"Android 4.2 (Dev)\" first.");
  const installed = await installedVersion(adb);
  return { model, installed };
}

// the version of Rondo already on the Y1, or null
export async function installedVersion(adb) {
  const out = await shell(adb, `dumpsys package ${PACKAGE} | grep versionName`);
  return out.match(/versionName=(\S+)/)?.[1] ?? null;
}

// copies the APK to the Y1 (progress 0..1 as it goes) and installs it over any earlier Rondo
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
  if (/INCONSISTENT_CERTIFICATES|ALREADY_EXISTS|UPDATE_INCOMPATIBLE/.test(reason)) {
    throw new InstallError("other-build", "A different build of Rondo is already installed, so this one can't replace it.");
  }
  throw new InstallError("install", `Android didn't install it (${reason}).`);
}

// removes Rondo and its settings (for replacing a build signed with another key)
export async function uninstall(adb) {
  await shell(adb, `pm uninstall ${PACKAGE}`);
}

async function focus(adb) {
  return shell(adb, "dumpsys window | grep mCurrentFocus");
}

async function tapText(adb, text) {
  const xml = await shell(adb, `uiautomator dump ${TMP_UI} >/dev/null 2>&1; cat ${TMP_UI}; rm -f ${TMP_UI}`);
  const node = xml.match(new RegExp(`text="${text}"[^>]*bounds="\\[(\\d+),(\\d+)\\]\\[(\\d+),(\\d+)\\]"`));
  if (!node) return false;
  const [x1, y1, x2, y2] = node.slice(1).map(Number);
  await shell(adb, `input tap ${Math.round((x1 + x2) / 2)} ${Math.round((y1 + y2) / 2)}`);
  return true;
}

// "already" when Rondo is the home screen, "set" when it was just chosen, "ask" when the Y1 shows
// the dialog and the user has to finish it
export async function makeHome(adb) {
  await shell(adb, "am start -a android.intent.action.MAIN -c android.intent.category.HOME");
  // a freshly installed Rondo takes a few seconds to start
  let now = "";
  for (let i = 0; i < 10; i++) {
    await sleep(1000);
    now = await focus(adb);
    if (now.includes(PACKAGE) || now.includes("ResolverActivity")) break;
  }
  if (now.includes(PACKAGE)) return "already";
  if (now.includes("ResolverActivity") && (await tapText(adb, "Rondo"))) {
    await sleep(1000);
    if (await tapText(adb, "Always")) {
      for (let i = 0; i < 8; i++) {
        await sleep(1000);
        if ((await focus(adb)).includes(PACKAGE)) return "set";
      }
    }
  }
  return "ask";
}
