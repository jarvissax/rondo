// The update page: connects to the Y1 over WebUSB with Tango, then runs the steps in updater.js.
// The APK comes from this site (rondo.apk and latest.json, put next to the page when a release is
// published, see .github/workflows/release.yml), or from a file the user picks. Its texts are in
// lang/*.json's "updater" section (lang.js).
import { Adb, AdbDaemonTransport } from "@yume-chan/adb";
import AdbWebCredentialStore from "@yume-chan/adb-credential-web";
import { AdbDaemonWebUsbDeviceManager } from "@yume-chan/adb-daemon-webusb";
import { applyLang, html, langPicker, onLang, te, th } from "./lang.js";
import { checkDevice, installApk, installBundle, UpdateError, waitForRondo } from "./updater.js";

const $ = (id) => document.getElementById(id);
const manager = AdbDaemonWebUsbDeviceManager.BROWSER;
// the Y1 doesn't ask to allow a computer (its firmware has adb authorization off), but Tango
// wants a key store anyway
const credentialStore = new AdbWebCredentialStore("Rondo updater");

let adb = null;
let device = null; // { installed, firmware }
let apk = null; // { bytes, version, firmware, file } (file: the name of a file the user chose)
let apkFailed = false;
const UPDATER_GUIDE = "https://github.com/jarvissax/rondo/blob/main/docs/install.md#updating-rondo";
const INSTALL_GUIDE = "https://github.com/jarvissax/rondo/blob/main/docs/install.md";
const link = (href, text) => html(`<a href="${href}">${text}</a>`);
let busy = false;

// the texts this script puts on the page, each kept as a function that makes its HTML, so a change
// of language says them again
const texts = new Map();
function put(id, make) {
  const el = $(id);
  el.removeAttribute("data-i18n");
  el.removeAttribute("data-i18n-html");
  texts.set(el, make);
  el.innerHTML = make();
}
onLang(() => { for (const [el, make] of texts) el.innerHTML = make(); });

function status(id, make, kind = "") {
  $(id).className = "status" + (kind ? " " + kind : "");
  put(id, make);
}

function setStep(active) {
  const order = ["stepConnect", "stepUpdate", "stepDone"];
  order.forEach((id, i) => {
    const el = $(id);
    const at = order.indexOf(active);
    el.classList.toggle("active", i === at);
    el.classList.toggle("waiting", i > at);
    el.querySelector(".tick").hidden = !(i < at || (active === "stepDone" && i === at));
  });
}

// the {name} blank: a release's "Rondo 4.1.0", or the chosen file's name (th() escapes that)
const apkName = () => (apk.file != null ? apk.file : html(te("update.rondoVersion", { version: apk.version })));

function refreshUpdate() {
  // a release for a newer firmware than this Y1's (a file of unknown origin can't be told apart)
  const needsFirmware = !!device && !!apk?.firmware && apk.firmware > device.firmware;
  $("updateBtn").disabled = busy || !adb || !apk || needsFirmware;
  $("connectBtn").disabled = busy || !manager;
  if (apkFailed && !apk) return put("apkLine", () => te("update.couldntLoad"));
  if (!apk) return;
  if (needsFirmware) {
    put("apkLine", () => th("update.needsFirmware", { name: apkName(), link: link(UPDATER_GUIDE, "Innioasis Updater CE") }));
    return;
  }
  const same = device?.installed && apk.version && device.installed === apk.version;
  put("updateBtn", () => (same ? te("update.again") : te("update.button")));
  const installed = device?.installed;
  if (!installed) put("apkLine", () => th("update.ready", { name: apkName() }));
  else if (same) put("apkLine", () => th("update.readySame", { name: apkName() }));
  else put("apkLine", () => th("update.readyHas", { name: apkName(), version: installed }));
}

// --- the APK ---

async function loadBundledApk() {
  try {
    const info = await (await fetch("latest.json", { cache: "no-store" })).json();
    const res = await fetch(info.file || "rondo.apk", { cache: "no-store" });
    if (!res.ok) throw new Error(String(res.status));
    apk = { bytes: new Uint8Array(await res.arrayBuffer()), version: info.version, firmware: Number(info.firmware) || 1 };
  } catch {
    apkFailed = true;
  }
  refreshUpdate();
}

$("apkFile").addEventListener("change", async (e) => {
  const file = e.target.files?.[0];
  if (!file) return;
  apk = { bytes: new Uint8Array(await file.arrayBuffer()), file: file.name, version: file.name.match(/rondo-([\w.]+)\.(apk|rondo)$/i)?.[1] };
  refreshUpdate();
});

// --- what went wrong, in the page's language ---

