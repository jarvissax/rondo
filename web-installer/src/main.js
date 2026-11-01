// The installer page: connects to the Y1 over WebUSB with Tango, then runs the steps in
// installer.js. The APK comes from this site (rondo.apk and latest.json, put next to the page when
// it's published, see .github/workflows/pages.yml), or from a file the user picks.
import { Adb, AdbDaemonTransport } from "@yume-chan/adb";
import AdbWebCredentialStore from "@yume-chan/adb-credential-web";
import { AdbDaemonWebUsbDeviceManager } from "@yume-chan/adb-daemon-webusb";
import { checkDevice, installApk, InstallError, makeHome, uninstall } from "./installer.js";

const $ = (id) => document.getElementById(id);
const manager = AdbDaemonWebUsbDeviceManager.BROWSER;
// the Y1 doesn't ask to allow a computer (its firmware has adb authorization off), but Tango
// wants a key store anyway
const credentialStore = new AdbWebCredentialStore("Rondo installer");

let adb = null;
let device = null; // { installed }
let apk = null; // { bytes, label }
let busy = false;

function status(id, text, kind = "") {
  const el = $(id);
  el.textContent = text;
  el.className = "status" + (kind ? " " + kind : "");
}

function setStep(active) {
  const order = ["stepConnect", "stepInstall", "stepDone"];
  order.forEach((id, i) => {
    const el = $(id);
    const at = order.indexOf(active);
    el.classList.toggle("active", i === at);
    el.classList.toggle("waiting", i > at);
    el.querySelector(".tick").hidden = !(i < at || (active === "stepDone" && i === at));
  });
}

function refreshInstall() {
  $("installBtn").disabled = busy || !adb || !apk;
  $("connectBtn").disabled = busy || !manager;
  if (!apk) return;
  let line = `Ready to install <b>${apk.label}</b>.`;
  if (device?.installed) line += ` This Y1 has Rondo ${device.installed} now; it will be replaced, and its settings kept.`;
  $("apkLine").innerHTML = line;
}

// --- the APK ---

async function loadBundledApk() {
  try {
    const info = await (await fetch("latest.json", { cache: "no-store" })).json();
    const res = await fetch(info.file || "rondo.apk", { cache: "no-store" });
    if (!res.ok) throw new Error(String(res.status));
    apk = { bytes: new Uint8Array(await res.arrayBuffer()), label: `Rondo ${info.version}` };
  } catch {
    $("apkLine").textContent = "Couldn't load Rondo from this page. Download rondo-<version>.apk from GitHub's Releases page and choose it with \"Use an APK file\".";
  }
  refreshInstall();
}

$("apkFile").addEventListener("change", async (e) => {
  const file = e.target.files?.[0];
  if (!file) return;
  apk = { bytes: new Uint8Array(await file.arrayBuffer()), label: file.name };
  refreshInstall();
});

// --- connecting ---

function explainConnectError(e) {
  const text = String(e?.message || e);
  if (/claim|busy|in use|Access denied|SecurityError|NetworkError/i.test(text)) {
    return "Another program is using the Y1's USB connection. Close the Innioasis Updater, run \"adb kill-server\" if you use adb, then unplug the Y1, plug it back in and try again.";
  }
  return `Couldn't connect to the Y1 (${text}). Unplug it, plug it back in and try again.`;
}

async function connect() {
  busy = true;
  refreshInstall();
  status("connectStatus", "Choose the Y1 in the browser's list…");
  try {
    const usb = await manager.requestDevice();
    if (!usb) {
      status("connectStatus", "No device chosen. If the Y1 wasn't in the list, see the tips at the bottom of this page.");
      return;
    }
    status("connectStatus", "Connecting…");
    const connection = await usb.connect();
    const transport = await AdbDaemonTransport.authenticate({ serial: usb.serial, connection, credentialStore });
    adb = new Adb(transport);
    adb.disconnected.then(() => {
      if (!adb) return;
      adb = null;
      if (!$("stepDone").classList.contains("active")) {
        setStep("stepConnect");
        status("connectStatus", "The Y1 was disconnected. Plug it back in and connect again.", "error");
      }
      refreshInstall();
    });
    device = await checkDevice(adb);
    status("connectStatus", "Connected to your Y1 (Android 4.2).", "ok");
    setStep("stepInstall");
  } catch (e) {
    await adb?.close().catch(() => {});
    adb = null;
    status("connectStatus", e instanceof InstallError ? e.message : explainConnectError(e), "error");
  } finally {
    busy = false;
    refreshInstall();
  }
}

// --- installing ---

async function install({ replace = false } = {}) {
  busy = true;
  $("otherBuild").hidden = true;
  refreshInstall();
  const bar = $("bar");
  $("progress").hidden = false;
  bar.style.width = "0";
  try {
    if (replace) {
      status("installStatus", "Removing the old Rondo…");
      await uninstall(adb);
    }
    status("installStatus", "Copying Rondo to the Y1…");
    await installApk(adb, apk.bytes, (p) => {
      bar.style.width = `${Math.round(p * 100)}%`;
      if (p >= 1) status("installStatus", "Installing… this takes about 10 seconds.");
    });
    status("installStatus", "Making Rondo the home screen…");
    const home = await makeHome(adb);
    $("doneText").textContent = home === "ask"
      ? "Rondo is installed. If the Y1 shows \"Complete action using\", choose Rondo, then Always (or click Install again and this page will do it)."
      : "Rondo is installed, and the Y1 will start in it from now on.";
    status("installStatus", "Installed.", "ok");
    setStep("stepDone");
    await adb.close().catch(() => {});
    adb = null;
  } catch (e) {
    if (e instanceof InstallError && e.kind === "other-build") {
      status("installStatus", e.message, "error");
      $("otherBuild").hidden = false;
    } else {
      status("installStatus", e instanceof InstallError ? e.message : `Something went wrong: ${e?.message || e}. Check the cable and try again.`, "error");
    }
  } finally {
    busy = false;
    refreshInstall();
  }
}

// --- start ---

if (!manager) {
  $("unsupported").hidden = false;
  $("connectBtn").disabled = true;
} else {
  $("connectBtn").addEventListener("click", connect);
  $("installBtn").addEventListener("click", () => install());
  $("replaceBtn").addEventListener("click", () => install({ replace: true }));
}
setStep("stepConnect");
loadBundledApk();
