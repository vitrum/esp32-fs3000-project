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
  function favorites() {
    try {
      const values = JSON.parse(localStorage.getItem('aero-ble-favorites') || '[]');
      return Array.isArray(values) ? values.slice(0, 4) : [];
    } catch { return []; }
  }
  const isFavorite = (device) => Boolean(device && favorites().some((entry) =>
    entry.address.toLowerCase() === device.address.toLowerCase() && (entry.addressType || 0) === (device.addressType || 0)));
  const state = {
    language: localStorage.getItem('aero-language') === 'zh' ? 'zh' : 'en',
    mode: 0, bleConnected: false, bleName: "", connectedDevice: null, autoFavorites: [], power: 230, cadence: 88, bleSpeed: 0,
    virtualPower: 0, virtualCadence: 0, wind: 9.5, windValid: true,
    samples: [], seconds: 0, running: false, page: "dashboard",
    wifiEnabled: false, wifi: "off", wifiEnvironment: "unconfigured", wifiDeadline: 0,
    networkTime: false, ntpDeadline: 0, ntpAvailable: true,
    volume: Number.isFinite(savedVolume) ? Math.max(0, Math.min(100, savedVolume)) : 55,
    gpsEnvironment: "searching", gps: "searching", gpsDeadline: 0, gpsSpeed: 30 / 3.6,
    sdReady: true, sdBusy: false, directory: "/", audio: null, audioDeadline: 0,
    operation: null, bleResult: "success", discoveredDevices: [], manualPairing: false,
    postureEnvironment: "absent", postureConnected: false, nextPostureMs: 30000,
    recording: false, currentRecord: null, logDate: "", pending: [], writer: [], dropped: 0,
    armed: false, recordStarted: 0, recordedDuration: 0, skipped: 0, powerValid: true,
    download: null, downloadMessage: "", error: "", extraMs: 0, fileSequence: 0,
    sessionRecords: [], fitSaving: null, fitPath: "", fitBytes: null, trustedTime: false
  };
  const now = () => performance.now() + state.extraMs;
  let backBlockedUntil = 0;
  document.querySelector(".device").addEventListener("click", (event) => {
    if (!event.target.closest("[data-back], #settings-back, #dialog-close, #virtual-back, #loading-back")) return;
    if (now() < backBlockedUntil) {
      event.stopImmediatePropagation();
      event.preventDefault();
      return;
    }
    backBlockedUntil = now() + 250;
  }, true);
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
    if (!state.windValid || !source() || watts <= 0 || state.wind < 3) return null;
    const aeroPower = watts * 0.975 - 85 * 9.80665 * 0.004 * state.wind;
    return aeroPower > 0 ? aeroPower / (0.5 * 1.225 * state.wind ** 3) : null;
  }
  function zoneFor(cda) {
    if (cda === null) return null;
    const bounds = [0.23, 0.27, 0.31, 0.36, 0.42, 0.50, Infinity];
    const index = bounds.findIndex((bound) => cda < bound);
    return { zone: 7 - index, posture: (state.language === 'zh'
      ? ["精英姿态", "竞赛姿态", "极致气动", "激进", "气动", "低位下把", "直立"]
      : ["ELITE TT", "RACE TT", "VERY AERO", "AGGRESSIVE", "AERO", "DROPS", "UPRIGHT"])[index] };
  }
  function applyLanguage(){
    const zh=state.language==='zh';
    document.documentElement.lang=zh?'zh-CN':'en';
    for(const [selector,en,cn] of [
      ['.cda-title','AERO ZONE','气动区间'],['#wind-title','WIND','风速'],['.avg3','POWER','功率'],
      ['#language-page p','Choose interface language','选择界面语言'],
      ['#settings-log','☷ RIDE LOG','☷ 记录日志'],['#settings-meter','⏻ POWER','⏻ 功率'],
      ['#settings-language','☷ LANG','☷ 语言'],['#settings-sound','♫ SOUND','♫ 声音'],
      ['#settings-sd','▣ SD/MUSIC','▣ SD/音乐'],['#wifi-setup','SET UP WI-FI','配置网络']
    ]){const element=document.querySelector(selector);if(element)element.textContent=zh?cn:en;}
    for(const [id,en,cn] of [['settings-dialog','SETTINGS','设置'],['language-page','LANGUAGE','语言'],
      ['sound-page','SOUND','声音'],['ride-log-page','LOG','日志'],['sd-page','SD CARD','SD 卡']]){
      const header=document.querySelector('#'+id+' .page-header');
      if(header?.lastChild?.nodeType===Node.TEXT_NODE)header.lastChild.textContent=zh?cn:en;
    }
    for(const button of document.querySelectorAll('[data-back],#settings-back'))button.textContent=zh?'‹ 返回':'‹ BACK';
    $('screen-meter-button').title=zh?'设置':'Settings';
    $('screen-meter-button').setAttribute('aria-label',zh?'设置':'Settings');
    $('language-en').classList.toggle('selected',!zh);$('language-zh').classList.toggle('selected',zh);
  }
  function setLanguage(language){
    if(!['en','zh'].includes(language))return;
    state.language=language;localStorage.setItem('aero-language',language);applyLanguage();render();
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
    const favorite = isFavorite(state.connectedDevice);
    $('ble-favorite').disabled = !state.bleConnected || !state.connectedDevice || state.connectedDevice.profile.startsWith('CSC');
    $('ble-favorite').setAttribute('aria-pressed', String(favorite));
    $('ble-favorite-star').textContent = favorite ? '★' : '☆';
    $('ble-favorite-name').textContent = state.connectedDevice?.name || '--';
    const cda = estimateCda();
    const zone = zoneFor(cda);
    $("power-reading").textContent = String(Math.round(power()));
    $("power-reading").style.fontSize = Math.abs(power()) >= 10000 ? "28px" : "40px";
    $("cadence-label").textContent = state.language==='zh'?'踏频 / rpm':'CADENCE / rpm';
    $("cadence-reading").textContent = String(Math.round(cadence()));
    $("wind-reading").textContent = state.windValid ? (state.wind * 3.6).toFixed(1) : "--";
    $("cda-reading").textContent = cda === null ? "--" : cda.toFixed(2);
    $("zone-reading").textContent = `Z${zone?.zone ?? "--"}`;
    $("posture-reading").textContent = zone?.posture ?? (state.language==='zh'?'等待数据':'NO DATA');
    $("clock-reading").textContent = gpsFixed() || state.networkTime
      ? formattedDate(localDate()).slice(11) : "--:--:--";
    $("log-status").title = `${state.recording ? "Recording" : "Recording stopped"}; open ride log`;
    $("log-status").setAttribute("aria-label", $("log-status").title);
    $("log-status").style.color = state.recording ? "#16a34a" : "#64748b";
    $("toggle-run").textContent = state.running ? "Pause Auto Sampling" : "Start Auto Sampling";
    $("connect-ble").classList.toggle("selected", state.bleConnected);
    for (const [id, enabled] of [["wifi-off", false], ["wifi-on", true]]) {
      $(id).classList.toggle("selected", state.wifiEnabled === enabled);
      $(id).setAttribute("aria-pressed", String(state.wifiEnabled === enabled));
    }
    $("wifi-status").textContent = {
      off: "Wi-Fi is off", provisioning: "Waiting for Wi-Fi setup",
      connecting: "Connecting to Wi-Fi...",
      waiting: "Saved networks unavailable. Use ON to retry or SET UP WI-FI.",
      connected: "Connected: Demo-WiFi\nhttp://aeroprobe.local/\nhttp://192.168.1.88/"
    }[state.wifi];
    $("sound-volume").textContent = `Volume: ${state.volume}%`;
    if ($("sound-volume-input").value !== String(state.volume))
      $("sound-volume-input").value = String(state.volume);
    const gpsActive = state.gps !== "absent";
    $("gps-satellites").textContent = gpsFixed() ? "12" : "--";
    $("gps-satellites").hidden = gpsActive && !gpsFixed();
    $("gps-spinner").hidden = !gpsActive || gpsFixed();
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
    $("fit-save").disabled = Boolean(state.fitSaving);
    $("fit-save").textContent = state.fitSaving ? (state.language==='zh'?'保存中':'SAVING') : (state.language==='zh'?'保存 FIT':'SAVE FIT');
    $("ride-start").disabled=state.recording||state.armed||Boolean(state.fitSaving);
    $("ride-start").textContent=state.armed ? (state.language==='zh'?'等待数据':'WAITING') : (state.language==='zh'?'▶ 开始':'▶ START');
    $('ride-stop').textContent=state.language==='zh'?'■ 停止':'■ STOP';
    $("ride-stop").disabled=(!state.recording&&!state.armed)||Boolean(state.fitSaving);
    const duration=state.recording ? state.seconds-state.recordStarted : state.recordedDuration;
    $("ride-duration").textContent=`${state.armed ? "WAITING" : "TIME"} ${timeString(duration)}`;
    const recent = state.currentRecord ? [...state.currentRecord.rows, ...state.writer, ...state.pending] : [];
    $("ride-log-data").textContent = `${state.armed ? "WAITING FOR POWER + WIND" : state.fitSaving ? "SAVING" : state.recording ? "RECORDING" : "STOPPED"}\n${state.currentRecord?.path ?? "No recording yet"}\n\n` +
      `Samples: ${state.sessionRecords.reduce((sum, record) => sum + record.rows.length, 0) + state.writer.length + state.pending.length}\nDropped: ${state.dropped}\n` +
      `\nPower: ${Math.round(power())} W\nCadence: ${Math.round(cadence())} rpm\n` +
      `Speed: ${((gpsFixed() ? state.gpsSpeed : state.bleConnected ? state.bleSpeed : 0) * 3.6).toFixed(1)} km/h\n\nRecent samples (ms,W,rpm,m/s):\n` +
      recent.slice(-6).map((row) => row.trim().split(",").slice(0,4).join(",")).join("\n") +
      "\n\nSAVE FIT ends recording.\nOriginal CSV is retained." + (state.fitPath ? `\nSaved: ${state.fitPath}` : "");
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
    if (gpsFixed() || state.gps === "stale" || state.networkTime) state.logDate = localDate().toISOString().slice(0, 10);
    const date = state.logDate;
    const directory = date ? `/LOG/${date}` : "/LOG";
    files.set(directory, { directory: true });
    const path = `${directory}/fs_${String(++state.fileSequence).padStart(6, "0")}.csv`;
    const record = { path, date, rows: [] };
    files.set(path, record);
    state.currentRecord = record;
    state.sessionRecords.push(record);
  }
  function recording(enabled, local = false) {
    if (state.fitSaving) return notice("Wait for FIT export before recording control.");
    if (!local && state.wifi !== "connected") return notice("Web command unavailable: connect Wi-Fi first.");
    if (!state.sdReady || state.sdBusy) return notice("SD card unavailable or busy. Recording state unchanged.");
    if (enabled && !state.recording && !state.armed) {
      state.pending = []; state.writer = []; state.dropped = 0;
      state.sessionRecords = []; state.fitPath = ""; state.fitBytes = null;
      state.currentRecord=null;state.recordedDuration=0;state.skipped=0;
      state.trustedTime = gpsFixed() || state.gps === "stale" || state.networkTime;
      state.armed = true;
    } else if (!enabled && state.armed) {
      state.armed=false;
    } else if (!enabled && state.recording) {
      saveFit(true);
    }
    render();
  }
  function captureSample() {
    state.seconds++;
    state.samples.push(power());
    if (state.samples.length > 60) state.samples.shift();
    flushWriter();
    const inputsValid=Boolean(source())&&(state.mode===3||state.powerValid)&&state.windValid&&Number.isFinite(power())&&Number.isFinite(state.wind);
    if(state.armed&&inputsValid){createRecord();state.recording=true;state.armed=false;state.recordStarted=state.seconds;}
    if (!state.recording) return;
    if(!inputsValid){state.skipped++;return;}
    if (gpsFixed() || state.gps === "stale" || state.networkTime) state.trustedTime = true;
    if (gpsFixed() || state.gps === "stale" || state.networkTime) state.logDate = localDate().toISOString().slice(0, 10);
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
      state.wifiAttemptEnd = now() + 15000;
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
    if (kind === 'scan') state.autoFavorites = [];
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
      button.textContent = `${isFavorite(device) ? '★ ' : ''}${device.name}\n${device.profile}  ${device.rssi} dBm`;
      button.addEventListener("click", () => {
        state.autoFavorites = [];
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
    state.connectedDevice = devices[0];
    state.manualPairing = false;
    showPage();
  }
  function disconnect() {
    state.bleConnected = false; state.bleName = "";
    render();
  }
  function play(path) {
    if (state.fitSaving) return notice("Wait for FIT export before playing audio.");
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
        else if (entry.binary) {
          const url = URL.createObjectURL(new Blob([entry.binary], { type: "application/octet-stream" }));
          const link = document.createElement("a");
          link.href = url; link.download = path.split("/").pop(); link.click();
          setTimeout(() => URL.revokeObjectURL(url),1000);
        }
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
    if (state.fitSaving && time >= state.fitSaving.deadline) {
      const pending = state.fitSaving;
      state.fitSaving = null;
      if (!state.sdReady || state.sdBusy) notice("FIT export failed. CSV retained; retry saving.");
      else {
        state.fitPath = pending.path; state.fitBytes = pending.bytes;
        files.set(pending.path, { binary: pending.bytes });
        if(!pending.auto)notice(`Saved ${pending.path}`);
      }
    }
    if (state.wifiEnabled && state.wifiEnvironment === "connected" && time >= state.wifiDeadline) {
      if (state.wifi !== "connected") state.ntpDeadline = time + 1000;
      state.wifi = "connected";
    }
    if(state.wifiEnabled&&state.wifi==='connecting'&&time>=state.wifiAttemptEnd){state.wifi='waiting';state.wifiEnabled=false;}
    if (state.wifi === "connected" && state.ntpDeadline && state.ntpAvailable && time >= state.ntpDeadline) {
      state.networkTime = true;
      state.ntpDeadline = 0;
    }
    if (["detecting", "restarting"].includes(state.gps) && time >= state.gpsDeadline)
      state.gps = state.gpsEnvironment === "absent" ? "absent" : "searching";
    if (state.audio && time >= state.audioDeadline) stopAudio();
    if (state.operation && time >= state.operation.deadline) {
      const operation = state.operation;
      state.operation = null;
      if (operation.cancelled) {
        state.autoFavorites = [];
        state.manualPairing = false;
        $("loading-back").disabled = false;
        $("loading-back").textContent = "‹ BACK";
        renderDevices();
      } else if (operation.kind === "scan") {
        const nearby = state.bleResult === 'crowded' ? [...devices, ...Array.from({length: 20}, (_, index) => ({
          name: index % 2 ? 'Shared bIkE ' + index : 'Power Sensor ' + index,
          profile: 'CPS', address: 'D0:00:00:00:00:' + index.toString(16).padStart(2, '0'), rssi: -65-index
        }))] : devices;
        state.discoveredDevices = state.bleResult === "empty" ? [] : nearby
          .filter((device) => isFavorite(device) || !/bike/i.test(device.name))
          .sort((left, right) => Number(isFavorite(right))-Number(isFavorite(left)) || right.rssi-left.rssi).slice(0, 16);
        renderDevices();
        state.autoFavorites = state.bleConnected ? [] : state.discoveredDevices.filter(isFavorite);
        if (state.autoFavorites.length) beginBle('connect', state.autoFavorites.shift());
      }
      else if (operation.kind === "connect") {
        if (state.bleResult === "failure" || operation.device.profile.startsWith("CSC")) {
          $("loading-title").textContent = "CONNECTION FAILED";
          $("loading-spinner").style.display = "none";
          $("loading-message").textContent = operation.device.profile.startsWith("CSC")
            ? "CSC provides cadence/speed, not power. Choose a CPS or FTMS device."
            : "BLE connection/GATT discovery failed. Return to the list and retry.";
          if (state.autoFavorites.length) beginBle('connect', state.autoFavorites.shift());
          else state.manualPairing = false;
        } else {
          state.bleConnected = true; state.bleName = operation.device.name;
          state.connectedDevice = operation.device;
          state.autoFavorites = [];
          state.manualPairing = false;
          showPage();
        }
      } else state.postureConnected = state.postureEnvironment === "connected";
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
  $("settings-log").onclick = () => showPage("ride-log-page");
  $("log-status").onclick = () => showPage("ride-log-page");
  function saveFit(auto = false) {
    if (state.fitSaving) return;
    if (!state.sdReady || state.sdBusy) return notice("SD card unavailable or busy. Recording state unchanged.");
    if (state.audio) return notice("Stop audio before saving FIT.");
    const rows = [...state.sessionRecords.flatMap((record) => record.rows), ...state.writer, ...state.pending]
      .map((row) => row.trim().split(",").map(Number));
    if (!rows.length) return notice("No recorded samples. Start recording first.");
    const path = state.sessionRecords[0].path.replace(/\.csv$/, ".fit");
    if (files.has(path)) return notice("FIT already exists. CSV retained.");
    if (state.sessionRecords.length > 8) return notice("Too many daily CSV segments for FIT export.");
    const epoch=state.trustedTime ? Date.UTC(2026,9,19,4)/1000 : 1704067200;
    const bytes = window.encodeRideFit(rows, epoch + Math.floor(rows[0][0] / 1000));
    // A local screen command does not require Wi-Fi.
    state.currentRecord.rows.push(...state.writer, ...state.pending);
    if(state.recording)state.recordedDuration=state.seconds-state.recordStarted;
    state.writer = []; state.pending = []; state.recording = false;
    state.fitSaving = { path, bytes, auto: auto===true, deadline: now() + 1000 };
    render();
  }
  $("fit-save").onclick = () => saveFit();
  $("ride-start").onclick = () => recording(true,true);
  $("ride-stop").onclick = () => recording(false,true);
  $("settings-gps").onclick = () => state.gps === "absent"
    ? notice("GPS module not found.\nCheck the UART wiring and power.") : showPage("gps-page");
  document.querySelectorAll("[data-back]").forEach((button) => {
    button.onclick = () => button.dataset.back === "touch-dialog" ? (state.manualPairing = false, openMeterDialog()) : showPage(button.dataset.back);
  });
  $("language-en").onclick = () => setLanguage('en');
  $("language-zh").onclick = () => setLanguage('zh');
  $("notice-dismiss").onclick = () => { $("notice-dialog").hidden = true; state.error = ""; };
  $("wifi-off").onclick = () => setWifi(false);
  $("wifi-on").onclick = () => { if (!state.wifiEnabled) setWifi(true); };
  $("wifi-setup").onclick = () => {state.wifiEnvironment="unconfigured";setWifi(true);};
  $("wifi-input").onchange = (event) => { state.wifiEnvironment = event.target.value; if (state.wifiEnabled) setWifi(true); };
  $("sound-volume-input").oninput = (event) => { state.volume = Number(event.target.value); render(); };
  $("sound-volume-input").onchange = () => localStorage.setItem("aero-volume", String(state.volume));
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
  $('ble-favorite').onclick = () => {
    if ($('ble-favorite').disabled) return;
    const entries = favorites();
    const favorite = isFavorite(state.connectedDevice);
    if (!favorite && entries.length === 4) return notice('Favorite limit is 4.');
    try {
      localStorage.setItem('aero-ble-favorites', JSON.stringify(favorite
        ? entries.filter((entry) => entry.address !== state.connectedDevice.address || (entry.addressType || 0) !== (state.connectedDevice.addressType || 0))
        : [...entries, state.connectedDevice]));
    } catch { return notice('Could not save favorite.'); }
    render();
  };
  $("dialog-close").onclick = () => {
    if ($("dialog-confirm").classList.contains("open")) renderDevices();
    else { state.manualPairing = false; showPage("settings-dialog"); }
  };
  $("loading-back").onclick = () => {
    state.autoFavorites = [];
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
    connect, disconnect, openMeterDialog, step, showPage, browse, recording, download, saveFit, setLanguage,
    reset() { state.samples = []; render(); },
    setInput(values) {
      for (const field of ["power", "cadence", "virtualPower", "virtualCadence"])
        if (field in values) state[field] = Number(values[field]);
      if ("wind" in values) state.wind = Number(values.wind) / 3.6;
      if ("ntpAvailable" in values) state.ntpAvailable = Boolean(values.ntpAvailable);
      if ("powerValid" in values) state.powerValid = Boolean(values.powerValid);
      syncInputs(); render();
    },
    snapshot() {
      return {
        source: source() === 3 ? "VIRTUAL" : source() === 1 ? "BLE" : null, powerSrc: source(), mode: state.mode,
        power: power(), cadence: cadence(), wind: state.wind * 3.6, windMps: state.wind, windValid: state.windValid,
        cda: estimateCda(), zone: zoneFor(estimateCda())?.zone ?? null,
        average3: average(state.samples.slice(-3)), average60: average(state.samples), sampleCount: state.samples.length,
        seconds: state.seconds, page: state.page, language: state.language, volume: state.volume,
        wifi: state.wifi, gps: state.gps, gpsSpeedMps: state.gpsSpeed, audio: state.audio,
        networkTime: state.networkTime, trustedTime: state.trustedTime,
        bleConnected: state.bleConnected, postureConnected: state.postureConnected, operation: state.operation?.kind ?? null,
        recording: state.recording, buffered: state.pending.length, queued: state.writer.length, dropped: state.dropped,
        armed:state.armed,duration:state.recording ? state.seconds-state.recordStarted : state.recordedDuration,skipped:state.skipped,
        written: state.currentRecord?.rows.length ?? 0, csv: CSV_HEADER + (state.currentRecord?.rows.join("") ?? ""),
        recordPath: state.currentRecord?.path ?? null, downloading: Boolean(state.download), downloadMessage: state.downloadMessage,
        error: state.error, fitSaving: Boolean(state.fitSaving), fitPath: state.fitPath,
        fitBytes: state.fitBytes ? Array.from(state.fitBytes) : null
      };
    }
  };
  applyLanguage();
  syncInputs();
  render();
})();