function errorText(e) {
  const d = e.details;
  switch (e.kind) {
    case "not-y1": return d.model ? te("error.notY1", { model: d.model }) : te("error.notY1Unknown");
    case "no-firmware": return te("error.noFirmware");
    case "other-key": return te("error.otherKey");
    case "older": return te("error.older");
    case "android-install": return te("error.androidDidntInstall", { reason: d.reason });
    case "rondo-install": return te("error.rondoDidntInstall", { reason: d.reason });
    case "want-apk": return te("update.wantApk");
    case "want-bundle": return te("update.wantBundle");
    default: return te("update.failed", { reason: e.message });
  }
}

// --- connecting ---

function explainConnectError(e) {
  const text = String(e?.message || e);
  if (/claim|busy|in use|Access denied|SecurityError|NetworkError/i.test(text)) return () => te("connect.busy");
  return () => te("connect.failed", { reason: text });
}

async function connect() {
  busy = true;
  refreshUpdate();
  status("connectStatus", () => te("connect.choose"));
  try {
    const usb = await manager.requestDevice();
    if (!usb) {
      status("connectStatus", () => te("connect.noDevice"));
      return;
    }
    status("connectStatus", () => te("connect.connecting"));
    const connection = await usb.connect();
    const transport = await AdbDaemonTransport.authenticate({ serial: usb.serial, connection, credentialStore });
    adb = new Adb(transport);
    adb.disconnected.then(() => {
      if (!adb) return;
      adb = null;
      if (!$("stepDone").classList.contains("active")) {
        setStep("stepConnect");
        status("connectStatus", () => te("connect.disconnected"), "error");
      }
      refreshUpdate();
    });
    device = await checkDevice(adb);
    const version = device.installed;
    status("connectStatus", () => (version ? te("connect.connected", { version }) : te("connect.connectedFirmware")), "ok");
    setStep("stepUpdate");
  } catch (e) {
    await adb?.close().catch(() => {});
    adb = null;
    status("connectStatus", e instanceof UpdateError ? () => errorText(e) : explainConnectError(e), "error");
  } finally {
    busy = false;
    refreshUpdate();
  }
}

// --- updating ---

async function update() {
  busy = true;
  refreshUpdate();
  const bar = $("bar");
  $("progress").hidden = false;
  bar.style.width = "0";
  try {
    status("updateStatus", () => te("update.copying"));
    // Rondo 2.0's firmware takes a bundle (a file that begins "RONDOB1"), 1.0.x an APK
    const native = (device?.firmware ?? 1) >= 2;
    const isBundle = apk.bytes.length > 8 && new TextDecoder().decode(apk.bytes.subarray(0, 8)) === "RONDOB1\n";
    if (native !== isBundle) throw new UpdateError(native ? "want-bundle" : "want-apk");
    await (native ? installBundle : installApk)(adb, apk.bytes, (p) => {
      bar.style.width = `${Math.round(p * 100)}%`;
      if (p >= 1) status("updateStatus", () => te("update.installing"));
    });
    status("updateStatus", () => te("update.waiting"));
    const back = await waitForRondo(adb, native);
    put("doneText", () => (back ? te("done.running") : te("done.dark")));
    status("updateStatus", () => te("update.done"), "ok");
    setStep("stepDone");
    await adb.close().catch(() => {});
    adb = null;
  } catch (e) {
    const reason = String(e?.message || e);
    status("updateStatus", e instanceof UpdateError ? () => errorText(e) : () => te("update.failed", { reason }), "error");
  } finally {
    busy = false;
    refreshUpdate();
  }
}

// --- start ---

// the page's texts with links in them (the HTML has their English)
put("firmwareNote", () => th("page.firmwareOnly", { link: link(INSTALL_GUIDE, "Innioasis Updater CE") }));
put("doneSee", () => th("done.seeWhatsNew", { link: link("https://github.com/jarvissax/rondo/releases/latest", te("done.whatsNew")) }));
put("helpWindows", () => th("help.windows", { link: link("https://developer.android.com/studio/run/win-usb", te("help.windowsDriver")) }));
put("helpAdb", () => th("help.adb", { link: link("https://github.com/jarvissax/rondo/blob/main/docs/building.md#the-update-bundle", te("help.adbGuide")) }));
put("footerLicense", () => th("footer.license", { link: link("https://github.com/jarvissax/rondo", "Rondo") }));
langPicker($("langPick"), $("langSelect"));
applyLang();

if (!manager) {
  $("unsupported").hidden = false;
  $("connectBtn").disabled = true;
} else {
  $("connectBtn").addEventListener("click", connect);
  $("updateBtn").addEventListener("click", update);
}
setStep("stepConnect");
loadBundledApk();
