#pragma once

// ============================================================================
// Wi-Fi 网页前端资源（PROGMEM 内嵌）
// 实时监控主页：数值卡片 + WebSocket 实时刷新 + 历史记录下载
// 轻量：原生 HTML/JS，无外部构建，适合 ESP32 内存
// ============================================================================

static const char WIFI_WEB_INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>AeroProbe 实时监控</title>
<style>
  :root{--bg:#0f172a;--card:#1e293b;--fg:#e2e8f0;--accent:#38bdf8;--ok:#22c55e;--warn:#f59e0b;--err:#ef4444;}
  *{box-sizing:border-box;}
  body{margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;}
  header{display:flex;justify-content:space-between;align-items:center;padding:16px 24px;background:var(--card);}
  header h1{font-size:20px;margin:0;}
  #conn{font-size:14px;padding:4px 12px;border-radius:12px;background:var(--err);}
  #conn.ok{background:var(--ok);}
  main{padding:24px;display:grid;gap:24px;}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:16px;}
  .card{background:var(--card);border-radius:12px;padding:18px;text-align:center;}
  .card .label{font-size:13px;opacity:.7;letter-spacing:.05em;text-transform:uppercase;}
  .card .value{font-size:34px;font-weight:700;margin-top:8px;font-variant-numeric:tabular-nums;}
  .card .unit{font-size:14px;opacity:.6;margin-left:4px;}
  .panel{background:var(--card);border-radius:12px;padding:18px;}
  .panel h2{font-size:16px;margin:0 0 12px;}
  table{width:100%;border-collapse:collapse;font-size:14px;}
  td,th{padding:8px;text-align:left;border-bottom:1px solid #334155;}
  button{background:var(--accent);border:none;color:#0f172a;font-weight:600;padding:8px 16px;border-radius:8px;cursor:pointer;margin-right:8px;}
  button:hover{opacity:.9;}
  .status{font-size:13px;opacity:.75;}
</style>
</head>
<body>
<header>
  <h1>AeroProbe 实时监控</h1>
  <span id="conn">未连接</span>
</header>
<main>
  <div class="cards">
    <div class="card"><div class="label">功率</div><div class="value"><span id="p">--</span><span class="unit">W</span></div></div>
    <div class="card"><div class="label">踏频</div><div class="value"><span id="cad">--</span><span class="unit">rpm</span></div></div>
    <div class="card"><div class="label">车速</div><div class="value"><span id="vg">--</span><span class="unit">m/s</span></div></div>
    <div class="card"><div class="label">风速</div><div class="value"><span id="vair">--</span><span class="unit">m/s</span></div></div>
    <div class="card"><div class="label">偏航角</div><div class="value"><span id="yaw">--</span><span class="unit">°</span></div></div>
    <div class="card"><div class="label">姿态</div><div class="value"><span id="posture">--</span><span class="unit">°</span></div></div>
    <div class="card"><div class="label">空气密度</div><div class="value"><span id="rho">--</span></div></div>
    <div class="card"><div class="label">功率源</div><div class="value" style="font-size:22px"><span id="src">--</span></div></div>
  </div>

  <div class="panel">
    <h2>控制</h2>
    <button onclick="ctl('start')">开始记录</button>
    <button onclick="ctl('stop')">停止记录</button>
    <button onclick="provision()">重新配网</button>
    <span class="status" id="logstate">记录状态：--</span>
  </div>

  <div class="panel">
    <h2>历史记录</h2>
    <table><thead><tr><th>文件</th><th>大小</th><th>操作</th></tr></thead>
    <tbody id="records"><tr><td colspan="3" class="status">加载中…</td></tr></tbody></table>
  </div>
</main>

<script>
const connEl = document.getElementById('conn');
let ws = null, retryTimer = null;

function setConn(ok){
  connEl.textContent = ok ? '已连接' : '未连接';
  connEl.className = ok ? 'ok' : '';
}

function render(d){
  const fmt = (value, digits) => Number.isFinite(Number(value))
    ? Number(value).toFixed(digits) : '--';
  document.getElementById('p').textContent      = fmt(d.p, 0);
  document.getElementById('cad').textContent    = fmt(d.cad, 0);
  document.getElementById('vg').textContent     = fmt(d.vg, 2);
  document.getElementById('vair').textContent   = d.windValid ? fmt(d.vair, 2) : '--';
  document.getElementById('yaw').textContent    = d.windValid ? fmt(d.yaw, 1) : '--';
  document.getElementById('posture').textContent= fmt(d.posture, 1);
  document.getElementById('rho').textContent    = fmt(d.rho, 3);
  document.getElementById('src').textContent    = d.src;
  document.getElementById('logstate').textContent = '记录状态：' + (d.log ? '记录中' : '已停止');
}

function connect(){
  const proto = location.protocol === 'https:' ? 'wss://' : 'ws://';
  ws = new WebSocket(proto + location.host + '/ws');
  ws.onopen = () => { setConn(true); if(retryTimer){clearTimeout(retryTimer);retryTimer=null;} };
  ws.onmessage = (ev) => {
    try { render(JSON.parse(ev.data)); }
    catch(e) { document.getElementById('conn').textContent = '数据格式错误'; }
  };
  ws.onclose = () => {
    setConn(false);
    if (!document.hidden) retryTimer = setTimeout(connect, 2000);
  };
  ws.onerror = () => ws.close();
}

async function loadRecords(){
  try{
    const r = await fetch('/api/records');
    if(!r.ok) throw new Error('HTTP '+r.status);
    const list = await r.json();
    const tb = document.getElementById('records');
    tb.replaceChildren();
    if(!list.length){
      const row = tb.insertRow();
      const cell = row.insertCell();
      cell.colSpan = 3;
      cell.className = 'status';
      cell.textContent = '无记录';
      return;
    }
    list.forEach(f => {
      const row = tb.insertRow();
      row.insertCell().textContent = f.name;
      row.insertCell().textContent = (Number(f.size)/1024).toFixed(1) + ' KB';
      const action = row.insertCell();
      const button = document.createElement('button');
      button.textContent = '下载';
      button.onclick = () => downloadRecord(f.name);
      action.appendChild(button);
    });
  }catch(e){
    const tb = document.getElementById('records');
    tb.replaceChildren();
    const row = tb.insertRow();
    const cell = row.insertCell();
    cell.colSpan = 3;
    cell.className = 'status';
    cell.textContent = '加载失败：' + e.message;
  }
}

async function ctl(cmd){
  const status = document.getElementById('logstate');
  try {
    const r = await fetch('/api/control?cmd='+encodeURIComponent(cmd), {method:'POST'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    status.textContent = cmd === 'start' ? '开始记录请求已发送' : '停止记录请求已发送';
  } catch(e) {
    status.textContent = '控制失败：' + e.message;
  }
}

async function downloadRecord(name){
  try{
    const r = await fetch('/api/download?name='+encodeURIComponent(name));
    if(!r.ok) throw new Error('HTTP '+r.status);
    const url = URL.createObjectURL(await r.blob());
    const a = document.createElement('a');
    a.href = url;
    a.download = name.split('/').pop();
    a.click();
    URL.revokeObjectURL(url);
  }catch(e){
    document.getElementById('logstate').textContent = '下载失败：' + e.message;
  }
}

async function provision(){
  if(!confirm('设备将断开当前 Wi-Fi 并进入 SmartConfig 配网。继续？')) return;
  try{
    const r = await fetch('/api/wifi/provision', {method:'POST'});
    if(!r.ok) throw new Error('HTTP '+r.status);
    document.getElementById('logstate').textContent = '配网已启动，请使用 ESP-Touch App 配置';
  }catch(e){
    document.getElementById('logstate').textContent = '配网启动失败：' + e.message;
  }
}

document.addEventListener('visibilitychange', () => {
  if(!document.hidden && (!ws || ws.readyState>1)) connect();
});

connect();
loadRecords();
setInterval(loadRecords, 10000);
</script>
</body>
</html>
)HTML";
