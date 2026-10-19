/* Browser model of the current BLE-only firmware; hardware events are injected. */
(() => {
  "use strict";
  const $ = (id) => document.getElementById(id);
  const pages = [...document.querySelectorAll(".screen-page")].filter((page) => page.id !== "notice-dialog");
  const CSV_HEADER = "t_ms,powerW,cadenceRpm,speedMps,vAirMps,yawDeg,postureDeg,rhoKgM3,powerSrc\n";
  const devices = [
    { name: "Assioma Duo", profile: "CPS", address: "C0:00:00:00:00:01", rssi: -48 },
    { name: "Smart Trainer", profile: "FTMS", address: "C0:00:00:00:00:02", rssi: -56 },
    { name: "Cadence Sensor", profile: "CSC cadence only", address: "C0:00:00:00:00:03", rssi: -62 }
  ];
  const files = new Map([
    ["/LOG", { directory: true }],
    ["/music", { directory: true }],
    ["/system", { directory: true }],
    ["/music/demo.mp3", { audio: true }],
    ["/system/boot.wav", { audio: true }],
    ["/readme.txt", {}]
  ]);
  const savedVolume = Number(localStorage.getItem("aero-volume") ?? 55);
  const state = {
    mode: 0, bleConnected: false, bleName: "", power: 230, cadence: 88, bleSpeed: 0,
    virtualPower: 0, virtualCadence: 0, wind: 9.5, windValid: true,
    samples: [], seconds: 0, running: false, page: "dashboard",
    wifiEnabled: false, wifi: "off", wifiEnvironment: "unconfigured", wifiDeadline: 0,
    volume: Number.isFinite(savedVolume) ? Math.max(0, Math.min(100, savedVolume)) : 55,
    gpsEnvironment: "searching", gps: "searching", gpsDeadline: 0, gpsSpeed: 30 / 3.6,
    sdReady: true, sdBusy: false, directory: "/", audio: null, audioDeadline: 0,
    operation: null, bleResult: "success", discoveredDevices: [], manualPairing: false,
    postureEnvironment: "absent", postureConnected: false, nextPostureMs: 30000,
    recording: false, currentRecord: null, logDate: "", pending: [], writer: [], dropped: 0,
    download: null, downloadMessage: "", error: "", extraMs: 0, fileSequence: 0
  };
  const now = () => performance.now() + state.extraMs;
  const source = () => state.mode === 3 ? 3 : state.bleConnected ? 1 : 0;
  const power = () => state.mode === 3 ? state.virtualPower : state.bleConnected ? state.power : 0;
  const cadence = () => state.mode === 3 ? state.virtualCadence : state.bleConnected ? state.cadence : 0;
  const average = (values) => values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : 0;
  const gpsFixed = () => state.gps === "fixed";
  const timeString = (seconds) => [Math.floor(seconds / 3600) % 24, Math.floor(seconds / 60) % 60, seconds % 60]
    .map((value) => String(value).padStart(2, "0")).join(":");
  const utcDate = () => new Date(Date.UTC(2026, 9, 19, 4, 0, 0) + state.seconds * 1000);
  const formattedDate = (date) => date.toISOString().slice(0, 19).replace("T", " ");
  const localDate = () => new Date(utcDate().getTime() + 8 * 3600000);

  function estimateCda() {
    const watts = average(state.samples.slice(-3));
    if (!source() || watts <= 0 || state.wind < 3) return null;
    const aeroPower = watts * 0.975 - 85 * 9.80665 * 0.004 * state.wind;
    return aeroPower > 0 ? aeroPower / (0.5 * 1.225 * state.wind ** 3) : null;
  }
  function zoneFor(cda) {
    if (cda === null) return null;
    const bounds = [0.23, 0.27, 0.31, 0.36, 0.42, 0.50, Infinity];
    const index = bounds.findIndex((bound) => cda < bound);
    return { zone: 7 - index, posture: ["ELITE TT", "RACE TT", "VERY AERO", "AGGRESSIVE", "AERO", "LOW / DROPS", "UPRIGHT"][index] };
  }
  function showPage(id = "dashboard") {
    state.page = id;
    pages.forEach((page) => { page.hidden = page.id !== id; });
    for (const dialog of ["touch-dialog", "virtual-dialog", "ble-loading"]) {
      $(dialog).classList.toggle("open", dialog === id);
      $(dialog).setAttribute("aria-hidden", String(dialog !== id));
    }
    render();
  }
  function notice(message) {
    state.error = message;
    $("notice-message").textContent = message;
    $("notice-dialog").hidden = false;
  }
  function syncInputs() {
    for (const [id, value, suffix] of [
      ["power", state.power, " W"], ["cadence", state.cadence, " rpm"],
      ["virtual-power", state.virtualPower, " W"], ["virtual-cadence", state.virtualCadence, " rpm"]
    ]) {
      $(id + "-input").value = String(value);
      $(id + "-value").textContent = Math.round(value) + suffix;
    }
    $("wind-input").value = String(state.wind * 3.6);
    $("wind-value").textContent = `${(state.wind * 3.6).toFixed(1)} km/h`;
  }
  function render() {
    const cda = estimateCda();
    const zone = zoneFor(cda);
    $("power-reading").textContent = `${Math.round(power())} W`;
    $("cadence-reading").textContent = `CADENCE ${Math.round(cadence())} rpm`;
    $("wind-reading").textContent = state.windValid ? `${(state.wind * 3.6).toFixed(1)} km/h` : "-- km/h";
    $("cda-reading").textContent = cda === null ? "-- m2" : `${cda.toFixed(2)} m2`;
    $("zone-reading").textContent = zone?.zone ?? "--";
    $("posture-reading").textContent = zone?.posture ?? "WAITING FOR DATA";
    $("screen-meter-button").style.backgroundColor = source() === 3 ? "#d97706" : "#2563eb";
    $("clock-reading").textContent = timeString(state.seconds);
    $("log-status").textContent = `LOG ${state.recording ? "OK" : "OFF"}`;
    $("log-status").style.color = state.recording ? "#16a34a" : "#64748b";
    $("toggle-run").textContent = state.running ? "Pause Auto Sampling" : "Start Auto Sampling";
    $("connect-ble").classList.toggle("selected", state.bleConnected);
    $("wifi-enabled").checked = state.wifiEnabled;
    $("wifi-status").textContent = {
      off: "Wi-Fi is off", provisioning: "Waiting for Wi-Fi setup",
      connecting: "Connecting to Wi-Fi...",
      connected: "Connected: Demo-WiFi\nhttp://aeroprobe.local/\nhttp://192.168.1.88/"
    }[state.wifi];
    $("sound-volume").textContent = `Volume: ${state.volume}%`;
    document.querySelectorAll("[data-volume]").forEach((button) =>
      button.classList.toggle("selected", Number(button.dataset.volume) === state.volume));
    const gpsActive = state.gps !== "absent";
    $("gps-indicator").textContent = gpsFixed() ? "GPS 12" : gpsActive ? "GPS" : "GPS --";
    $("gps-indicator").classList.toggle("searching", gpsActive && !gpsFixed());
    $("gps-indicator").style.color = gpsFixed() ? "#16a34a" : gpsActive ? "#d97706" : "#94a3b8";
    $("gps-fix").textContent = state.gps === "restarting" ? "Restarting GPS..." :
      gpsFixed() ? "FIX ACQUIRED" : gpsActive ? "Searching satellites..." : "GPS disabled";
    const hasTime = gpsFixed() || state.gps === "stale";
    $("gps-data").textContent = `UTC  ${hasTime ? formattedDate(utcDate()) : "--"}\n` +
      `LOCAL  ${hasTime ? formattedDate(localDate()) : "--"}\n` +
      `${gpsFixed() ? "31.23040, 121.47370" : "-- , --"}\n` +
      `SAT ${hasTime ? 12 : 0}    HDOP ${hasTime ? "0.9" : "0.0"}` +
      `\nALT  ${hasTime ? "12.0" : "0.0"} m\nSPD  ${hasTime ? (state.gpsSpeed * 3.6).toFixed(1) : "0.0"} km/h`;
    $("sd-playing").textContent = state.audio ? `Playing: ${state.audio}` : "Stopped";
    $("runtime-status").textContent = `Source: ${["NONE", "BLE", "", "VIRTUAL"][source()]} (mode ${state.mode === 3 ? "VIRTUAL" : state.mode === 1 ? "BLE" : "AUTO"})\n` +
      `BLE: ${state.audio ? "suspended for audio" : state.bleConnected ? state.bleName : "disconnected"}\n` +
      `Posture: ${state.postureConnected ? "-15.00 deg" : "not connected"}; BLE operation: ${state.operation?.kind ?? "idle"}\n` +
      `SD: ${state.sdReady ? state.sdBusy ? "busy" : "ready" : "not mounted"}; CSV submissions: ${state.audio ? "paused" : "enabled"}`;
    $("record-status").textContent = `${state.recording ? "Recording" : "Stopped"}: ${state.currentRecord?.path ?? "no recording yet"}\n` +
      `Buffered ${state.pending.length}; writer queued ${state.writer.length}; dropped ${state.dropped}; written ${state.currentRecord?.rows.length ?? 0}`;
    $("download-status").textContent = state.download
      ? `Downloading ${state.download.offset}/${state.download.text.length} bytes (simulated)`
      : state.downloadMessage;
  }

  function flushWriter() {
    if (!state.currentRecord) return;
    if (!state.audio && state.pending.length >= 60 && !state.writer.length)
      state.writer = state.pending.splice(0, 60);
    if (!state.sdReady || state.sdBusy) return;
    if (state.writer.length) {
      state.currentRecord.rows.push(...state.writer);
      state.writer = [];
    }
    if (!state.audio && state.pending.length >= 60) {
      state.writer = state.pending.splice(0, 60);
      state.currentRecord.rows.push(...state.writer);
      state.writer = [];
    }
  }
  function createRecord() {
    if (gpsFixed() || state.gps === "stale") state.logDate = localDate().toISOString().slice(0, 10);
    const date = state.logDate;
    const directory = date ? `/LOG/${date}` : "/LOG";
    files.set(directory, { directory: true });
    const path = `${directory}/fs_${String(++state.fileSequence).padStart(6, "0")}.csv`;
    const record = { path, date, rows: [] };
    files.set(path, record);
    state.currentRecord = record;
  }
  function recording(enabled) {
    if (state.wifi !== "connected") return notice("Web command unavailable: connect Wi-Fi first.");
    if (!state.sdReady || state.sdBusy) return notice("SD card unavailable or busy. Recording state unchanged.");
    if (enabled && !state.recording) {
      state.pending = []; state.writer = []; state.dropped = 0;
      createRecord();
      state.recording = true;
    } else if (!enabled && state.recording) {
      state.currentRecord.rows.push(...state.writer, ...state.pending);
      state.pending = []; state.writer = [];
      state.recording = false;
    }
    render();
  }
  function captureSample() {
    state.seconds++;
    state.samples.push(power());
    if (state.samples.length > 60) state.samples.shift();
    flushWriter();
    if (!state.recording) return;
    if (gpsFixed() || state.gps === "stale") state.logDate = localDate().toISOString().slice(0, 10);
    const date = state.logDate;
    if (date !== state.currentRecord.date && !state.sdBusy && state.sdReady) {
      state.currentRecord.rows.push(...state.writer, ...state.pending);
      state.writer = []; state.pending = [];
      createRecord();
    }
    if (state.pending.length >= 60) {
      if (!state.audio && !state.writer.length) state.writer = state.pending.splice(0, 60);
      else { state.dropped++; return; }
    }
    const speed = gpsFixed() ? state.gpsSpeed : state.bleConnected ? state.bleSpeed : 0;
    const row = [state.seconds * 1000, power(), cadence(), speed.toFixed(3),
      state.wind.toFixed(3), "0.000", state.postureConnected ? "-15.000" : "0.000", "1.225", source()].join(",") + "\n";
    state.pending.push(row);
    flushWriter();
  }
  function step(seconds = 1) {
    for (let i = 0; i < seconds; i++) {
      state.extraMs += 1000;
      processEvents();
      captureSample();
    }
    render();
  }
  function setWifi(enabled) {
    state.wifiEnabled = enabled;
    if (!enabled) {
      state.wifi = "off";
      if (state.download) endDownload("Download interrupted: Wi-Fi disabled.");
    } else {
      state.wifi = state.wifiEnvironment === "unconfigured" ? "provisioning" : "connecting";
      state.wifiDeadline = now() + 1000;
    }
    render();
  }
  function setGps(environment) {
    state.gpsEnvironment = environment;
    state.gps = environment === "absent" ? "detecting" : environment;
    state.gpsDeadline = now() + 3000;
    render();
  }
  function beginBle(kind, device = null) {
    if (state.audio) return notice("BLE is suspended for audio playback. Stop playback first.");
    if (state.operation) return notice("BLE is busy. Wait for the current scan/connection, then retry.");
    state.operation = { kind, device, deadline: now() + (kind === "scan" ? 6000 : kind === "posture" ? 1500 : 900) };
    if (kind !== "posture") {
      state.manualPairing = true;
      state.mode = 1;
      $("loading-title").textContent = "LOADING";
      $("loading-back").textContent = "‹ BACK";
      $("loading-back").disabled = false;
      $("loading-message").textContent = kind === "scan" ? "Scanning for nearby BLE devices (6 seconds)..." : `Connecting to ${device.name}...`;
      $("loading-spinner").style.display = "block";
      showPage("ble-loading");
    }
  }
  function renderDevices() {
    $("ble-device-list").replaceChildren();
    const found = state.discoveredDevices;
    $("ble-device-status").textContent = `${found.length} device(s) found`;
    for (const device of found) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = `${device.name}\n${device.profile}  ${device.rssi} dBm`;
      button.addEventListener("click", () => {
        $("confirm-device-name").textContent = device.name;
        $("confirm-device-details").textContent = `${device.address}\n${device.profile}`;
        $("dialog-list").hidden = true;
        $("dialog-confirm").classList.add("open");
        selectedDevice = device;
        showPage("touch-dialog");
      });
      $("ble-device-list").append(button);
    }
    showPage("ble-devices-page");
  }
  let selectedDevice = null;
  function openMeterDialog() {
    $("dialog-list").hidden = false;
    $("dialog-list").style.display = "flex";
    $("dialog-confirm").classList.remove("open");
    selectedDevice = null;
    showPage("touch-dialog");
  }
  function connect() {
    if (state.audio || state.operation) return notice("BLE is busy or suspended; stop playback/wait and retry.");
    state.bleConnected = true; state.bleName = "Assioma Duo"; state.mode = 1;
    state.manualPairing = false;
    showPage();
  }
  function disconnect() {
    state.bleConnected = false; state.bleName = "";
    render();
  }
  function play(path) {
    if (!state.sdReady || state.sdBusy) return notice("Cannot play: SD card unavailable or busy.");
    if (state.operation) return notice("Wait for the BLE operation to finish before playback.");
    disconnect();
    state.postureConnected = false;
    state.audio = path; state.audioDeadline = now() + 90000;
    render();
  }
  function stopAudio() {
    state.audio = null;
    state.nextPostureMs = now() + 30000;
    flushWriter();
    render();
  }
  function browse(directory = "/") {
    state.directory = directory;
    $("sd-path").textContent = directory;
    $("sd-up").hidden = directory === "/";
    $("sd-files").replaceChildren();
    const prefix = directory === "/" ? "/" : `${directory}/`;
    const entries = state.sdReady && !state.sdBusy ? [...files.entries()]
      .filter(([path]) => path.startsWith(prefix) && !path.slice(prefix.length).includes("/") && path !== directory)
      .sort(([a, x], [b, y]) => Number(Boolean(y.directory)) - Number(Boolean(x.directory)) ||
        Number(Boolean(y.audio)) - Number(Boolean(x.audio)) || a.localeCompare(b)).slice(0, 12) : [];
    $("sd-status").textContent = !state.sdReady ? "No files / SD not ready" :
      state.sdBusy ? "SD card busy; reopen to retry" : `${entries.length} file(s)`;
    for (const [path, entry] of entries) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = `${entry.directory ? "▣ " : entry.audio ? "▶ " : ""}${path.slice(prefix.length)}`;
      button.addEventListener("click", () => {
        if (entry.directory) browse(path);
        else if (entry.audio) play(path);
        else notice("Not an audio file");
      });
      $("sd-files").append(button);
    }
    showPage("sd-page");
  }
  function endDownload(message) {
    state.download = null; state.downloadMessage = message;
  }
  function download() {
    if (state.wifi !== "connected") return notice("Download unavailable: connect Wi-Fi first.");
    if (state.download) return notice("Another download is active (503).");
    if (!state.sdReady || !state.currentRecord || state.sdBusy)
      return notice("No record available or SD busy (503).");
    const record = state.currentRecord;
    state.download = { text: CSV_HEADER + record.rows.join(""), path: record.path, offset: 0, nextMs: now(), waitStarted: null };
    state.downloadMessage = "";
    render();
  }
  function processEvents() {
    const time = now();
    if (state.wifiEnabled && state.wifiEnvironment === "connected" && time >= state.wifiDeadline) state.wifi = "connected";
    if (["detecting", "restarting"].includes(state.gps) && time >= state.gpsDeadline)
      state.gps = state.gpsEnvironment === "absent" ? "absent" : "searching";
    if (state.audio && time >= state.audioDeadline) stopAudio();
    if (state.operation && time >= state.operation.deadline) {
      const operation = state.operation;
      state.operation = null;
      if (operation.cancelled) {
        state.manualPairing = false;
        $("loading-back").disabled = false;
        $("loading-back").textContent = "‹ BACK";
        renderDevices();
      } else if (operation.kind === "scan") {
        state.discoveredDevices = state.bleResult === "empty" ? [] : devices;
        renderDevices();
      }
      else if (operation.kind === "connect") {
        if (state.bleResult === "failure" || operation.device.profile.startsWith("CSC")) {
          $("loading-title").textContent = "CONNECTION FAILED";
          $("loading-spinner").style.display = "none";
          $("loading-message").textContent = operation.device.profile.startsWith("CSC")
            ? "CSC provides cadence/speed, not power. Choose a CPS or FTMS device."
            : "BLE connection/GATT discovery failed. Return to the list and retry.";
        } else {
          state.bleConnected = true; state.bleName = operation.device.name;
          state.manualPairing = false;
          showPage();
        }
      } else state.postureConnected = state.postureEnvironment === "connected";
    }
    if (!state.audio && !state.operation && !state.manualPairing && !state.postureConnected && time >= state.nextPostureMs) {
      state.nextPostureMs = time + 30000;
      beginBle("posture");
    }
    flushWriter();
    if (state.download && time >= state.download.nextMs) {
      const transfer = state.download;
      if (state.wifi !== "connected" || !state.sdReady) endDownload("Download interrupted: Wi-Fi or SD unavailable.");
      else if (state.sdBusy) {
        transfer.waitStarted ??= time;
        if (time - transfer.waitStarted >= 5000) endDownload("Download failed: SD lock timeout (5 seconds).");
      } else {
        transfer.waitStarted = null;
        transfer.offset = Math.min(transfer.text.length, transfer.offset + 1024);
        transfer.nextMs = time + 20;
        if (transfer.offset === transfer.text.length) {
          const url = URL.createObjectURL(new Blob([transfer.text], { type: "text/csv" }));
          const link = document.createElement("a");
          link.href = url; link.download = transfer.path.split("/").pop();
          link.click();
          setTimeout(() => URL.revokeObjectURL(url), 1000);
          endDownload(`Downloaded ${transfer.text.length} bytes. Only rows already written to SD are included.`);
        }
      }
    }
  }

  $("screen-meter-button").onclick = $("boot-key").onclick = () => showPage("settings-dialog");
  $("settings-back").onclick = () => showPage();
  $("settings-meter").onclick = $("choose-meter").onclick = openMeterDialog;
  for (const [button, page] of [["language", "language-page"], ["wifi", "wifi-page"], ["sound", "sound-page"]])
    $(`settings-${button}`).onclick = () => showPage(page);
  $("settings-sd").onclick = () => browse("/");
  $("settings-gps").onclick = () => state.gps === "absent"
    ? notice("GPS module not found.\nCheck the UART wiring and power.") : showPage("gps-page");
  document.querySelectorAll("[data-back]").forEach((button) => {
    button.onclick = () => button.dataset.back === "touch-dialog" ? (state.manualPairing = false, openMeterDialog()) : showPage(button.dataset.back);
  });
  $("language-en").onclick = () => { localStorage.setItem("aero-language", "en"); };
  $("language-zh").onclick = () => notice("Chinese language temporarily disabled; staying English.");
  $("notice-dismiss").onclick = () => { $("notice-dialog").hidden = true; state.error = ""; };
  $("wifi-enabled").onchange = (event) => setWifi(event.target.checked);
  $("wifi-input").onchange = (event) => { state.wifiEnvironment = event.target.value; if (state.wifiEnabled) setWifi(true); };
  document.querySelectorAll("[data-volume]").forEach((button) => {
    button.onclick = () => { state.volume = Number(button.dataset.volume); localStorage.setItem("aero-volume", String(state.volume)); render(); };
  });
  $("gps-input").onchange = (event) => setGps(event.target.value);
  $("gps-restart").onclick = () => { state.gps = "restarting"; state.gpsDeadline = now() + 3000; render(); };
  $("gps-speed-input").oninput = (event) => { state.gpsSpeed = Number(event.target.value) / 3.6; render(); };
  $("ble-speed-input").oninput = (event) => { state.bleSpeed = Number(event.target.value) / 3.6; render(); };
  $("wind-valid").onchange = (event) => { state.windValid = event.target.checked; render(); };
  $("posture-input").onchange = (event) => { state.postureEnvironment = event.target.value; if (event.target.value === "absent") state.postureConnected = false; render(); };
  $("sd-ready").onchange = (event) => {
    state.sdReady = event.target.checked;
    if (!state.sdReady && state.audio) stopAudio();
    if (state.page === "sd-page") browse(state.directory);
    render();
  };
  $("sd-busy").onchange = (event) => { state.sdBusy = event.target.checked; processEvents(); render(); };
  $("sd-up").onclick = () => browse(state.directory.slice(0, state.directory.lastIndexOf("/")) || "/");
  $("sd-stop").onclick = () => state.sdBusy ? notice("SD card busy; playback could not stop.") : stopAudio();
  $("connect-ble").onclick = connect;
  $("disconnect").onclick = disconnect;
  $("scan-again").onclick = $("ble-scan-again").onclick = () => beginBle("scan");
  $("ble-result").onchange = (event) => { state.bleResult = event.target.value; };
  $("confirm-connect").onclick = () => { if (selectedDevice) beginBle("connect", selectedDevice); };
  $("dialog-close").onclick = () => {
    if ($("dialog-confirm").classList.contains("open")) renderDevices();
    else { state.manualPairing = false; showPage("settings-dialog"); }
  };
  $("loading-back").onclick = () => {
    if (state.operation) {
      state.operation.cancelled = true;
      state.operation.deadline = now() + 200;
      $("loading-back").disabled = true;
      $("loading-back").textContent = "WAIT...";
      $("loading-message").textContent = "Cancelling operation... Please wait for BLE to release.";
    } else { state.manualPairing = false; renderDevices(); }
  };
  $("virtual-source").onclick = () => { state.mode = 3; state.manualPairing = false; showPage("virtual-dialog"); };
  $("auto-source").onclick = () => { state.mode = 0; state.manualPairing = false; showPage(); };
  $("virtual-back").onclick = openMeterDialog;
  for (const [id, field, divisor] of [["wind", "wind", 3.6], ["power", "power", 1], ["cadence", "cadence", 1],
    ["virtual-power", "virtualPower", 1], ["virtual-cadence", "virtualCadence", 1]]) {
    $(id + "-input").oninput = (event) => { state[field] = Number(event.target.value) / divisor; syncInputs(); render(); };
  }
  $("step-one").onclick = () => step(1);
  $("step-sixty").onclick = () => step(60);
  $("reset").onclick = () => { state.samples = []; render(); };
  $("toggle-run").onclick = () => { state.running = !state.running; render(); };
  $("record-start").onclick = () => recording(true);
  $("record-stop").onclick = () => recording(false);
  $("download-record").onclick = download;
  let lastSampleMs = performance.now();
  setInterval(() => {
    processEvents();
    const time = performance.now();
    if (state.running && time - lastSampleMs >= 1000) { captureSample(); lastSampleMs = time; }
    if (!state.running) lastSampleMs = time;
    render();
  }, 20);

  window.aeroProbeSim = {
    connect, disconnect, openMeterDialog, step, showPage, browse, recording, download,
    reset() { state.samples = []; render(); },
    setInput(values) {
      for (const field of ["power", "cadence", "virtualPower", "virtualCadence"])
        if (field in values) state[field] = Number(values[field]);
      if ("wind" in values) state.wind = Number(values.wind) / 3.6;
      syncInputs(); render();
    },
    snapshot() {
      return {
        source: source() === 3 ? "VIRTUAL" : source() === 1 ? "BLE" : null, powerSrc: source(), mode: state.mode,
        power: power(), cadence: cadence(), wind: state.wind * 3.6, windMps: state.wind, windValid: state.windValid,
        cda: estimateCda(), zone: zoneFor(estimateCda())?.zone ?? null,
        average3: average(state.samples.slice(-3)), average60: average(state.samples), sampleCount: state.samples.length,
        seconds: state.seconds, page: state.page, language: "en", volume: state.volume,
        wifi: state.wifi, gps: state.gps, gpsSpeedMps: state.gpsSpeed, audio: state.audio,
        bleConnected: state.bleConnected, postureConnected: state.postureConnected, operation: state.operation?.kind ?? null,
        recording: state.recording, buffered: state.pending.length, queued: state.writer.length, dropped: state.dropped,
        written: state.currentRecord?.rows.length ?? 0, csv: CSV_HEADER + (state.currentRecord?.rows.join("") ?? ""),
        recordPath: state.currentRecord?.path ?? null, downloading: Boolean(state.download), downloadMessage: state.downloadMessage,
        error: state.error
      };
    }
  };
  document.documentElement.lang = "en";
  syncInputs();
  render();
})();
