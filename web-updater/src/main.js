// The update page: connects to the Y1 over WebUSB with Tango, then runs the steps in updater.js.
// The APK comes from this site (rondo.apk and latest.json, put next to the page when a release is
// published, see .github/workflows/release.yml), or from a file the user picks.
import { Adb, AdbDaemonTransport } from "@yume-chan/adb";
import AdbWebCredentialStore from "@yume-chan/adb-credential-web";
import { AdbDaemonWebUsbDeviceManager } from "@yume-chan/adb-daemon-webusb";
import { checkDevice, installApk, installBundle, UpdateError, waitForRondo } from "./updater.js";

const $ = (id) => document.getElementById(id);
const manager = AdbDaemonWebUsbDeviceManager.BROWSER;
// the Y1 doesn't ask to allow a computer (its firmware has adb authorization off), but Tango
// wants a key store anyway
const credentialStore = new AdbWebCredentialStore("Rondo updater");

let adb = null;
let device = null; // { installed, firmware }
let apk = null; // { bytes, label, version, firmware }
const UPDATER_GUIDE = "https://github.com/jarvissax/rondo/blob/main/docs/install.md#updating-rondo";
let busy = false;

function status(id, text, kind = "") {
  const el = $(id);
  el.textContent = text;
  el.className = "status" + (kind ? " " + kind : "");
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

function refreshUpdate() {
  // a release for a newer firmware than this Y1's (a file of unknown origin can't be told apart)
  const needsFirmware = !!device && !!apk?.firmware && apk.firmware > device.firmware;
  $("updateBtn").disabled = busy || !adb || !apk || needsFirmware;
  $("connectBtn").disabled = busy || !manager;
  if (!apk) return;
  if (needsFirmware) {
    $("apkLine").innerHTML = `<b>${apk.label}</b> needs a new firmware, so it installs with <a href="${UPDATER_GUIDE}">Innioasis Updater CE</a> instead. Rondo on this Y1 stays as it is.`;
    return;
  }
  const same = device?.installed && apk.version && device.installed === apk.version;
  $("updateBtn").textContent = same ? "Install again" : "Update";
  let line = `Ready: <b>${apk.label}</b>.`;
  if (device?.installed) line += same ? ` This Y1 already has it.` : ` This Y1 has Rondo ${device.installed}.`;
  $("apkLine").innerHTML = line;
}

// --- the APK ---

async function loadBundledApk() {
  try {
    const info = await (await fetch("latest.json", { cache: "no-store" })).json();
    const res = await fetch(info.file || "rondo.apk", { cache: "no-store" });
    if (!res.ok) throw new Error(String(res.status));
    apk = { bytes: new Uint8Array(await res.arrayBuffer()), label: `Rondo ${info.version}`, version: info.version, firmware: Number(info.firmware) || 1 };
  } catch {
    $("apkLine").textContent = "Couldn't load Rondo from this page. Download rondo-<version>.rondo (Rondo 2.0) or rondo-<version>.apk (1.0) from GitHub's Releases page and choose it with \"Use a file\".";
  }
  refreshUpdate();
}

$("apkFile").addEventListener("change", async (e) => {
  const file = e.target.files?.[0];
  if (!file) return;
  apk = { bytes: new Uint8Array(await file.arrayBuffer()), label: file.name, version: file.name.match(/rondo-([\w.]+)\.(apk|rondo)$/i)?.[1] };
  refreshUpdate();
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
  refreshUpdate();
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
      refreshUpdate();
    });
    device = await checkDevice(adb);
    status("connectStatus", `Connected to your Y1 (Rondo ${device.installed ?? "firmware"}).`, "ok");
    setStep("stepUpdate");
  } catch (e) {
    await adb?.close().catch(() => {});
    adb = null;
    status("connectStatus", e instanceof UpdateError ? e.message : explainConnectError(e), "error");
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
    status("updateStatus", "Copying Rondo to the Y1…");
    // Rondo 2.0's firmware takes a bundle (a file that begins "RONDOB1"), 1.0.x an APK
    const native = (device?.firmware ?? 1) >= 2;
    const isBundle = apk.bytes.length > 8 && new TextDecoder().decode(apk.bytes.subarray(0, 8)) === "RONDOB1\n";
    if (native !== isBundle) {
      throw new UpdateError("other-build", native ? "This Y1 runs Rondo 2.0: choose a rondo-<version>.rondo file." : "This Y1 runs Rondo 1.0: choose a rondo-<version>.apk file.");
    }
    await (native ? installBundle : installApk)(adb, apk.bytes, (p) => {
      bar.style.width = `${Math.round(p * 100)}%`;
      if (p >= 1) status("updateStatus", "Installing… this takes about 10 seconds.");
    });
    status("updateStatus", "Installed. Waiting for Rondo to start again…");
    const back = await waitForRondo(adb, native);
    $("doneText").textContent = back
      ? `Rondo is updated and running again, with your settings, Wi-Fi, playlists and music as they were.`
      : `Rondo is updated. If the Y1's screen is dark, press a button to wake it.`;
    status("updateStatus", "Updated.", "ok");
    setStep("stepDone");
    await adb.close().catch(() => {});
    adb = null;
  } catch (e) {
    status("updateStatus", e instanceof UpdateError ? e.message : `Something went wrong: ${e?.message || e}. Check the cable and try again.`, "error");
  } finally {
    busy = false;
    refreshUpdate();
  }
}

// --- start ---

if (!manager) {
  $("unsupported").hidden = false;
  $("connectBtn").disabled = true;
} else {
  $("connectBtn").addEventListener("click", connect);
  $("updateBtn").addEventListener("click", update);
}
setStep("stepConnect");
loadBundledApk();
